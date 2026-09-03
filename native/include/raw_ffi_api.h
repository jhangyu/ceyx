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
 * CamXyz) -- that file is round-2-owned by a parallel task and its route
 * decision is NOT threaded into RawPipelineResult this round (see the
 * BLOCKED-partial note below and in round2_vendor_curve.md), so
 * matrix_route below always reads kRawCameraMatrixRouteNone in this build. */
enum {
    kRawCameraMatrixRouteNone = 0,
    kRawCameraMatrixRouteRgbCam = 1,
    kRawCameraMatrixRouteCamXyz = 2
};

/* Mirrors RawAutoExposureStatus (raw_auto_exposure.h) by value; kept as a
 * plain uint32_t here rather than including that header, which is owned by
 * a parallel round-2 task. kOk=0 is the only value this build can currently
 * produce (see BLOCKED-partial note) -- it is NOT a claim that the solver
 * always succeeds, only that this channel cannot see its real status yet. */
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
 * KNOWN GAP (Round 2 Task 2.4, reported rather than silently partial):
 * auto_exposure_ev/auto_exposure_status/matrix_route/clamped_mask are NOT
 * wired to their real per-decode values in this build. The data exists --
 * auto_exposure_ev and the solver's status/reason are computed inside
 * libraw_gpu_input_adapter.cpp::build() (round-2-owned by a parallel task,
 * off limits this round), matrix_route inside the same file's
 * cameraMatrixRoute() helper -- but RawPipelineResult (raw_gpu_pipeline.h,
 * also not owned by this task) carries only the frozen RawDecodeDiagnostics
 * back to this translation unit, with no field for any of the above. Wiring
 * this channel end to end requires adding fields to RawPipelineResult and
 * populating them in raw_gpu_pipeline.cpp / libraw_gpu_input_adapter.cpp,
 * both outside this task's file ownership boundary. vendor_curve_applied is
 * similarly computed by libraw_frontend.cpp (this task's file, see
 * LibRawRawView::vendor_curve_applied) but that view is consumed entirely
 * inside raw_pipeline_decode_file's local LibRawFrontendContext and never
 * handed back either, for the same reason.
 * `reason` is always empty in this build; `struct_size` is always valid. */
int32_t raw_last_color_diagnostics(RawColorDiagnostics *out);

#ifdef __cplusplus
}
#endif
