// raw_persistent_device_arena.h — C1 of the GPU copy-elimination campaign
// (plan docs/logs/2026-09-11/plan-gpu-copy-elimination.md §2.1-§2.2, §2.6,
// §3.1-§3.4, R2-T1).
//
// WHY: every generic-RAW decode implicitly device-mallocs and device-frees the
// same three buffers — the uint16 mosaic source, the interleaved RGB16 Stage3
// intermediate and the RGBA8 destination (plan §1.2's inventory). The arena
// holds those three device regions per decode lane for the process lifetime, so
// a steady-state decode performs zero new device allocations. That delta is
// exactly what acceptance criterion AC1 measures, through
// raw_persistent_device_arena_allocation_count() below.
//
// LANE SCOPE (plan §2.1 Candidate A, §8.2 hazard 1): one arena per lane, keyed
// on reinterpret_cast<uintptr_t>(pthread_self()) — the same derivation
// dng_metal_context.cpp:220 uses for the sticky Metal queue key, and the same
// one the C3 render-parameter cache already uses. Because the queue is bound to
// the thread and never rebound, keying the arena on the thread makes
// arena-to-queue a 1:1 correspondence BY CONSTRUCTION rather than by review: a
// region can never be written by a kernel on one queue while being read by a
// copy on another. A lane is single-threaded, so the arena itself needs no
// synchronisation; only the lane map does, and that lock is never held across
// GPU work (invariant I-B, plan §8.2 hazard 4).
//
// REUSE SAFETY DEPENDS ON A BLOCKING STAGE4 (plan §8.2 hazard 2): reusing a
// region for frame N+1 before frame N's command buffer completes would corrupt
// frame N. Today that handshake exists implicitly — the shared Stage4 call
// blocks until the GPU command completes (raw_gpu_pipeline.cpp:897-902), so one
// decode per lane is fully retired before the next begins. ANY future change
// that makes Stage4 non-blocking invalidates arena reuse and must add an
// explicit completion handshake here.
//
// SIZING IS ACTUAL-SIZE, NOT WORST-CASE (user ruling R-2026-09-11-2, plan
// §3.1): a region is allocated at its first bind, at exactly the byte count
// that bind needs. A later, larger frame reallocates THAT region of THAT lane
// only, counted in raw_persistent_device_arena_growth_reallocation_count() and
// never in the allocation count, so a growth event stays visible instead of
// hiding inside AC1's headline number (plan §3.3). Worst-case sizing was
// rejected because 12 bytes/pixel/lane is ~5.9GB resident at eight lanes on a
// 61 MP frame.
//
// LIFETIME (plan §2.2): the lane count is a budget published by
// dng_decode_resize_slots; nothing is pre-allocated, because a thread identity
// does not exist before the thread runs. A lane above a shrunken width releases
// its own regions at the start of its next decode — lanes are never destroyed
// from another thread. The registry (map plus mutex) is deliberately leaked at
// teardown, exactly as decodeSlotPool() (dng_pipeline.cpp:236-247) and
// dng_metal_context.cpp's pool_lock()/bindings()/queues() already are: running
// arena destructors at static-destruction time would issue Metal release calls
// with no defined ordering against Halide's own halide_metal_device_release,
// which arrives from atexit (dng_metal_context.cpp:47-51). The OS reclaims the
// device and its buffers at process exit.
//
// NULLPTR IS ALWAYS LEGAL (plan §3.4, §2.5): raw_persistent_device_arena_for_current_lane()
// returns nullptr on non-Metal targets, when no Metal device exists, when the
// lane ceiling is exceeded and on any allocation failure. Every call site must
// then take today's path verbatim. AN ARENA FAILURE IS NEVER A DECODE FAILURE;
// kRawErrAllocationFailed keeps its existing meaning (a caller-destination
// problem) and is not repurposed here.
//
// DETACH, NEVER DEVICE-FREE (invariant I-D): release goes through
// halide_metal_detach_buffer, which drops the binding and leaves the MTLBuffer
// alive. halide_device_free on a wrapped buffer would destroy the arena region.
// Likewise no set_min / translate on a wrapped buffer (invariant I-C): those
// trigger device_deallocate.

#ifndef RAW_PERSISTENT_DEVICE_ARENA_H
#define RAW_PERSISTENT_DEVICE_ARENA_H

#include <cstddef>
#include <cstdint>

struct halide_buffer_t;

namespace ceyx {

// Lane identity, derived exactly as dng_metal_context.cpp:220 derives the
// sticky-queue key: reinterpret_cast<uintptr_t>(pthread_self()).
using RawDecodeLaneIdentifier = uintptr_t;

RawDecodeLaneIdentifier raw_persistent_device_arena_current_lane_identifier();

// The three device regions of plan §3.1's table, one per entry in the §1.2
// allocation inventory.
enum class RawDeviceArenaRegion {
  kSourceMosaicRegion = 0,             // uint16 mosaic source, pixels * 2 bytes
  kStageThreeInterleavedRgb16Region = 1,  // uint16 interleaved x3, pixels * 6
  kDestinationRgba8Region = 2,         // uint8 interleaved x4, pixels * 4
};

constexpr size_t kRawDeviceArenaRegionCount = 3;

// Lane ceiling. Deliberately the same constant the slot machinery caps on
// (PipelineConfig::kAbsoluteMaxDecodeSlots); asserted equal in the
// implementation so the two cannot drift apart silently.
constexpr size_t kRawDeviceArenaMaximumLaneCount = 16;

// Apple Silicon page size. newBufferWithLength:options: already returns
// page-aligned storage, but the arena ASSERTS it rather than assuming it,
// because C2's caller-destination wrapping depends on the same property and the
// two must be checked by one rule (plan §3.1).
constexpr size_t kRawDeviceArenaAlignmentBytes = 16384;

// The per-lane owner of the three device regions. Touched only by its own lane,
// so it carries no lock of its own.
class RawPersistentDeviceArena {
 public:
  // The lane identifier is carried only so the pre-registered log lines can
  // name the lane even when the release is driven from another thread by
  // raw_persistent_device_arena_release_all_lanes().
  explicit RawPersistentDeviceArena(RawDecodeLaneIdentifier lane_identifier = 0);
  ~RawPersistentDeviceArena();

