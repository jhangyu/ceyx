// Format-agnostic reusable-buffer entries. ALWAYS compiled: this TU is in
// NATIVE_SOURCES via the GLOB_RECURSE at cmake/pipeline.cmake:90 and is named
// in none of the EXCLUDE filters. The generic-RAW arm is guarded by
// DNG_ENABLE_GENERIC_RAW, which reaches this TU through libraw_vendored's
// INTERFACE definition (cmake/tests.cmake:1372, linked at :1381) — so in an
// OFF build the SYMBOLS still exist and a RAW input gets a specific error,
// rather than the Dart lookup finding nothing and the whole feature going
// silently missing.
#include "ceyx_decode_into.h"

#include <atomic>
#include <cstdlib>

#include "ceyx_orient.h"
#include "dng_error_codes.h"
#include "dng_ffi_api.h"
#include "dng_pipeline.h"
#include "raw_file_router.h"
#if defined(DNG_ENABLE_GENERIC_RAW)
#include "raw_ffi_api.h"
#include "raw_gpu_pipeline.h"
#endif

#if defined(_WIN32)
#define CEYX_FFI_EXPORT __declspec(dllexport)
#else
#define CEYX_FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

extern "C" {

CEYX_FFI_EXPORT int32_t ceyx_probe_output_size(const char *file_path,
                                               int32_t max_dim,
                                               int32_t *out_width,
                                               int32_t *out_height) {
  // Zero the out-params FIRST, so every failure return below leaves them 0
  // without each path having to remember to.
  if (out_width) *out_width = 0;
  if (out_height) *out_height = 0;
  if (!out_width || !out_height) return kDngErrNullPath;

  RawRoute route = kRawRouteUnknown;
  const RawErrorCode prc = raw_probe_file(file_path, &route);
  if (prc != kRawSuccess) return static_cast<int32_t>(prc);

  if (route == kRawRouteDng) {
    DngPipelineResult probe;
    if (!dng_pipeline_probe_output_size(file_path, max_dim, probe)) {
      return probe.error_code != 0 ? probe.error_code : kCeyxErrProbeFailed;
    }
    *out_width = static_cast<int32_t>(probe.width);
    *out_height = static_cast<int32_t>(probe.height);
    return 0;
  }

#if defined(DNG_ENABLE_GENERIC_RAW)
  uint32_t w = 0, h = 0;
  const RawErrorCode rc = raw_pipeline_probe_output_size(
      file_path, max_dim > 0 ? static_cast<uint32_t>(max_dim) : 0u, &w, &h);
  if (rc != kRawSuccess) return static_cast<int32_t>(rc);
  *out_width = static_cast<int32_t>(w);
  *out_height = static_cast<int32_t>(h);
  return 0;
#else
  return kCeyxErrFormatUnsupportedInBuild;
#endif
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Phases 1-2 and phase 3, EXTRACTED (not copied) so that
// ceyx_decode_into_buffer and ceyx_decode_into_buffer_oriented cannot drift.
// Task 2 constraint, spec §4 Task 2 "Behavior": the route probe, extent probe
// and capacity refusal are shared verbatim; only the phase-3 DESTINATION and
// the post-decode orientation differ between the two entries.
// ---------------------------------------------------------------------------

// Phases 1 and 2. Returns true when the caller may proceed to phase 3; on
// false, `result` already carries the error (and, on a capacity refusal, the
// extent) and is ready to be returned to the caller as-is.
static bool ceyxDecodeIntoPrepare(const char *file_path, int32_t max_dim,
                                  const uint8_t *dst, size_t dst_capacity,
                                  RawRoute *route, DngResult *result) {
  *route = kRawRouteUnknown;
  const RawErrorCode prc = raw_probe_file(file_path, route);
  if (prc != kRawSuccess) {
    result->error_code = static_cast<int32_t>(prc);
    return false;
  }

  // Phase 1: metadata-only extent, on whichever route owns this file. Neither
  // probe touches pixels, so a stale caller-side prediction is refused below
  // for the price of a header parse.
  int32_t w = 0, h = 0;
  const int32_t probe_rc = ceyx_probe_output_size(file_path, max_dim, &w, &h);
  if (probe_rc != 0) {
    result->error_code = probe_rc;
    return false;
  }
  result->width = w;
  result->height = h;

  // Phase 2: the capacity decision, before any pixel work on either route.
  // The extent stays filled in on refusal: that is what lets the caller
  // re-acquire an EXACT slot and retry once, rather than guessing again.
  // NOTE for the oriented entry: w*h*4 is invariant under transposition, so
  // this same refusal is correct for every orientation and the caller's
  // pre-acquired slot size never has to know which one was requested
  // (spec §1.3, asserted by AC-2.4).
  const size_t need = static_cast<size_t>(w) * h * 4;
  if (!dst || dst_capacity < need) {
    result->error_code = kCeyxErrDstTooSmall;
    return false;
  }
  return true;
}

// Phase 3: decode through the route's caller-buffer sibling, each of which
// binds dst AFTER its own internal result reset (A3.2). `dst` is whatever
// buffer the caller wants the pixels in — the caller's own buffer for both
// entries now that the GPU kernel writes ORIENTED pixels directly (plan §1.4,
// §1.5 Task 4). exif_orientation is forwarded to the `_oriented` pipeline
// entry / `RawDevelopParams::exif_orientation` on both routes; the plain
// entry (`ceyx_decode_into_buffer`) calls this with 1, so the two entries
// share one body and can never drift.
static void ceyxDecodeIntoPhase3(const char *file_path, int32_t max_dim,
                                 RawRoute route, uint8_t *dst,
                                 size_t dst_capacity,
                                 int32_t exif_orientation,
                                 DngResult *result) {
  if (route == kRawRouteDng) {
    DngPipelineResult pipeline;
    if (!dng_pipeline_decode_to_rgb_into_oriented(
            file_path, max_dim, dst, dst_capacity, exif_orientation,
            pipeline)) {
      result->error_code = pipeline.error_code;
      result->decode_ms = pipeline.decode_ms;
      result->process_ms = pipeline.process_ms;
      return;
    }
    if (!pipeline.rgba_ptr) {
      // fuse_rgba_output=false leaves RGB8 in the buffer, which is not the
      // layout this entry advertises. Refuse loudly rather than hand back
      // pixels in a shape the caller will misread.
      result->error_code = kDngErrRgbaAllocFailed;
      return;
    }
    result->rgba_data = pipeline.rgba_ptr;    // == dst, by construction
    result->width = static_cast<int32_t>(pipeline.width);
    result->height = static_cast<int32_t>(pipeline.height);
    result->decode_ms = pipeline.decode_ms;
    result->process_ms = pipeline.process_ms;
    return;
  }

#if defined(DNG_ENABLE_GENERIC_RAW)
  RawDevelopParams develop{};
  develop.exposure_ev = 0.0f;
  develop.tone_curve_strength = 1.0f;
  develop.output_space = kRawOutputColorSpaceSrgb;
  develop.max_output_long_edge =
      max_dim > 0 ? static_cast<uint32_t>(max_dim) : 0u;
  // Plan §1.4 (RAW side): a field on RawDevelopParams, not a new FFI entry —
  // develop is already constructed locally here on every call.
  develop.exif_orientation = exif_orientation;

  RawPipelineResult out;
  const RawErrorCode rc =
      raw_pipeline_decode_file_into(file_path, develop, dst, dst_capacity, out);
  // R6 fix: raw_decode_and_process (raw_ffi_api.cpp) records out.diag/
  // out.color_diag into thread-local state on every call, success or
  // failure, so raw_last_diagnostics()/raw_last_color_diagnostics() always
  // describe the most recent decode on this thread. This decode-INTO entry
  // point used to skip that recording entirely — a caller who decoded via
  // ceyx_decode_into_buffer and then queried raw_last_diagnostics() got
  // whatever an earlier raw_decode_and_process call had left behind (or "no
  // decode has run" if none had), never THIS call's diagnostics. Record
  // unconditionally, matching raw_decode_and_process's unconditional
  // g_last_diagnostics = out.diag (raw_ffi_api.cpp) before its success check.
  raw_record_decode_into_diagnostics(&out.diag, &out.color_diag);
  result->decode_ms = out.diag.raw_unpack_ms;
  result->process_ms = out.diag.gpu_process_ms;
  if (rc != kRawSuccess) {
    // WP10 boundary map (lead ruling, 2026-09-06). kRawErrDstTooSmall (-213) is
    // INTERNAL: it is what makeRgbaCheckout's POST-UNPACK capacity backstop
    // returns. That backstop is a genuinely different event from the pre-decode
    // refusal above — the pre-check runs before either pipeline is entered,
    // whereas some formats only reveal their true extent during unpack (the
    // X3F/Foveon raw_pitch case, libraw_frontend.cpp:238-243), so the shortfall
    // cannot be known until after. Keeping -213 internal lets the backstop name
    // its own failure precisely instead of borrowing a wrong code; mapping it
    // here keeps A3.4's "one too-small code for one entry pair" true of the
    // PUBLIC surface, which is what Dart sees.
    // width/height are already filled in from the probe above, so the ruling's
    // "with width/height filled in" condition holds on this path too.
    result->error_code = (rc == kRawErrDstTooSmall)
                             ? static_cast<int32_t>(kCeyxErrDstTooSmall)
                             : static_cast<int32_t>(rc);
    // WP10 stale-extent fix. The probe's extent was written above, but on a
    // post-unpack refusal it is the extent that JUST PROVED INSUFFICIENT —
    // handing it back makes the caller re-acquire the same failing size, so the
    // retry can never advance and a bounded retry becomes "photo will not open".
    // The pipeline now publishes the TRUE post-unpack extent before its early
    // return (makeRgbaCheckout), so prefer it whenever it is populated.
    if (out.width > 0) {
      if (out.height > 0) {
        result->width = static_cast<int32_t>(out.width);
        result->height = static_cast<int32_t>(out.height);
      }
    }
    return;
  }
  result->rgba_data = out.rgba_ptr;           // == dst, by construction
  result->width = static_cast<int32_t>(out.width);
  result->height = static_cast<int32_t>(out.height);
  return;
#else
  result->error_code = kCeyxErrFormatUnsupportedInBuild;
  return;
#endif
}

// AC-2.6 hook. The degradation arm is reachable in production only under real
// memory pressure, which a test cannot induce reliably or cheaply; without a
// hook the one branch whose whole purpose is "never fail the decode" would be
// the one branch never executed. Process-global and relaxed: it is flipped by
// a single-threaded test around a single call.
static std::atomic<int32_t> g_force_scratch_failure{0};

extern "C" {

CEYX_FFI_EXPORT int32_t ceyx_debug_force_scratch_failure(int32_t enable) {
  return g_force_scratch_failure.exchange(enable, std::memory_order_relaxed);
}

CEYX_FFI_EXPORT DngResult *ceyx_decode_into_buffer(const char *file_path,
                                                   int32_t max_dim,
                                                   uint8_t *dst,
                                                   size_t dst_capacity) {
  DngResult *result =
      static_cast<DngResult *>(std::calloc(1, sizeof(DngResult)));
  if (!result) return nullptr;

  RawRoute route = kRawRouteUnknown;
  if (!ceyxDecodeIntoPrepare(file_path, max_dim, dst, dst_capacity, &route,
                             result)) {
    return result;
  }
  ceyxDecodeIntoPhase3(file_path, max_dim, route, dst, dst_capacity,
                       /*exif_orientation=*/1, result);
  return result;
}

#if defined(DNG_STAGE4_SPLIT_KERNEL)
// TEMP-VULKAN-ORIENT (deleted in Task 9): the Vulkan split kernel does not
// carry the fused orientation until Task 7/Phase 3 lands. Until then this
// build decodes unoriented and orients on the CPU, exactly as before the
// productionization plan — this is the CURRENT (pre-plan) body of
// ceyx_decode_into_buffer_oriented, moved verbatim into a static function
// under this guard (plan §3 Task 4 Step 4.2).
static DngResult *ceyxDecodeIntoBufferOrientedCpuLegacy(
    const char *file_path, int32_t max_dim, uint8_t *dst, size_t dst_capacity,
    int32_t exif_orientation, RawRoute route, DngResult *result) {
  const bool transposes = ceyx_orientation_transposes(exif_orientation) != 0;

  // Non-transposing (1,2,3,4 and every out-of-range value, which the host's
  // table treats as 1): decode straight into the caller's buffer and orient it
  // in place. Zero extra memory — the common non-identity case, orientation 3,
  // lands here.
  if (!transposes) {
    ceyxDecodeIntoPhase3(file_path, max_dim, route, dst, dst_capacity,
                         /*exif_orientation=*/1, result);
    if (result->error_code != 0) return result;
    int32_t ow = 0, oh = 0;
    const int32_t orc =
        ceyx_orient_rgba(dst, dst, dst_capacity, result->width, result->height,
                         exif_orientation, &ow, &oh);
    if (orc != 0) {
      // Structurally unreachable: capacity was proven in phase 2 and in-place
      // is legal for every non-transposing case. Reported rather than ignored,
      // because silently handing back half-oriented pixels is worse than an
      // error the caller can see.
      result->rgba_data = nullptr;
      result->error_code = orc;
      return result;
    }
    result->width = ow;
    result->height = oh;
    return result;
  }

  // Transposing (5,6,7,8): the decode cannot write its own source in place, so
  // it goes to a scratch frame from the SAME pool the decoders use — bounded
  // by the configured slot count, not by the number of photos.
  const size_t need =
      static_cast<size_t>(result->width) * result->height * 4;
  uint8_t *scratch = g_force_scratch_failure.load(std::memory_order_relaxed)
                         ? nullptr
                         : dng_rgba_output_acquire(need);
  if (!scratch) {
    // MANDATED DEGRADATION (spec §4 Task 2). A scratch shortage is a
    // memory-pressure blip; refusing the decode would turn it into "the photo
    // will not open". Decode unoriented into dst and return SUCCESS with the
    // UNSWAPPED extent — the caller's extent-consistency check sees that the
    // extent did not swap, reports appliedOrientation = 1, and rotates on the
    // host exactly as it does for every non-ceyx decoder arm.
    ceyxDecodeIntoPhase3(file_path, max_dim, route, dst, dst_capacity,
                         /*exif_orientation=*/1, result);
    return result;
  }

  ceyxDecodeIntoPhase3(file_path, max_dim, route, scratch, need,
                       /*exif_orientation=*/1, result);
  if (result->error_code != 0) {
    dng_rgba_output_release(scratch);
    return result;
  }

  int32_t ow = 0, oh = 0;
  const int32_t orc =
      ceyx_orient_rgba(scratch, dst, dst_capacity, result->width,
                       result->height, exif_orientation, &ow, &oh);
  dng_rgba_output_release(scratch);
  if (orc != 0) {
    result->rgba_data = nullptr;
    result->error_code = orc;
    return result;
  }
  // Pointer identity contract, preserved: phase 3 set rgba_data to the SCRATCH
  // (its own dst), which must never escape to the caller. The oriented pixels
  // are in the caller's buffer, so that is what is reported.
  result->rgba_data = dst;
  result->width = ow;
  result->height = oh;
  return result;
}
#endif  // DNG_STAGE4_SPLIT_KERNEL

CEYX_FFI_EXPORT DngResult *ceyx_decode_into_buffer_oriented(
    const char *file_path, int32_t max_dim, uint8_t *dst, size_t dst_capacity,
    int32_t exif_orientation) {
  DngResult *result =
      static_cast<DngResult *>(std::calloc(1, sizeof(DngResult)));
  if (!result) return nullptr;

  RawRoute route = kRawRouteUnknown;
  if (!ceyxDecodeIntoPrepare(file_path, max_dim, dst, dst_capacity, &route,
                             result)) {
    return result;
  }

#if defined(DNG_STAGE4_SPLIT_KERNEL)
  // Vulkan split-kernel build: dispatch to the CPU-legacy path above (see its
  // marker comment for why it still exists and when it goes away).
  return ceyxDecodeIntoBufferOrientedCpuLegacy(file_path, max_dim, dst,
                                               dst_capacity, exif_orientation,
                                               route, result);
#else
  // Fused path: the kernel writes oriented pixels straight into the caller's
  // buffer. No scratch, no second pass, no degradation arm — the transposing
  // in-place impossibility that motivated them no longer exists.
  //
  // R-19 (named behaviour change, plan §3 Task 4): post-fusion there is no
  // successful-but-unoriented return value any more. Previously a scratch
  // shortage under memory pressure still produced a viewable photo
  // (unoriented, with the host rotating). Now an orientation failure IS a
  // decode failure — whatever the Stage4 kernel failure is (result->error_code
  // set by the pipeline, rgba_data left null). This is the accepted
  // consequence of D1, not an oversight.
  ceyxDecodeIntoPhase3(file_path, max_dim, route, dst, dst_capacity,
                       exif_orientation, result);
  if (result->error_code != 0) return result;
  result->rgba_data = dst;   // pipeline already reported the ORIENTED extent
  return result;
#endif
}

}  // extern "C"

// Deliberate absence, recorded so it is not "fixed" later: there is NO
// failure-path release here. raw_decode_and_process has an
// `else if (out.rgba_ptr) { dng_rgba_output_release(...) }` arm
// (raw_ffi_api.cpp:84-87) because ITS buffer is pool-owned. With a caller
// buffer there is nothing pool-owned to give back, and releasing dst into the
// pool would hand a Dart-owned address to the next decode — the exact
// corruption the borrowing guards exist to prevent, and one the pool would
// absorb silently (RgbaOutputPool::release logs unknown pointers as a no-op).
//
// SIBLING NOTE (Task 2), because the paragraph above now has exactly one
// exception and an unqualified "this file never releases" would be false:
// ceyx_decode_into_buffer_oriented's transposing arm checks a SCRATCH frame out
// of that same pool, and the scratch IS pool-owned, so this file DOES release
// it — on every exit without exception: the decode-failure return, the
// orientation-error return, and the success path. `dst` remains untouched by
// the rule above; the two buffers are never confused because the scratch never
// leaves this function and result->rgba_data is re-pointed at `dst` before the
// oriented entry returns. The one arm that takes no scratch at all is the
// checkout-failure degradation, which has nothing to release.
