#pragma once

/* RAW (LibRaw) route FFI surface.
 *
 * Extracted from dng_ffi_api.h on 2026-08-25. The RAW route returns the same
 * DngResult as the DNG route and reuses dng_free_result/dng_free_rgba_buffer
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

/* Generic (non-DNG) RAW decode. Returns the SAME DngResult layout as
 * dng_decode_and_process, so no Dart struct change is needed; free it with
 * dng_free_result(). max_dim <= 0 means full resolution.
 * error_code carries a RawErrorCode (<= -201) on failure. */
struct RawDecodeDiagnostics;
DngResult *raw_decode_and_process(const char *file_path, int32_t max_dim);

/* Diagnostics for the calling thread's most recent raw_decode_and_process.
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

/* Colour diagnostics for the calling thread's most recent
 * raw_decode_and_process. Returns 0 on success, -1 when out is null or no
 * decode has run yet (mirrors raw_last_diagnostics's contract exactly).
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

#ifdef __cplusplus
}
#endif
