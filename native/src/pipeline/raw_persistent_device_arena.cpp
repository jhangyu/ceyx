// raw_persistent_device_arena.cpp — C1 implementation. See
// native/include/raw_persistent_device_arena.h for the design and the plan
// references (docs/logs/2026-09-11/plan-gpu-copy-elimination.md §2.1-§2.2,
// §3.1-§3.4).
//
// PLATFORM SHAPE: the Metal body below is guarded by
// `#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)`, exactly as
// dng_metal_context.cpp and render_parameter_upload_cache.cpp are. Every other
// target compiles the portable stub at the bottom, where
// raw_persistent_device_arena_for_current_lane() returns nullptr and every call
// site keeps today's behaviour verbatim — which is also the universal failure
// behaviour (plan §3.4: no arena is always correct, only slower). The counters
// and the lane-identity derivation exist on both platforms so the probe export
// has one definition everywhere.
//
// REUSE SAFETY (plan §8.2 hazard 2), recorded at the definition site as the
// plan requires and not only in the document: reusing a region for frame N+1
// before frame N's command buffer completes would corrupt frame N. That
// handshake exists implicitly today because the shared Stage4 call blocks until
// the GPU command completes (raw_gpu_pipeline.cpp:897-902). ANY future change
// making Stage4 non-blocking invalidates arena reuse.

#include "raw_persistent_device_arena.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

// Lane identity needs the platform's own thread-identity call. POSIX has
// pthread_self(); Windows (clang-cl) has no <pthread.h> at all, so it uses
// GetCurrentThreadId(). See raw_persistent_device_arena_current_lane_identifier()
// below for why the two derivations are interchangeable here.
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "dng_pipeline_config.h"

// The lane ceiling must be the slot machinery's own absolute cap, not a second
// number that can drift away from it (plan §2.1).
static_assert(ceyx::kRawDeviceArenaMaximumLaneCount ==
                  PipelineConfig::kAbsoluteMaxDecodeSlots,
              "kRawDeviceArenaMaximumLaneCount must equal "
              "PipelineConfig::kAbsoluteMaxDecodeSlots");

