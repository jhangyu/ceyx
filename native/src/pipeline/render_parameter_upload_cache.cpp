// render_parameter_upload_cache.cpp — C3 implementation. See
// native/include/render_parameter_upload_cache.h for the design and the plan
// references (docs/logs/2026-09-11/plan-gpu-copy-elimination.md §5).
//
// PLATFORM SHAPE: the Metal body below is guarded by
// `#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)`, exactly as
// dng_metal_context.cpp is. Every other target compiles the portable stub at
// the bottom of this file, where render_parameter_cache_for_current_lane()
// returns nullptr and every call site keeps today's behaviour. The counters
// exist on both platforms so the probe export has one definition everywhere.

#include "render_parameter_upload_cache.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "raw_persistent_device_arena.h"

namespace ceyx {
namespace {

// Process-wide counters. They are read by the debug probe from an arbitrary
// thread, so they are atomic even though each lane only increments its own.
std::atomic<uint64_t> g_uploads_performed{0};
std::atomic<uint64_t> g_cache_hits{0};

// 64-bit FNV-1a. Used ONLY as a fast reject in front of the memcmp — the
// authoritative comparison is the memcmp, so a digest collision costs one
// unnecessary memcmp and can never serve stale bytes (plan §5.1).
uint64_t fnv1a_64_digest(const unsigned char *bytes, size_t byte_length) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < byte_length; ++i) {
    hash ^= static_cast<uint64_t>(bytes[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

}  // namespace

RenderParameterLaneIdentifier render_parameter_cache_current_lane_identifier() {
  // Plan §5.5 mechanical swap, done (R2.5 item 3): C1 has landed, so the lane
  // identifier comes from the arena's single definition instead of a second
  // local derivation. Cache identity, arena identity and sticky-queue identity
  // are now the same number by construction, not merely by agreement
  // (plan §8.2 hazard 1).
  return raw_persistent_device_arena_current_lane_identifier();
}

uint64_t render_parameter_uploads_performed() {
  return g_uploads_performed.load(std::memory_order_relaxed);
}

uint64_t render_parameter_cache_hits() {
  return g_cache_hits.load(std::memory_order_relaxed);
}

}  // namespace ceyx

#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)

#include <objc/message.h>
#include <objc/runtime.h>

#include "dng_metal_context.h"

namespace ceyx {
namespace {

using ObjectiveCSendNoArgument = void *(*)(void *, SEL);
using ObjectiveCSendVoidNoArgument = void (*)(void *, SEL);
using ObjectiveCSendNewBuffer = void *(*)(void *, SEL, unsigned long,
                                          unsigned long);

// MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache == 0.
// Shared storage is valid on every Metal device (on unified memory it makes the
// "upload" a memcpy, plan §5.3); nothing here depends on that being the case.
constexpr unsigned long kMetalResourceStorageModeSharedOptions = 0;

void *allocate_metal_buffer(size_t byte_length) {
  // ensure_created, not the plain accessor: this runs on the GPU decode path,
  // where the device is about to be created anyway (see the WHY IT EXISTS note
  // in dng_metal_context.h) — R2.5 item 2.
  void *device = metal_shared_device_handle_ensure_created();
  if (!device || byte_length == 0) return nullptr;
  return reinterpret_cast<ObjectiveCSendNewBuffer>(objc_msgSend)(
      device, sel_registerName("newBufferWithLength:options:"),
      static_cast<unsigned long>(byte_length),
      kMetalResourceStorageModeSharedOptions);
}

void release_metal_buffer(void *metal_buffer) {
  if (!metal_buffer) return;
  reinterpret_cast<ObjectiveCSendVoidNoArgument>(objc_msgSend)(
      metal_buffer, sel_registerName("release"));
}

void *metal_buffer_contents(void *metal_buffer) {
  if (!metal_buffer) return nullptr;
  return reinterpret_cast<ObjectiveCSendNoArgument>(objc_msgSend)(
      metal_buffer, sel_registerName("contents"));
}

// INTENTIONALLY LEAKED, for the reason dng_metal_context.cpp's pool_lock()
// documents: Metal teardown runs from static destructors, after a
// function-local static mutex in this TU would already have been destroyed.
std::mutex &lane_map_lock() {
  static std::mutex *lock = new std::mutex();
  return *lock;
}

// DELIBERATE LEAK, TWO LAYERS (R2.5 nit): the map itself is leaked for the
// static-destruction-order reason above, and the per-lane caches it holds are
// never deleted at thread exit — a lane's cache outlives its thread and is
// reclaimed only by process exit, exactly as the arena leaks its per-lane
// device regions. pthread_t recycling is therefore harmless: a recycled
// identifier may hand a new thread a previous thread's cache, but EVERY ENTRY
// IS SERVED ONLY AFTER A CONTENT MEMCMP (see ensure_buffer_uploaded), so an
// inherited entry is either byte-identical to what the new lane wants — correct
// to reuse — or it misses and is replaced. Nothing lane-specific is inherited.
std::unordered_map<RenderParameterLaneIdentifier,
                   RenderParameterUploadCache *> &lane_map() {
  static auto *map =
      new std::unordered_map<RenderParameterLaneIdentifier,
                             RenderParameterUploadCache *>();
  return *map;
}

}  // namespace

RenderParameterUploadCache::~RenderParameterUploadCache() {
  reset_for_lane_teardown();
}

void RenderParameterUploadCache::reset_for_lane_teardown() {
  for (size_t i = 0; i < kRenderParameterBufferCount; ++i) {
    release_metal_buffer(entries_[i].metal_buffer);
    entries_[i].metal_buffer = nullptr;
    entries_[i].byte_length = 0;
    entries_[i].content_digest = 0;
    entries_[i].retained_host_copy.clear();
    entries_[i].retained_host_copy.shrink_to_fit();
  }
}

void *RenderParameterUploadCache::ensure_buffer_uploaded(
    RenderParameterBufferIdentity buffer_identity, const void *host_bytes,
    size_t byte_length) {
  const size_t index = static_cast<size_t>(buffer_identity);
  if (index >= kRenderParameterBufferCount || !host_bytes || byte_length == 0) {
    return nullptr;
  }

  CachedParameterBuffer &entry = entries_[index];
  const unsigned char *incoming =
      static_cast<const unsigned char *>(host_bytes);

  // Fast reject on the digest, authoritative decision on the memcmp against the
  // retained host copy. The retained copy is what makes this safe across
  // decodes: the caller's RenderParams local goes out of scope between them
  // (plan §5.5), so comparing against the caller's pointer would be a
  // use-after-free, and comparing against MTLBuffer contents would trust bytes
  // the GPU may legitimately never have received.
  //
  // A HIT HANDS BACK A BUFFER THE GPU MAY STILL BE READING, and that is fine:
  // the hit path writes nothing. Only the miss path below writes, and it writes
  // only into a buffer allocated in that same miss.
  if (entry.metal_buffer != nullptr && entry.byte_length == byte_length &&
      entry.retained_host_copy.size() == byte_length) {
    const uint64_t incoming_digest = fnv1a_64_digest(incoming, byte_length);
    if (incoming_digest == entry.content_digest &&
        std::memcmp(entry.retained_host_copy.data(), incoming, byte_length) ==
            0) {
      g_cache_hits.fetch_add(1, std::memory_order_relaxed);
      return entry.metal_buffer;
    }
  }

  // MISS — ALWAYS A FRESH BUFFER, NEVER AN IN-PLACE REWRITE (R1 review
  // should-fix #2; invariant stated in full at the definition site, see
  // WRITE-ONCE DEVICE BUFFERS in render_parameter_upload_cache.h).
  //
  // Rewriting entry.metal_buffer here would race the GPU on the failure path: a
  // Stage4 that returned failure committed its command buffer without waiting
  // for completion, so the previous decode's kernel may still be reading these
  // parameter buffers when this decode misses. Allocating a fresh buffer cannot
  // race — a committed command buffer retains every resource it references
  // until completion, so the release below drops only this cache's reference
  // and never frees or mutates bytes still in use.
  void *replacement = allocate_metal_buffer(byte_length);
  if (!replacement) {
    // No device buffer: leave the entry exactly as it was rather than
    // half-updated, and let the caller take today's upload path.
    return nullptr;
  }
  void *destination = metal_buffer_contents(replacement);
  if (!destination) {
    // Same rule: the entry keeps its previous, still-valid contents, and the
    // buffer we could not populate is released rather than published.
    release_metal_buffer(replacement);
    return nullptr;
  }
  std::memcpy(destination, incoming, byte_length);

  // Publish only after the bytes are in place, so a caller can never observe a
  // buffer that the entry's digest/host copy does not describe.
  release_metal_buffer(entry.metal_buffer);
  entry.metal_buffer = replacement;
  entry.byte_length = byte_length;
  entry.content_digest = fnv1a_64_digest(incoming, byte_length);
  entry.retained_host_copy.assign(incoming, incoming + byte_length);

  g_uploads_performed.fetch_add(1, std::memory_order_relaxed);
  return entry.metal_buffer;
}

RenderParameterUploadCache *render_parameter_cache_for_current_lane() {
  // No Metal device means no cache; nullptr is a legal answer everywhere.
  //
  // R2.5 item 2 (first-decode gap, same root cause as the arena's AC1 defect):
  // the device is created lazily inside the first kernel dispatch, i.e. LATER
  // in the decode than this gate. Gating on the plain accessor therefore
  // answered "no cache" on a lane's very FIRST decode, so that decode uploaded
  // all twelve parameters uncached — invisible to AC5, which measures
  // post-warmup deltas, but one whole upload set per lane. ensure_created()
  // does what this decode was going to do moments later anyway.
  if (metal_shared_device_handle_ensure_created() == nullptr) return nullptr;

  const RenderParameterLaneIdentifier lane =
      render_parameter_cache_current_lane_identifier();

  // The lock covers map lookup and insertion only, and is released before the
  // caller does any Metal work — the rule dng_metal_context.cpp:42-45 states
  // and plan §8.2 hazard 4 binds. The returned cache is touched only by its own
  // lane, so it needs no lock of its own.
  std::lock_guard<std::mutex> guard(lane_map_lock());
  auto &map = lane_map();
  auto found = map.find(lane);
  if (found != map.end()) return found->second;

  auto *created = new (std::nothrow) RenderParameterUploadCache();
  if (!created) return nullptr;
  map[lane] = created;
  return created;
}

void render_parameter_cache_release_all_lanes() {
  std::lock_guard<std::mutex> guard(lane_map_lock());
  auto &map = lane_map();
  for (auto &entry : map) {
    delete entry.second;
  }
  map.clear();
}

}  // namespace ceyx

#else  // portable stub

namespace ceyx {

RenderParameterUploadCache::~RenderParameterUploadCache() = default;

void RenderParameterUploadCache::reset_for_lane_teardown() {}

void *RenderParameterUploadCache::ensure_buffer_uploaded(
    RenderParameterBufferIdentity, const void *, size_t) {
  return nullptr;
}

// Non-Metal targets have no MTLBuffer to cache, so there is nothing to hand
// back: every caller keeps today's behaviour verbatim.
RenderParameterUploadCache *render_parameter_cache_for_current_lane() {
  return nullptr;
}

void render_parameter_cache_release_all_lanes() {}

}  // namespace ceyx

#endif  // __APPLE__ && !DNG_FORCE_VULKAN
