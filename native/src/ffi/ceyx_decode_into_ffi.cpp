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
#include <cstdio>
#include <cstdlib>

#include "ceyx_orient.h"
#include "dng_error_codes.h"
#include "dng_ffi_api.h"
#include "dng_pipeline.h"
#include "dng_render_params.h"
#include "raw_file_router.h"
#if defined(DNG_ENABLE_GENERIC_RAW)
#include "raw_ffi_api.h"
#include "raw_gpu_pipeline.h"
#include "raw_persistent_device_arena.h"  // R3-T4: kRawDeviceArenaAlignmentBytes
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
//
// `out_destination_is_page_aligned` (R3-T4, plan §4.3): a PROBE, never a
// refusal. Always written before returning true (left at its caller-supplied
// value -- typically zero-initialised false -- on any `false` return, since
// no decode will use it then). May be null; the plain (non-generic-RAW-only)
// callers of this function do not all need it.
static bool ceyxDecodeIntoPrepare(const char *file_path, int32_t max_dim,
                                  const uint8_t *dst, size_t dst_capacity,
                                  RawRoute *route, DngResult *result,
                                  bool *out_destination_is_page_aligned) {
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

  // R3-T4 (plan §4.3): the alignment probe, immediately after the capacity
  // check, in the one function both entries share. `newBufferWithBytesNoCopy:
  // length:options:deallocator:` requires the pointer page-aligned AND the
  // length a page multiple -- checked here with the SAME constant C1's arena
  // asserts on its own allocations (kRawDeviceArenaAlignmentBytes), because
  // the two must be checked by one rule (plan §3.1). This is a performance
  // signal only: an unaligned destination degrades to the arena-staged copy
  // path (plan §4.3 "Degradation path"), it never refuses the decode.
#if defined(DNG_ENABLE_GENERIC_RAW)
  if (out_destination_is_page_aligned) {
    const auto address = reinterpret_cast<uintptr_t>(dst);
    *out_destination_is_page_aligned =
        (address % ceyx::kRawDeviceArenaAlignmentBytes == 0) &&
        (dst_capacity % ceyx::kRawDeviceArenaAlignmentBytes == 0);
  }
#else
  if (out_destination_is_page_aligned) *out_destination_is_page_aligned = false;
#endif
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
                                 bool destination_is_page_aligned,
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
  // R3-T4 (plan §4.3): forwarded probe result; the pipeline reads this to
  // decide whether the unified-wrapped destination path is even attemptable
  // for this call (raw_pipeline_contract.h's field comment has the full
  // contract). Never a refusal signal -- false just means the degraded/
  // fallback destination shape is taken.
  develop.caller_destination_is_page_aligned = destination_is_page_aligned;

  RawPipelineResult out;
  const RawErrorCode rc =
      raw_pipeline_decode_file_into(file_path, develop, dst, dst_capacity, out);
  // R6 fix: record out.diag/out.color_diag into thread-local state on every
  // call, success or failure, so raw_last_diagnostics() and
  // raw_last_color_diagnostics() always describe the most recent decode on this
  // thread. This decode-INTO entry point used to skip that recording entirely —
  // a caller who decoded here and then queried raw_last_diagnostics() got
  // whatever the deleted allocating RAW entry had left behind (or "no decode has
  // run" if none had), never THIS call's diagnostics. WP5 deleted that entry, so
  // this call is now the SOLE writer; recording unconditionally, before the
  // success check, is what makes the queries honest on a failed decode too.
  raw_record_decode_into_diagnostics(&out.diag, &out.color_diag);
  // C4 (plan §6.4/§6.7): the timing recorder is called unconditionally,
  // success or failure, beside raw_record_decode_into_diagnostics above, so a
  // failed decode still reports whatever sub-timings it accumulated before
  // failing.
  raw_record_decode_timing_diagnostics(&out.timing);
  if (const char *timing_log = std::getenv("CEYX_RAW_TIMING_LOG")) {
    if (timing_log[0] == '1' && timing_log[1] == '\0') {
      // Plan §6.5: fixed key=value format, %.3f for every _ms value, %u for
      // counters, single prefix "[RawTiming] " -- gates grep by key name,
      // never by column position. Off by default (env var unset emits
      // nothing), so instrumentation cannot affect bit-exactness (AC4).
      std::fprintf(stderr,
                    "[RawTiming] host_to_device_copy_ms=%.3f "
                    "device_to_host_copy_ms=%.3f host_copy_ms=%.3f "
                    "auto_exposure_ms=%.3f gpu_submit_wait_ms=%.3f "
                    "gpu_process_ms=%.3f raw_unpack_ms=%.3f total_ms=%.3f "
                    "unified_memory_path_active=%u\n",
                    out.timing.host_to_device_copy_ms,
                    out.timing.device_to_host_copy_ms,
                    out.timing.host_copy_ms, out.timing.auto_exposure_ms,
                    out.timing.gpu_submit_wait_ms, out.diag.gpu_process_ms,
                    out.diag.raw_unpack_ms, out.diag.total_ms,
                    out.timing.unified_memory_path_active);
    }
  }
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

// Blocker B-1 fix (round reviewer): §1.6 promises -402/-403 out of the fused
// oriented path, but every runner refusal site returns a lossy `bool`, so the
// caller only ever sees the generic code the pipeline layer already reports
// (e.g. kDngErrStage4Failed / kRawErrKernelFailed). dngRenderStage4LastFailureReason()
// (dng_render_params.h) is the per-call-thread-local channel the bridge owner
// added to recover the reason: valid to read ONLY directly after a runner/
// pipeline call returned failure, and always kNone after a success.
//
// CRITICAL: kNone on failure is LEGITIMATE (bad args, scratch allocation, a
// non-orientation SDK refusal) — the existing generic error_code must be kept
// as-is in that case. Mapping kNone to -402/-403 would re-introduce exactly
// the over-claiming bug this channel exists to prevent.
static void ceyxMapStage4FailureReason(DngResult *result) {
  if (result->error_code == 0) return;   // success: nothing to override
  switch (dngRenderStage4LastFailureReason()) {
    case Stage4FailureReason::kOverlap:
      result->error_code = kCeyxOrientErrOverlap;   // -402
      return;
    case Stage4FailureReason::kKernel:
      result->error_code = kCeyxOrientErrKernel;    // -403
      return;
    case Stage4FailureReason::kNone:
    default:
      return;   // keep the existing generic code
  }
}

// WP5 (user ruling R3): the AC-2.6 scratch-failure test hook and its flag are
// DELETED. The degradation arm they existed to exercise no longer exists -- the
// fused kernel writes oriented pixels straight into the caller's buffer, so no
// scratch is taken -- which left the flag written by its setter and read by
// nothing.

extern "C" {

CEYX_FFI_EXPORT DngResult *ceyx_decode_into_buffer(const char *file_path,
                                                   int32_t max_dim,
                                                   uint8_t *dst,
                                                   size_t dst_capacity) {
  DngResult *result =
      static_cast<DngResult *>(std::calloc(1, sizeof(DngResult)));
  if (!result) return nullptr;

  RawRoute route = kRawRouteUnknown;
  bool destination_is_page_aligned = false;
  if (!ceyxDecodeIntoPrepare(file_path, max_dim, dst, dst_capacity, &route,
                             result, &destination_is_page_aligned)) {
    return result;
  }
  ceyxDecodeIntoPhase3(file_path, max_dim, route, dst, dst_capacity,
                       /*exif_orientation=*/1, destination_is_page_aligned,
                       result);
  return result;
}

CEYX_FFI_EXPORT DngResult *ceyx_decode_into_buffer_oriented(
    const char *file_path, int32_t max_dim, uint8_t *dst, size_t dst_capacity,
    int32_t exif_orientation) {
  DngResult *result =
      static_cast<DngResult *>(std::calloc(1, sizeof(DngResult)));
  if (!result) return nullptr;

  RawRoute route = kRawRouteUnknown;
  bool destination_is_page_aligned = false;
  if (!ceyxDecodeIntoPrepare(file_path, max_dim, dst, dst_capacity, &route,
                             result, &destination_is_page_aligned)) {
    return result;
  }

  // Fused path (Task 9: the only path now, on every platform). The kernel
  // writes oriented pixels straight into the caller's buffer. No scratch, no
  // second pass, no degradation arm — the transposing in-place impossibility
  // that motivated them no longer exists.
  //
  // R-19 (named behaviour change, plan §3 Task 4): post-fusion there is no
  // successful-but-unoriented return value any more. Previously a scratch
  // shortage under memory pressure still produced a viewable photo
  // (unoriented, with the host rotating). Now an orientation failure IS a
  // decode failure — whatever the Stage4 kernel failure is (result->error_code
  // set by the pipeline, rgba_data left null). This is the accepted
  // consequence of D1, not an oversight.
  // B-2 fix (round reviewer, fix cycle 2): reset the reason to kNone
  // IMMEDIATELY before phase 3, not just rely on the runner resetting it on
  // its own next call. Without this reset here, a failure that never reaches
  // Stage4 at all — file-not-found, parse failure, OpcodeList2 failure, or
  // the RAW pre-runner dst-too-small backstop in makeRgbaCheckout — would
  // read whatever reason a PRIOR decode on this thread left behind and
  // clobber the real, correct error code with a stale -402/-403. The runner
  // itself resets at its own entry (dng_render_halide.cpp), which is enough
  // ONLY when the runner actually runs; this call may return failure without
  // ever reaching it.
  dngRenderStage4ResetFailureReason();
  ceyxDecodeIntoPhase3(file_path, max_dim, route, dst, dst_capacity,
                       exif_orientation, destination_is_page_aligned, result);
  if (result->error_code != 0) {
    // The read below is honest ONLY because of the reset immediately above:
    // many phase-3 failures (bad file, parse, OpcodeList2, the RAW
    // pre-runner capacity backstop) return without Stage4 ever executing, so
    // the runner's own reset-on-entry never fires on this call. Without the
    // explicit reset here this read could observe a STALE reason left by an
    // earlier decode on this thread and overwrite a correct generic error
    // (e.g. kCeyxErrDstTooSmall) with a wrong -402/-403 (B-2).
    ceyxMapStage4FailureReason(result);
    return result;
  }
  result->rgba_data = dst;   // pipeline already reported the ORIENTED extent
  return result;
}

}  // extern "C"

// Deliberate absence, recorded so it is not "fixed" later: there is NO
// failure-path release in this file, and there is nothing left that could need
// one. `dst` is the CALLER's buffer on every path, and WP5 deleted the RGBA
// output pool, so there is no pool to release anything to and no library-owned
// buffer to give back. Freeing dst here would corrupt a Dart-owned address.
//
// This paragraph used to carry an exception: the oriented entry's transposing
// arm checked a SCRATCH frame out of that pool and did release it. Both the
// scratch checkout and the pool are gone -- the fused kernel writes the
// oriented pixels, transposing included, directly into dst -- so the rule is
// now unqualified.
