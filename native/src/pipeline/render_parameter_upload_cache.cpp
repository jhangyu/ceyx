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

#include <pthread.h>

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
  // Same derivation as dng_metal_context.cpp:220's sticky-queue key, so cache
  // identity and queue identity cannot disagree (plan §8.2 hazard 1). When C1
  // lands this becomes a call to
  // raw_persistent_device_arena_current_lane_identifier().
  return reinterpret_cast<RenderParameterLaneIdentifier>(pthread_self());
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
  void *device = metal_shared_device_handle();
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
    entries_[i].metal_buffer_capacity = 0;
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

  // Miss. Grow (or first-allocate) the device buffer when it cannot hold the
  // bytes; a shrink keeps the existing allocation and simply uses less of it.
  if (entry.metal_buffer == nullptr ||
      entry.metal_buffer_capacity < byte_length) {
    void *replacement = allocate_metal_buffer(byte_length);
    if (!replacement) {
      // No device buffer: leave the entry exactly as it was rather than
      // half-updated, and let the caller take today's upload path.
      return nullptr;
    }
    release_metal_buffer(entry.metal_buffer);
    entry.metal_buffer = replacement;
    entry.metal_buffer_capacity = byte_length;
  }

  void *destination = metal_buffer_contents(entry.metal_buffer);
  if (!destination) {
    return nullptr;
  }
  std::memcpy(destination, incoming, byte_length);

  entry.byte_length = byte_length;
  entry.content_digest = fnv1a_64_digest(incoming, byte_length);
  entry.retained_host_copy.assign(incoming, incoming + byte_length);

  g_uploads_performed.fetch_add(1, std::memory_order_relaxed);
  return entry.metal_buffer;
}

RenderParameterUploadCache *render_parameter_cache_for_current_lane() {
  // No Metal device means no cache; nullptr is a legal answer everywhere.
  if (metal_shared_device_handle() == nullptr) return nullptr;

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
