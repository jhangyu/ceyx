#pragma once

/* RAW (LibRaw) route FFI surface.
 *
 * Extracted from dng_ffi_api.h on 2026-08-25. The RAW route returns the same
 * DngResult as the DNG route and reuses dng_free_result
 * for teardown, so this header includes dng_ffi_api.h rather than duplicating
 * the struct: the ABI is shared by design, and duplicating it would create two
 * definitions to keep in sync with plugin/lib/src/dng_bindings.dart.
 *
 * The full definition of RawDecodeDiagnostics lives in raw_pipeline_contract.h;
 * this header only forward-declares it, exactly as dng_ffi_api.h used to.
 */
#include <stdint.h>

#include "dng_ffi_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WP5: the allocating generic-RAW decode entry that used to be declared here
 * is DELETED. Generic RAW is decoded through ceyx_decode_into_buffer
 * (ceyx_decode_into.h), which writes into a CALLER-OWNED buffer; the library
 * no longer allocates full-resolution RGBA output on any path. */
struct RawDecodeDiagnostics;

/* Diagnostics for the calling thread's most recent RAW decode.
 * Returns 0 on success, -1 when out is null or no decode has run. */
int32_t raw_last_diagnostics(struct RawDecodeDiagnostics *out);

/* Round 2 Task 2.4: colour-pipeline diagnostics channel, additive and
 * separate from RawDecodeDiagnostics (frozen -- Dart-visible layout, spec
 * section 12). New symbol, new struct, so no existing ABI moves.
 *
 * kRawCameraMatrixRoute* mirrors (by value, 0/1/2) the private constants
 * local to libraw_gpu_input_adapter.cpp (kRawCameraMatrixRouteNone/RgbCam/
 * CamXyz). Round 2 Task 2.6 threads the real per-decode value through
 * RawPipelineResult::color_diag (raw_gpu_pipeline.h) -- see
 * raw_last_color_diagnostics's contract below for what is now live. */
enum {
    kRawCameraMatrixRouteNone = 0,
    kRawCameraMatrixRouteRgbCam = 1,
    kRawCameraMatrixRouteCamXyz = 2
};

/* Mirrors RawAutoExposureStatus (raw_auto_exposure.h) by value; kept as a
 * plain uint32_t here rather than including that header, which is owned by
 * a parallel round-2 task. This name is retained as the literal 0 (== kOk)
 * for source compatibility with existing callers written against Task 2.4's
 * build; it is a real, meaningful status now (Task 2.6), not only a sentinel. */
enum { kRawColorAutoExposureStatusUnavailable = 0 };

typedef struct RawColorDiagnostics {
    uint32_t struct_size;
    float    auto_exposure_ev;      /* what Round 1 actually applied */
    uint32_t auto_exposure_status;  /* RawAutoExposureStatus (mirrored value) */
    uint32_t vendor_curve_applied;  /* 1 = LibRaw handed us curve-applied pixels */
    uint32_t matrix_route;          /* kRawCameraMatrixRoute* above */
    uint32_t clamped_mask;          /* bit0 exposure, bit1 tone, bit2 shadows, bit3 wb */
    char     reason[128];
} RawColorDiagnostics;

/* Colour diagnostics for the calling thread's most recent RAW decode.
 * Returns 0 on success, -1 when out is null or no decode has run yet
 * (mirrors raw_last_diagnostics's contract exactly).
 *
 * Round 2 Task 2.6 threads the real values through RawPipelineResult
 * (raw_gpu_pipeline.h's RawColorPipelineDiagnostics) from where each is
 * actually computed: auto_exposure_ev/status/reason from the auto-exposure
 * estimator inside LibRawGpuInputAdapter::build() (only when a CFA or
 * linear-RGB layout was attempted -- an unattempted layout leaves all three
 * at their zero-initialised default, matching auto_exposure_ev's existing
 * "no gain" contract); matrix_route from that same build() call's colour-
 * matrix routing; vendor_curve_applied from Round 2 Task 2.4's frontend-level
 * detection (LibRawRawView::vendor_curve_applied). `reason` is empty when
 * status is kOk or when auto-exposure was never attempted (DNG route,
 * decode failure before the adapter ran).
 * clamped_mask is NOT wired: no round-2 task reports WHICH RawDevelopParams
 * field a value was clamped from -- raw_contract_validate.cpp enforces the
 * ranges but does not report per-field clamp bits. Left at 0 rather than
 * guessed; a future task owns adding that reporting.
 * `struct_size` is always valid on a successful (0) return. */
int32_t raw_last_color_diagnostics(RawColorDiagnostics *out);