  RawPersistentDeviceArena(const RawPersistentDeviceArena &) = delete;
  RawPersistentDeviceArena &operator=(const RawPersistentDeviceArena &) = delete;

  // Wraps the region's MTLBuffer onto `halide_buffer` via
  // halide_metal_wrap_buffer, growing (reallocating) the region first when
  // `required_byte_count` exceeds its current size. Returns false on any
  // failure with `halide_buffer` left untouched, in which case the caller
  // proceeds with the un-wrapped buffer exactly as today (plan §3.4).
  bool bind_region(halide_buffer_t *halide_buffer, RawDeviceArenaRegion region,
                   size_t required_byte_count);

  // halide_metal_detach_buffer; NEVER halide_device_free (invariant I-D).
  void detach_region(halide_buffer_t *halide_buffer);

  // True when `halide_buffer` is currently bound to one of this arena's
  // regions. This is what lets the split-build explicit free at
  // dng_render_halide.cpp:1682-1684 decide mechanically rather than from a flag
  // the caller has to remember to pass.
  bool owns_buffer(const halide_buffer_t *halide_buffer) const;

  // Detaches any live binding and releases all three regions. Called when a
  // lane is marked surplus by a lane-width shrink and from
  // raw_persistent_device_arena_release_all_lanes().
  void release_all_regions();

  // Currently held device bytes across this lane's three regions.
  uint64_t resident_device_bytes() const;

 private:
  struct ArenaRegionStorage {
    void *metal_buffer = nullptr;  // retained MTLBuffer, or nullptr
    size_t byte_count = 0;         // allocated length of metal_buffer
    halide_buffer_t *bound_halide_buffer = nullptr;  // at most one, see §3.3
    bool unavailable = false;  // a prior allocation failed; stop retrying
  };

  ArenaRegionStorage regions_[kRawDeviceArenaRegionCount];
  RawDecodeLaneIdentifier lane_identifier_ = 0;
};

// The calling lane's arena, created on first use. nullptr is ALWAYS a legal
// answer and every call site must then take today's path (see header comment).
RawPersistentDeviceArena *raw_persistent_device_arena_for_current_lane();

// RAII wrapper over bind_region/detach_region, so an early return inside a
// decode branch cannot leak a binding (plan §3.2, §8.2 hazard 5). Constructing
// it with a null arena is legal and yields a binding that reports false.
class RawDeviceArenaRegionBinding {
 public:
  RawDeviceArenaRegionBinding(RawPersistentDeviceArena *arena,
                              halide_buffer_t *halide_buffer,
                              RawDeviceArenaRegion region,
                              size_t required_byte_count);
  ~RawDeviceArenaRegionBinding();

  RawDeviceArenaRegionBinding(const RawDeviceArenaRegionBinding &) = delete;
  RawDeviceArenaRegionBinding &operator=(const RawDeviceArenaRegionBinding &) =
      delete;

  // Whether the binding succeeded. False means the caller must use the buffer
  // exactly as it would today.
  explicit operator bool() const { return bound_; }

 private:
  RawPersistentDeviceArena *arena_ = nullptr;
  halide_buffer_t *halide_buffer_ = nullptr;
  bool bound_ = false;
};

// Records the lane-count budget. Called from dng_decode_resize_slots
// (pipeline/dng_pipeline.cpp:432), the single funnel for lane-width changes.
// Nothing is pre-allocated; on a shrink, lanes above the new width are marked
// for release at their own next quiescent point. Cannot fail.
void raw_persistent_device_arena_configure_lane_count(size_t lane_count);

// Test/reset entry: detaches and releases everything reachable, for a clean AC1
// warmup baseline. Safe to call with no lanes alive.
void raw_persistent_device_arena_release_all_lanes();

// True when `halide_buffer` is bound to any live lane's region. Used by the
// split-build free guard (plan §3.2).
bool raw_persistent_device_arena_owns_buffer(const halide_buffer_t *halide_buffer);

// Counters (plan §2.6). All process-wide totals since process start.
//
// THE AC1 COUNTER: total device allocations the arena has made, INCLUDING
// warmup. AC1 is judged on the delta across N post-warmup decodes being 0.
uint64_t raw_persistent_device_arena_allocation_count();

// Device allocations caused by a frame exceeding an existing region. Counted
// separately and NEVER folded into the allocation count, so a growth event is
// visible rather than hidden inside AC1's headline number (plan §3.3).
uint64_t raw_persistent_device_arena_growth_reallocation_count();

// Successful bind_region calls. A run where this is 0 while the arena is
// nominally enabled is the "arena silently not used" failure of plan §8; AC1
// must assert it is non-zero, otherwise a build with the arena disabled passes
// AC1 trivially.
uint64_t raw_persistent_device_arena_binding_count();

// Currently held device bytes across every live lane.
uint64_t raw_persistent_device_arena_resident_device_bytes();

size_t raw_persistent_device_arena_live_lane_count();

}  // namespace ceyx

#endif  // RAW_PERSISTENT_DEVICE_ARENA_H
