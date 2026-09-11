// render_parameter_upload_cache.h — C3 of the GPU copy-elimination campaign
// (plan docs/logs/2026-09-11/plan-gpu-copy-elimination.md §5, R1-T1).
//
// WHY: Stage4 builds twelve tiny parameter buffers per decode
// (dng_render_halide.cpp:1470-1499) and marks every one host-dirty
// (:1516-1527). Each host-dirty buffer costs one device allocation plus one
// synchronous host->device transfer on the thread's sticky Metal queue — the
// "~12 tiny param uploads per image, each a commit+wait" the campaign exists to
// remove. Ten of the twelve are byte-identical across every decode of the same
// camera (plan §5.1), so they only ever need to be uploaded once per lane.
//
// MECHANISM: one cache per decode lane, holding for each of the twelve buffers
// the retained host copy of the last uploaded bytes, the cached MTLBuffer and
// its byte length. A caller hands over the freshly built host bytes; the cache
// answers with the MTLBuffer to wrap, having copied the bytes into it only when
// they actually changed.
//
// KEY IS CONTENT, NOT INTENT (plan §5.1): the comparison is a 64-bit FNV-1a
// digest used purely as a fast reject, followed by a memcmp against the
// retained host copy. Deriving the key from the built bytes rather than from
// caller intent (camera model, settings struct) means a builder change, a
// profile change or an SDK constant change all invalidate automatically; an
// intent-derived key would serve stale uploads and produce wrong colours with
// no failing test.
//
// LANE SCOPE (plan §5.2, §8.2 hazard 1): the cache is per lane, keyed on
// reinterpret_cast<uintptr_t>(pthread_self()) — the same derivation
// dng_metal_context.cpp:220 uses for the sticky queue key, so cache identity
// and queue identity cannot disagree by construction. A lane is
// single-threaded, so the per-lane cache itself needs no synchronisation; only
// the lane map does, and that lock is never held across GPU work.
//
// C1 INDEPENDENCE (plan §5.5): this slice lands before the persistent device
// arena, so the lane identifier is computed locally here. When
// raw_persistent_device_arena_current_lane_identifier() exists, the single call
// inside render_parameter_cache_for_current_lane() is the only place to change;
// the two agree by construction because the derivation is fixed, not just the
// name.
//
// NULLPTR IS ALWAYS LEGAL: render_parameter_cache_for_current_lane() returns
// nullptr on non-Metal targets, when no Metal device exists, or on allocation
// failure. Every call site must then take today's behaviour verbatim
// (construct, set_host_dirty(), let Halide upload).

#ifndef RENDER_PARAMETER_UPLOAD_CACHE_H
#define RENDER_PARAMETER_UPLOAD_CACHE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ceyx {

// Lane identity, derived exactly as dng_metal_context.cpp:220 derives the
// sticky-queue key. Replaced by W1's RawDecodeLaneIdentifier when C1 lands.
using RenderParameterLaneIdentifier = uintptr_t;

RenderParameterLaneIdentifier render_parameter_cache_current_lane_identifier();

// The twelve Stage4 parameter buffers of plan §5.0's table, in that table's
// order. The index a caller passes to ensure_buffer_uploaded() is the buffer's
// identity: it must be stable across decodes, because it is what makes "the
// previous bytes of THIS buffer" well defined.
enum class RenderParameterBufferIdentity : size_t {
  kExposureRamp = 0,
  kToneCurve = 1,
  kEncodeGamma = 2,
  kCameraWhite = 3,
  kCameraToRgb = 4,
  kRgbToFinal = 5,
  kHueSaturationTable = 6,
  kHueSaturationEncode = 7,
  kHueSaturationDecode = 8,
  kLookTable = 9,
  kLookEncode = 10,
  kLookDecode = 11,
};

constexpr size_t kRenderParameterBufferCount = 12;

class RenderParameterUploadCache {
 public:
  RenderParameterUploadCache() = default;
  ~RenderParameterUploadCache();

  RenderParameterUploadCache(const RenderParameterUploadCache &) = delete;
  RenderParameterUploadCache &operator=(const RenderParameterUploadCache &) =
      delete;

  // THE one entry point. Returns the MTLBuffer (as an opaque void*) holding
  // `byte_length` bytes identical to `host_bytes`, uploading only when the
  // bytes differ from what this lane last uploaded for this identity — or when
  // the byte length changed, which guards a resized table.
  //
  // Returns nullptr when no device buffer could be provided; the caller then
  // takes today's path for that buffer. A nullptr answer never leaves the cache
  // in a state claiming the bytes are resident.
  //
  // Increments render_parameter_uploads_performed() on a miss and
  // render_parameter_cache_hits() on a hit. Both counters are process-wide on
  // purpose: a "zero uploads" reading produced by code that never ran is
  // distinguishable from one produced by twelve cache hits (plan §5.2).
  void *ensure_buffer_uploaded(RenderParameterBufferIdentity buffer_identity,
                               const void *host_bytes, size_t byte_length);

  // Releases the twelve MTLBuffers and the retained host copies. Called from
  // the same teardown path that frees a lane's other device resources.
  void reset_for_lane_teardown();

 private:
  struct CachedParameterBuffer {
    void *metal_buffer = nullptr;       // retained MTLBuffer, or nullptr
    size_t metal_buffer_capacity = 0;   // allocated length of metal_buffer
    size_t byte_length = 0;             // bytes currently valid, 0 = empty
    uint64_t content_digest = 0;        // FNV-1a over those bytes
    std::vector<unsigned char> retained_host_copy;
  };

  CachedParameterBuffer entries_[kRenderParameterBufferCount];
};

// The calling lane's cache, created on first use. nullptr is always a legal
// answer — see the header comment.
RenderParameterUploadCache *render_parameter_cache_for_current_lane();

// Counters, named exactly as plan §5.2 fixes them. AC5 reads the first; the
// second exists so "0 uploads" cannot be produced by instrumentation that never
// executed.
uint64_t render_parameter_uploads_performed();
uint64_t render_parameter_cache_hits();

// Test/reset entry: tears down every lane's cache, for a clean measurement
// baseline. Safe to call with no lanes alive.
void render_parameter_cache_release_all_lanes();

}  // namespace ceyx

#endif  // RENDER_PARAMETER_UPLOAD_CACHE_H