namespace ceyx {
namespace {

// Process-wide counters. Read by the debug probe from an arbitrary thread, so
// they are atomic even though each lane only increments its own work.
std::atomic<uint64_t> g_allocation_count{0};
std::atomic<uint64_t> g_growth_reallocation_count{0};
std::atomic<uint64_t> g_binding_count{0};
std::atomic<uint64_t> g_resident_device_bytes{0};
std::atomic<size_t> g_live_lane_count{0};

// 0 means "never configured": the budget then falls back to the absolute
// ceiling. Relaxed, exactly like g_configured_slots (dng_pipeline.cpp:421-426):
// a lane reading a stale budget for one decode gets a correct-but-different
// arena decision, never an incorrect decode.
std::atomic<size_t> g_configured_lane_count{0};

// [[maybe_unused]]: these three helpers serve the Metal body only; the portable
// stub below compiles without them.
// UNCONFIGURED DEFAULT (R2 N2, lead ruling docs/logs/2026-09-11/
// gpu-copy-elimination-execution-contract.md "Rulings during execution"):
// when nothing has called raw_persistent_device_arena_configure_lane_count(),
// this deliberately returns the absolute ceiling rather than some smaller
// number. That is safe because arenas are lazily allocated per ACTUALLY
// DECODING thread (raw_persistent_device_arena_for_current_lane() only
// creates one on that thread's first arena-eligible decode) — so resident
// bytes scale with real concurrency, never with this budget. The ceiling
// only bounds how many *distinct* lanes may hold an arena at once; it is not
// a pre-allocation and an unconfigured process at low concurrency pays
// nothing for the lanes it never uses.
[[maybe_unused]] size_t lane_count_budget() {
  const size_t configured = g_configured_lane_count.load(std::memory_order_relaxed);
  if (configured == 0 || configured > kRawDeviceArenaMaximumLaneCount) {
    return kRawDeviceArenaMaximumLaneCount;
  }
  return configured;
}

[[maybe_unused]] const char *region_name(RawDeviceArenaRegion region) {
  switch (region) {
    case RawDeviceArenaRegion::kSourceMosaicRegion:
      return "source_mosaic";
    case RawDeviceArenaRegion::kStageThreeInterleavedRgb16Region:
      return "stage_three_rgb16";
    case RawDeviceArenaRegion::kDestinationRgba8Region:
      return "destination_rgba8";
  }
  return "unknown";
}

// Pre-registered line format, plan §2.6. Deliberately unconditional (no env
// var): every event it reports — allocation, growth, binding failure, release —
// is rare by construction, and AC1's "gate forced off" style evidence depends on
// the lines being readable without opting in. Steady-state decodes emit nothing.
[[maybe_unused]] void log_arena_event(const char *event, RawDecodeLaneIdentifier lane_identifier,
                     RawDeviceArenaRegion region, size_t byte_count) {
  std::fprintf(stderr,
               "[RawPersistentDeviceArena] event=%s lane_identifier=0x%llx "
               "region=%s byte_count=%zu allocation_count=%llu "
               "growth_reallocation_count=%llu\n",
               event,
               static_cast<unsigned long long>(lane_identifier),
               region_name(region), byte_count,
               static_cast<unsigned long long>(
                   g_allocation_count.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(
                   g_growth_reallocation_count.load(std::memory_order_relaxed)));
  std::fflush(stderr);
}

}  // namespace

RawDecodeLaneIdentifier raw_persistent_device_arena_current_lane_identifier() {
  // On Apple this must stay VALUE-IDENTICAL to dng_metal_context.cpp:475/541's
  // sticky-queue key (reinterpret_cast<uintptr_t>(pthread_self())), so arena
  // identity and queue identity cannot disagree (plan §8.2 hazard 1). The C3
  // render-parameter cache derives its lane key by calling this function.
  //
  // PLATFORM SHAPE: reinterpret_cast is only legal here when pthread_t is a
  // pointer type (Apple, glibc). On bionic pthread_t is `long`, an
  // integral-to-integral reinterpret_cast, which is ill-formed; on Windows
  // there is no pthread_t at all. So:
  //   - Windows: the OS thread id, an integer already.
  //   - Everything else: the object representation of pthread_self() copied
  //     into the identifier. For a pointer-typed pthread_t that is bit-for-bit
  //     the same value the reinterpret_cast produced, which is what preserves
  //     the Apple invariant above; for an integral pthread_t it is the value
  //     itself. Either way the only property the arena needs holds: distinct
  //     live threads get distinct identifiers.
#if defined(_WIN32)
  return static_cast<RawDecodeLaneIdentifier>(::GetCurrentThreadId());
#else
  pthread_t self = pthread_self();
  static_assert(sizeof(self) <= sizeof(RawDecodeLaneIdentifier),
                "pthread_t must fit in RawDecodeLaneIdentifier");
  RawDecodeLaneIdentifier identifier = 0;
  std::memcpy(&identifier, &self, sizeof(self));
  return identifier;
#endif
}

uint64_t raw_persistent_device_arena_allocation_count() {
  return g_allocation_count.load(std::memory_order_relaxed);
}

uint64_t raw_persistent_device_arena_growth_reallocation_count() {
  return g_growth_reallocation_count.load(std::memory_order_relaxed);
}

uint64_t raw_persistent_device_arena_binding_count() {
  return g_binding_count.load(std::memory_order_relaxed);
}

uint64_t raw_persistent_device_arena_resident_device_bytes() {
  return g_resident_device_bytes.load(std::memory_order_relaxed);
}

size_t raw_persistent_device_arena_live_lane_count() {
  return g_live_lane_count.load(std::memory_order_relaxed);
}

void raw_persistent_device_arena_configure_lane_count(size_t lane_count) {
  // Records the budget only; nothing is pre-allocated, because a thread
  // identity does not exist before the thread runs (plan §2.2). A lane whose
  // index now sits above the budget releases its OWN regions at the start of
  // its next decode and then takes today's path — lanes are never destroyed
  // from another thread, mirroring DecodeSlotPool::trim_free_surplus_locked
  // (decode_context.h:349-360). Cannot fail (plan §3.4 row 1).
  size_t clamped = lane_count < 1 ? size_t{1} : lane_count;
  if (clamped > kRawDeviceArenaMaximumLaneCount) {
    clamped = kRawDeviceArenaMaximumLaneCount;
  }
  g_configured_lane_count.store(clamped, std::memory_order_relaxed);
}

RawDeviceArenaRegionBinding::RawDeviceArenaRegionBinding(
    RawPersistentDeviceArena *arena, halide_buffer_t *halide_buffer,
    RawDeviceArenaRegion region, size_t required_byte_count)
    : arena_(arena), halide_buffer_(halide_buffer) {
  // A null arena is the normal no-arena answer, not an error: the binding
  // simply reports false and the caller uses the buffer as it would today.
  if (arena_ == nullptr || halide_buffer_ == nullptr) return;
  bound_ = arena_->bind_region(halide_buffer_, region, required_byte_count);
}

RawDeviceArenaRegionBinding::~RawDeviceArenaRegionBinding() {
  if (!bound_) return;
  // Detach, never device-free (invariant I-D). This runs on every exit path of
  // the owning scope, which is the whole point of the RAII form (plan §8.2
  // hazard 5).
  arena_->detach_region(halide_buffer_);
}

}  // namespace ceyx

#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)

#include <objc/message.h>
#include <objc/runtime.h>

#include "HalideRuntime.h"
#include "HalideRuntimeMetal.h"

#include "dng_metal_context.h"

namespace ceyx {
namespace {

using ObjectiveCSendNoArgument = void *(*)(void *, SEL);
using ObjectiveCSendVoidNoArgument = void (*)(void *, SEL);
using ObjectiveCSendNewBuffer = void *(*)(void *, SEL, unsigned long,
                                          unsigned long);

// MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache == 0.
// Shared storage is what makes the wrapped-buffer copies no-ops on unified
// memory (plan §3.1 "Storage mode"); it is valid on every Metal device, so
// nothing here depends on the device being unified.
constexpr unsigned long kMetalResourceStorageModeSharedOptions = 0;

void *allocate_metal_buffer(size_t byte_count) {
  void *device = metal_shared_device_handle();
  if (!device || byte_count == 0) return nullptr;
  return reinterpret_cast<ObjectiveCSendNewBuffer>(objc_msgSend)(
      device, sel_registerName("newBufferWithLength:options:"),
      static_cast<unsigned long>(byte_count),
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

size_t round_up_to_arena_alignment(size_t byte_count) {
  const size_t alignment = kRawDeviceArenaAlignmentBytes;
  const size_t remainder = byte_count % alignment;
  if (remainder == 0) return byte_count;
  return byte_count + (alignment - remainder);
}

// INTENTIONALLY LEAKED, for the reason dng_pipeline.cpp:236-243 and
// dng_metal_context.cpp's pool_lock() document: Metal teardown runs from static
// destructors / atexit, after a function-local static mutex in this TU would
// already have been destroyed (plan §2.2 "Teardown: deliberate leak").
std::mutex &lane_map_lock() {
  static std::mutex *lock = new std::mutex();
  return *lock;
}

struct LaneRegistryEntry {
  RawPersistentDeviceArena *arena = nullptr;
  size_t lane_index = 0;  // stable for the process lifetime, see below
};

std::unordered_map<RawDecodeLaneIdentifier, LaneRegistryEntry> &lane_map() {
  static auto *map =
      new std::unordered_map<RawDecodeLaneIdentifier, LaneRegistryEntry>();
  return *map;
}

}  // namespace

RawPersistentDeviceArena::RawPersistentDeviceArena(
    RawDecodeLaneIdentifier lane_identifier)
    : lane_identifier_(lane_identifier) {}

RawPersistentDeviceArena::~RawPersistentDeviceArena() { release_all_regions(); }

bool RawPersistentDeviceArena::bind_region(halide_buffer_t *halide_buffer,
                                           RawDeviceArenaRegion region,
                                           size_t required_byte_count) {
  if (halide_buffer == nullptr || required_byte_count == 0) return false;
  const size_t index = static_cast<size_t>(region);
  if (index >= kRawDeviceArenaRegionCount) return false;

  ArenaRegionStorage &storage = regions_[index];
  if (storage.unavailable) return false;

  // halide_metal_wrap_buffer REQUIRES a null device field
  // (HalideRuntimeMetal.h:41-47). A buffer that already carries device memory is
  // not ours to rebind; the caller keeps today's path.
  if (halide_buffer->device != 0) return false;

  if (storage.metal_buffer == nullptr ||
      storage.byte_count < required_byte_count) {
    const bool is_growth = storage.metal_buffer != nullptr;

    // §3.3 step 1: a region carries at most one live binding, because a lane is
    // single-threaded by construction. Drop it before the memory under it goes
    // away.
    if (storage.bound_halide_buffer != nullptr) {
      halide_metal_detach_buffer(nullptr, storage.bound_halide_buffer);
      storage.bound_halide_buffer = nullptr;
      live_binding_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    const size_t allocation_byte_count =
        round_up_to_arena_alignment(required_byte_count);
    void *replacement = allocate_metal_buffer(allocation_byte_count);
    if (replacement == nullptr) {
      // Allocation failure degrades to no arena for this region, never to a
      // decode error (plan §3.4).
      release_metal_buffer(storage.metal_buffer);
      g_resident_device_bytes.fetch_sub(storage.byte_count,
                                       std::memory_order_relaxed);
      storage.metal_buffer = nullptr;
      storage.byte_count = 0;
      storage.unavailable = true;
      log_arena_event("binding_failure", lane_identifier_, region,
                      required_byte_count);
      return false;
    }

    // Alignment is ASSERTED, not assumed (plan §3.1): C2's caller-destination
    // wrapping depends on the same page-alignment property, so the two are
    // checked by one rule. A misaligned buffer is treated as a failure rather
    // than used, because silently proceeding would make that later check
    // unreliable.
    void *contents = metal_buffer_contents(replacement);
    if (contents == nullptr ||
        (reinterpret_cast<uintptr_t>(contents) %
         kRawDeviceArenaAlignmentBytes) != 0) {
      release_metal_buffer(replacement);
      release_metal_buffer(storage.metal_buffer);
      g_resident_device_bytes.fetch_sub(storage.byte_count,
                                       std::memory_order_relaxed);
      storage.metal_buffer = nullptr;
      storage.byte_count = 0;
      storage.unavailable = true;
      log_arena_event("binding_failure", lane_identifier_, region,
                      required_byte_count);
      return false;
    }

    release_metal_buffer(storage.metal_buffer);
    g_resident_device_bytes.fetch_sub(storage.byte_count,
                                     std::memory_order_relaxed);
    storage.metal_buffer = replacement;
    storage.byte_count = allocation_byte_count;
    g_resident_device_bytes.fetch_add(allocation_byte_count,
                                     std::memory_order_relaxed);

    // A growth reallocation is counted SEPARATELY and never folded into the
    // allocation count, so AC1 stays judgeable (plan §3.3).
    if (is_growth) {
      g_growth_reallocation_count.fetch_add(1, std::memory_order_relaxed);
      log_arena_event("growth_reallocation", lane_identifier_, region,
                      allocation_byte_count);
    } else {
      g_allocation_count.fetch_add(1, std::memory_order_relaxed);
      log_arena_event("allocation", lane_identifier_, region,
                      allocation_byte_count);
    }
  }

  // R2.5 review S-2: the growth branch above (:276-280) already detaches and
  // decrements when it drops the old binding to reallocate. This is the
  // symmetric case for a rebind that reuses the SAME region without growing
  // it (sufficient capacity already, or the wrap below is being retried) — a
  // still-live binding here must be detached and decremented the same way,
  // otherwise live_binding_count_ drifts permanently positive and
  // raw_persistent_device_arena_release_all_lanes() refuses forever.
  if (storage.bound_halide_buffer != nullptr &&
      storage.bound_halide_buffer != halide_buffer) {
    halide_metal_detach_buffer(nullptr, storage.bound_halide_buffer);
    storage.bound_halide_buffer = nullptr;
    live_binding_count_.fetch_sub(1, std::memory_order_relaxed);
  }

  if (halide_metal_wrap_buffer(
          nullptr, halide_buffer,
          reinterpret_cast<uint64_t>(storage.metal_buffer)) != 0) {
    // Wrap failure leaves halide_buffer untouched (the runtime does not modify
    // it on failure) and the region intact for the next attempt; the call site
    // proceeds un-wrapped, exactly as today.
    log_arena_event("binding_failure", lane_identifier_, region,
                    required_byte_count);
    return false;
  }

  storage.bound_halide_buffer = halide_buffer;
  live_binding_count_.fetch_add(1, std::memory_order_relaxed);
  g_binding_count.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void *RawPersistentDeviceArena::ensure_region_host_pointer(
    RawDeviceArenaRegion region, size_t required_byte_count) {
  if (required_byte_count == 0) return nullptr;
  const size_t index = static_cast<size_t>(region);
  if (index >= kRawDeviceArenaRegionCount) return nullptr;

  ArenaRegionStorage &storage = regions_[index];
  if (storage.unavailable) return nullptr;

  if (storage.metal_buffer == nullptr ||
      storage.byte_count < required_byte_count) {
    const bool is_growth = storage.metal_buffer != nullptr;

    // Same rule as bind_region's growth branch: a region carries at most one
    // live binding, and it must be dropped before the memory under it goes
    // away.
    if (storage.bound_halide_buffer != nullptr) {
      halide_metal_detach_buffer(nullptr, storage.bound_halide_buffer);
      storage.bound_halide_buffer = nullptr;
      live_binding_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    const size_t allocation_byte_count =
        round_up_to_arena_alignment(required_byte_count);
    void *replacement = allocate_metal_buffer(allocation_byte_count);
    if (replacement == nullptr) {
      release_metal_buffer(storage.metal_buffer);
      g_resident_device_bytes.fetch_sub(storage.byte_count,
                                       std::memory_order_relaxed);
      storage.metal_buffer = nullptr;
      storage.byte_count = 0;
      storage.unavailable = true;
      log_arena_event("binding_failure", lane_identifier_, region,
                      required_byte_count);
      return nullptr;
    }

    // Same alignment assertion as bind_region (plan §3.1): C2's
    // caller-destination wrapping depends on the identical property, so both
    // are checked by the one rule.
    void *contents = metal_buffer_contents(replacement);
    if (contents == nullptr ||
        (reinterpret_cast<uintptr_t>(contents) %
         kRawDeviceArenaAlignmentBytes) != 0) {
      release_metal_buffer(replacement);
      release_metal_buffer(storage.metal_buffer);
      g_resident_device_bytes.fetch_sub(storage.byte_count,
                                       std::memory_order_relaxed);
      storage.metal_buffer = nullptr;
      storage.byte_count = 0;
      storage.unavailable = true;
      log_arena_event("binding_failure", lane_identifier_, region,
                      required_byte_count);
      return nullptr;
    }

    release_metal_buffer(storage.metal_buffer);
    g_resident_device_bytes.fetch_sub(storage.byte_count,
                                     std::memory_order_relaxed);
    storage.metal_buffer = replacement;
    storage.byte_count = allocation_byte_count;
    g_resident_device_bytes.fetch_add(allocation_byte_count,
                                     std::memory_order_relaxed);

    if (is_growth) {
      g_growth_reallocation_count.fetch_add(1, std::memory_order_relaxed);
      log_arena_event("growth_reallocation", lane_identifier_, region,
                      allocation_byte_count);
    } else {
      g_allocation_count.fetch_add(1, std::memory_order_relaxed);
      log_arena_event("allocation", lane_identifier_, region,
                      allocation_byte_count);
    }
  }

  return metal_buffer_contents(storage.metal_buffer);
}

void RawPersistentDeviceArena::detach_region(halide_buffer_t *halide_buffer) {
  if (halide_buffer == nullptr) return;
  for (size_t i = 0; i < kRawDeviceArenaRegionCount; ++i) {
    if (regions_[i].bound_halide_buffer != halide_buffer) continue;
    // halide_metal_detach_buffer drops the binding and leaves the MTLBuffer
    // alive; halide_device_free here would destroy the arena region
    // (invariant I-D).
    halide_metal_detach_buffer(nullptr, halide_buffer);
    regions_[i].bound_halide_buffer = nullptr;
    live_binding_count_.fetch_sub(1, std::memory_order_relaxed);
    return;
  }
}

bool RawPersistentDeviceArena::owns_buffer(
    const halide_buffer_t *halide_buffer) const {
  if (halide_buffer == nullptr) return false;
  for (size_t i = 0; i < kRawDeviceArenaRegionCount; ++i) {
    if (regions_[i].bound_halide_buffer == halide_buffer) return true;
  }
  return false;
}

bool RawPersistentDeviceArena::has_live_binding() const {
  return live_binding_count_.load(std::memory_order_relaxed) > 0;
}

void RawPersistentDeviceArena::release_all_regions() {
  for (size_t i = 0; i < kRawDeviceArenaRegionCount; ++i) {
    ArenaRegionStorage &storage = regions_[i];
    if (storage.bound_halide_buffer != nullptr) {
      halide_metal_detach_buffer(nullptr, storage.bound_halide_buffer);
      storage.bound_halide_buffer = nullptr;
      live_binding_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    if (storage.metal_buffer != nullptr) {
      release_metal_buffer(storage.metal_buffer);
      g_resident_device_bytes.fetch_sub(storage.byte_count,
                                       std::memory_order_relaxed);
      log_arena_event("release", lane_identifier_,
                      static_cast<RawDeviceArenaRegion>(i), storage.byte_count);
    }
    storage.metal_buffer = nullptr;
    storage.byte_count = 0;
    // A released region may be allocated again on a later bind; the
    // unavailable latch exists to stop retrying a FAILING allocation, not to
    // retire a healthy region.
    storage.unavailable = false;
  }
}

uint64_t RawPersistentDeviceArena::resident_device_bytes() const {
  uint64_t total = 0;
  for (size_t i = 0; i < kRawDeviceArenaRegionCount; ++i) {
    total += static_cast<uint64_t>(regions_[i].byte_count);
  }
  return total;
}

RawPersistentDeviceArena *raw_persistent_device_arena_for_current_lane() {
  // Device acquisition CREATES the device when absent (R2 AC1 root cause). The
  // plain metal_shared_device_handle() cannot be used here: the device is
  // created lazily inside the first kernel dispatch, which happens LATER in the
  // decode than this first-touch call, so on a lane's very first decode the
  // plain accessor answers nullptr and that decode silently runs unarened. The
  // observable symptom was warmup allocating nothing, the next (smaller) frame
  // sizing the regions, and the largest frame then growing all three —
  // allocation=3 with growth=3 where growth must be 0 after warmup.
  //
  // This is only reached on the GPU decode path, where device creation is
  // imminent and intended; see dng_metal_context.h for why that keeps the plain
  // accessor's no-side-effect contract intact. A genuinely Metal-less system
  // still yields nullptr here, which is the normal no-arena answer.
  if (metal_shared_device_handle_ensure_created() == nullptr) return nullptr;

  const RawDecodeLaneIdentifier lane =
      raw_persistent_device_arena_current_lane_identifier();
  const size_t budget = lane_count_budget();

  RawPersistentDeviceArena *arena = nullptr;
  bool surplus_lane = false;

  {
    // The lock covers map lookup, insertion and bookkeeping ONLY, and is
    // released before the caller does any Metal work — invariant I-B, the rule
    // dng_metal_context.cpp:42-45 states and plan §8.2 hazard 4 binds. The
    // returned arena is touched only by its own lane, so it needs no lock of
    // its own (invariant I-A: one lane key, one owner, raw pointer handed only
    // to the calling thread's own entry).
    std::lock_guard<std::mutex> guard(lane_map_lock());
    auto &map = lane_map();
    auto found = map.find(lane);
    if (found != map.end()) {
      arena = found->second.arena;
      // lane_index is assigned once and never reused, so a later shrink of the
      // budget deterministically marks the same lanes surplus, and a later
      // growth of the budget lets them back in.
      surplus_lane = found->second.lane_index >= budget;
    } else {
      const size_t next_index = map.size();
      if (next_index >= budget) {
        // Beyond the ceiling: this lane gets no arena and takes today's
        // behaviour, which is always correct and merely slower (plan §2.1).
        return nullptr;
      }
      auto *created = new (std::nothrow) RawPersistentDeviceArena(lane);
      if (created == nullptr) return nullptr;
      map[lane] = LaneRegistryEntry{created, next_index};
      g_live_lane_count.store(map.size(), std::memory_order_relaxed);
      return created;
    }
  }

  if (surplus_lane) {
    // A lane marked surplus releases its OWN buffers at the start of its next
    // decode, before any binding, and then takes today's path (plan §2.2
    // "Shrink on a mid-session lane-width change"). The release runs with no
    // lock held and only ever on the owning thread.
    arena->release_all_regions();
    return nullptr;
  }
  return arena;
}

void raw_persistent_device_arena_release_all_lanes() {
  // Test/reset entry. Every lane's regions are released here rather than by
  // their owning threads, which is safe only because callers use this between
  // decodes to establish a known baseline (plan §2.2); it is not a concurrent
  // teardown path.
  //
  // QUIESCENCE GUARD (R2 N1): deleting another thread's arena while that
  // thread still holds a live RawDeviceArenaRegionBinding would dangle the
  // binding's pointer (its destructor would call detach_region on freed
  // memory). Rather than resting on this comment alone, refuse the whole
  // call mechanically whenever any lane reports has_live_binding(); the
  // quiescent case (no in-flight decode) keeps exactly today's behaviour.
  std::lock_guard<std::mutex> guard(lane_map_lock());
  auto &map = lane_map();
  for (const auto &entry : map) {
    if (entry.second.arena != nullptr && entry.second.arena->has_live_binding()) {
      std::fprintf(stderr,
                   "[RawPersistentDeviceArena] event=release_all_lanes_refused "
                   "reason=live_binding lane_identifier=0x%llx\n",
                   static_cast<unsigned long long>(entry.first));
      std::fflush(stderr);
      return;
    }
  }
  for (auto &entry : map) {
    delete entry.second.arena;  // destructor releases the three regions
  }
  map.clear();
  g_live_lane_count.store(0, std::memory_order_relaxed);
}

}  // namespace ceyx

#else  // portable stub

namespace ceyx {

RawPersistentDeviceArena::RawPersistentDeviceArena(
    RawDecodeLaneIdentifier lane_identifier)
    : lane_identifier_(lane_identifier) {
  // The lane identifier only feeds the Metal body's log lines; reading it here
  // keeps the field from looking dead on non-Metal targets.
  (void)lane_identifier_;
}

RawPersistentDeviceArena::~RawPersistentDeviceArena() = default;

// No halide_metal_wrap_buffer exists off Metal (plan §2.5), so there is nothing
// to bind: every caller keeps today's behaviour verbatim.
bool RawPersistentDeviceArena::bind_region(halide_buffer_t *, RawDeviceArenaRegion,
                                           size_t) {
  return false;
}

void RawPersistentDeviceArena::detach_region(halide_buffer_t *) {}

// No MTLBuffer / shared-storage memory exists off Metal (plan §2.5): every
// caller keeps today's behaviour verbatim.
void *RawPersistentDeviceArena::ensure_region_host_pointer(
    RawDeviceArenaRegion, size_t) {
  return nullptr;
}

bool RawPersistentDeviceArena::owns_buffer(const halide_buffer_t *) const {
  return false;
}

void RawPersistentDeviceArena::release_all_regions() {}

bool RawPersistentDeviceArena::has_live_binding() const { return false; }

uint64_t RawPersistentDeviceArena::resident_device_bytes() const { return 0; }

RawPersistentDeviceArena *raw_persistent_device_arena_for_current_lane() {
  return nullptr;
}

void raw_persistent_device_arena_release_all_lanes() {}

}  // namespace ceyx

#endif  // __APPLE__ && !DNG_FORCE_VULKAN
