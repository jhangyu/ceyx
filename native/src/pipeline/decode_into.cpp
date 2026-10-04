// decode_into.cpp -- the caller-buffer decode body shared by every
// ceyx_decode_into_* C entry (src/ffi/ceyx_decode_into_ffi.cpp).
//
// T5b (2026-10-02 techdebt campaign): moved verbatim out of the FFI layer so
// raw_gpu_pipeline.cpp's DNG forwarding branch calls it directly instead of
// calling back up through an exported FFI entry. Residual, deliberately
// not moved: the non-exported diagnostics recorders it calls still live in
// src/ffi/raw_ffi_api.cpp.
#include "decode_into.h"

#include "ceyx_decode_into.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "ceyx_orient.h"
// mem8 v3 T12: the frozen output-format contract (CeyxOutputFormat, the plane
// descriptor, the format-taking entry declarations) and THE sizing arithmetic.
// Included UNCONDITIONALLY, unlike the DNG_ENABLE_GENERIC_RAW block below,
// because the format entries must EXIST in every build for the same reason
// this file's header records for its format-agnostic siblings: a symbol that
// is absent in some builds ships a feature that is silently missing, whereas a
// specific error code is diagnosable.
#include "ceyx_output_format_size.h"
#include "dng_error_codes.h"
#include "dng_ffi_api.h"
#include "dng_pipeline.h"
#include "dng_render_params.h"
#include "raw_ffi_api.h"
#include "raw_file_router.h"
#include "../ffi/heap_page_return.h"
#if defined(DNG_ENABLE_GENERIC_RAW)
#include "raw_gpu_pipeline.h"
#include "raw_persistent_device_arena.h"  // R3-T4: kRawDeviceArenaAlignmentBytes
#endif

#include "ceyx_ffi_export.h"

namespace ceyx {

int32_t probeOutputSize(const char *file_path, int32_t max_dim,
                        int32_t *out_width, int32_t *out_height) {
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
bool decodeIntoPrepare(const char *file_path, int32_t max_dim,
                                  const uint8_t *dst, size_t dst_capacity,
                                  int32_t output_format, RawRoute *route,
                                  DngResult *result,
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
  const int32_t probe_rc = probeOutputSize(file_path, max_dim, &w, &h);
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
  //
  // mem8 v3 T12: the floor is the FORMAT's byte count, from the frozen
  // contract's own sizing function -- w*h*4 for rgba8 exactly as before, and
  // w*h + 2*ceil(w/2)*ceil(h/2) for yuv420. The invariance-under-transposition
  // note above still holds for both formats: transposing swaps w and h, and
  // both formulas are symmetric in them (ceil(w/2)*ceil(h/2) included).
  //
  // A negative answer means the caller named a format this build does not
  // know. It is refused HERE, before any pixel work, and reported as
  // kCeyxErrFormatUnsupportedInBuild -- the existing code whose meaning is
  // "this build cannot produce that", which is exactly the situation. No new
  // error code is invented for it (lead ruling): the alternative would be
  // editing the frozen header.
  const int64_t need_signed = ceyx::output_format_byte_count(output_format, w, h);
  if (need_signed < 0) {
    result->error_code = kCeyxErrFormatUnsupportedInBuild;
    return false;
  }
  const size_t need = static_cast<size_t>(need_signed);
  if (!dst || dst_capacity < need) {
    result->error_code = kCeyxErrDstTooSmall;
    return false;
  }

  // memreclaim spec §9.1: the slot may have been discarded by idle funnel
  // step 4a; re-arm it before phase 3's first write (no-op off macOS).
  ceyx::prepare_slot_reuse(const_cast<uint8_t *>(dst), dst_capacity);

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
void decodeIntoPhase3(const char *file_path, int32_t max_dim,
                                 RawRoute route, uint8_t *dst,
                                 size_t dst_capacity,
                                 int32_t exif_orientation,
                                 bool destination_is_page_aligned,
                                 int32_t output_format,
                                 DngResult *result) {
  if (route == kRawRouteDng) {
    // mem8 v3 T12.7 (user no-divergence ruling 2026-09-20): the DNG route now
    // SERVES the format instead of refusing it. T12.5's refusal lived here
    // because the DNG route's Stage-4 entry is the host-source runner rather
    // than the device-handoff one the format thread first reached; that runner
    // takes the format now, dispatching the SAME yuv420 AOT variants from the
    // SAME colour body as the generic-RAW route (dng_render_halide.cpp), so
    // there is one implementation, not two.
    //
    // The refusal's REASONING is not discarded, it is discharged: the 4-B/px-
    // into-a-1.5-B/px-destination overrun it protected against is now
    // prevented by sizing every destination on this route through
    // ceyx::output_format_byte_count -- in ceyx::decodeIntoPrepare above, in
    // acquireStage4OutputBuffer, and in both Stage-4 capacity checks. An
    // unknown format still refuses, in the runner and in the sizing oracle.
    DngPipelineResult pipeline;
    if (!dng_pipeline_decode_to_rgb_into_oriented(
            file_path, max_dim, dst, dst_capacity, exif_orientation,
            pipeline, output_format)) {
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
  // mem8 v3 T12: the requested output format, on the same principle as
  // exif_orientation above -- a field on RawDevelopParams, not a new pipeline
  // entry, because `develop` is already built locally on every call. From here
  // it reaches the three Stage-4 call sites and, through them, the one place
  // that chooses which AOT entry to dispatch.
  develop.output_format = output_format;

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
  // The [RawTiming] emit that used to live inline here has MOVED into the
  // shared decode path (raw_gpu_pipeline.cpp's decode_file_*_into entries, via
  // raw_timing_log_emit). Emitting from this FFI entry made the CPU phases
  // invisible to probe_concurrent_raw, which calls the shared path directly —
  // so raw_unpack/auto_exposure could not be attributed per lane above w1. Do
  // not re-add an emit here: raw_pipeline_decode_file_into above has already
  // printed this decode's line, and a second one would double-count in every
  // median the harness computes.
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

DngResult *decodeIntoBuffer(const char *file_path, int32_t max_dim,
                            uint8_t *dst, size_t dst_capacity) {
  DngResult *result =
      static_cast<DngResult *>(std::calloc(1, sizeof(DngResult)));
  if (!result) return nullptr;

  RawRoute route = kRawRouteUnknown;
  bool destination_is_page_aligned = false;
  if (!ceyx::decodeIntoPrepare(file_path, max_dim, dst, dst_capacity,
                             /*output_format=*/kCeyxOutputFormatRgba8, &route,
                             result, &destination_is_page_aligned)) {
    return result;
  }
  ceyx::decodeIntoPhase3(file_path, max_dim, route, dst, dst_capacity,
                       /*exif_orientation=*/1, destination_is_page_aligned,
                       /*output_format=*/kCeyxOutputFormatRgba8, result);
  return result;
}

}  // namespace ceyx