/* C4 (plan §6.4): sub-timing diagnostics channel, additive and separate from
 * RawDecodeDiagnostics (frozen -- Dart-visible layout, spec section 12) and
 * from RawColorDiagnostics above. New symbol, new struct, so no existing ABI
 * moves and no Dart file is touched.
 *
 * Splits the single `gpu_process_ms` window (raw_gpu_pipeline.cpp gpu_t0/
 * gpu_t1) into host<->device copy time, CPU auto-exposure time (previously
 * unattributed to either raw_unpack_ms or gpu_process_ms -- plan §6.3), and
 * the GPU submit/wait residue. gpu_submit_wait_ms is computed as
 * gpu_process_ms - host_copy_ms, never independently bracketed, so the parts
 * cannot drift out of agreement with the existing total. */
typedef struct RawTimingDiagnostics {
    uint32_t struct_size;            /* sizeof(RawTimingDiagnostics) by the producer */
    double   host_copy_ms;           /* host->device + device->host, frame-sized buffers */
    double   host_to_device_copy_ms; /* the upload half, reported separately for AC2 */
    double   device_to_host_copy_ms; /* the copy-back half, reported separately for AC2 */
    double   auto_exposure_ms;       /* CPU auto-exposure estimator, plan §6.3 */
    double   gpu_submit_wait_ms;     /* gpu_process_ms - host_copy_ms, plan §6.2 item 3 */
    uint32_t unified_memory_path_active;   /* 1 = zero-copy path taken this decode */
} RawTimingDiagnostics;

/* Timing diagnostics for the calling thread's most recent RAW decode.
 * Returns 0 on success, -1 when out is null or no decode has run yet
 * (mirrors raw_last_color_diagnostics's contract exactly). `struct_size` is
 * always valid on a successful (0) return. */
int32_t raw_last_timing_diagnostics(struct RawTimingDiagnostics *out);

/* Internal call, same binary, never looked up via dlsym/FFI -- deliberately
 * NOT RAW_FFI_EXPORT'd and not part of the Dart-visible surface, mirroring
 * raw_record_decode_into_diagnostics below. Called unconditionally (success
 * or failure) from the decode-INTO entry point so a failed decode still
 * reports whatever sub-timings it accumulated before failing (plan §6.4). */
void raw_record_decode_timing_diagnostics(const struct RawTimingDiagnostics *timing);

/* R6 fix: wires the decode-INTO entry point (ceyx_decode_into_buffer's
 * generic-RAW arm, native/src/ffi/ceyx_decode_into_ffi.cpp) into the
 * thread-local diagnostics state the two queries above read, so they reflect
 * the most recent decode. WP5 deleted the allocating RAW entry that used to
 * share this duty, so this is now the SOLE writer -- which is the intended end
 * state. Before the R6 fix a decode-into call left both queries describing
 * whatever earlier allocating-entry call last ran on this thread (or "no decode
 * has run" if none had) -- stale-by-construction, not merely by timing.
 *
 * Internal call, same binary, never looked up via dlsym/FFI -- deliberately
 * NOT RAW_FFI_EXPORT'd and not part of the Dart-visible surface. `diag` is
 * required; `color_diag` may be null (a route with no colour pipeline, e.g. a
 * failure before the adapter ran, simply leaves the colour channel
 * unrecorded, exactly as the deleted allocating entry already treated a
 * failed build()). RawColorPipelineDiagnostics is forward-declared only: this header
 * never needs its layout, only a pointer to it. */
struct RawColorPipelineDiagnostics;
void raw_record_decode_into_diagnostics(
    const RawDecodeDiagnostics *diag,
    const struct RawColorPipelineDiagnostics *color_diag);

/* ===================================================================== */
/* C3 render-parameter upload cache probe (R1-T1, GPU copy-elimination    */
/* campaign, docs/logs/2026-09-11/plan-gpu-copy-elimination.md §5.2/§5.4). */
/*                                                                        */
/* DEBUG/PROBE API, in the same category as the pool live-address gauge:   */
/* it is NOT part of the Dart-visible surface and adds nothing to          */
/* DngResult. Defined in native/src/ffi/dng_ffi_api.cpp beside the         */
/* existing FFI_EXPORT block (precedent: dng_decode_configured_slots).     */
/*                                                                        */
/* Reports the PROCESS-WIDE totals since process start. The gate reads     */
/* deltas across decodes: same-settings repeat decode => uploads delta 0   */
/* and cache-hits delta 12. Both counters are reported because zero        */
/* uploads with zero hits means the caching code never ran at all, which   */
/* is a different and much worse outcome than zero uploads with twelve     */
/* hits. Returns 0 on success, -1 when BOTH out-pointers are null; either  */
/* out-pointer alone may be null.                                          */
/* ===================================================================== */
int32_t ceyx_debug_render_parameter_cache_counters(
    uint64_t *out_uploads_performed,
    uint64_t *out_cache_hits);

#ifdef __cplusplus
}
#endif
