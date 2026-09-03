// Generic RAW C ABI. Reuses the FROZEN DngResult layout, so Dart bindings need
// no struct change (spec section 12.2). error_code carries a RawErrorCode,
// whose values (<= -201) are disjoint from DngErrorCode.
#include <cstdlib>

#include "dng_pipeline.h"
#include "raw_ffi_api.h"
#include "raw_gpu_pipeline.h"

// Same export decoration as src/dng_ffi_api.cpp, so this entry survives any
// future visibility tightening on the dylib.
#if defined(_WIN32)
#define RAW_FFI_EXPORT __declspec(dllexport)
#else
#define RAW_FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

// Round 2 Task 2.4 acceptance bullet: RawDecodeDiagnostics is explicitly NOT
// modified by this task (fixed Dart-visible layout, spec section 12). This
// pins its pre-change size so a future accidental edit anywhere in this
// header chain fails the build instead of silently reflowing the Dart FFI
// struct. Measured via native/scripts/tmp/round2_sizeof_probe.cpp before
// this assert was written (RC=0, see round2_syms.txt).
static_assert(sizeof(RawDecodeDiagnostics) == 64,
              "RawDecodeDiagnostics size changed -- this struct is Dart-visible "
              "and frozen for Round 2 Task 2.4 (spec section 12)");

namespace {
thread_local RawDecodeDiagnostics g_last_diagnostics{};
// Round 2 Task 2.4. Mirrors g_last_diagnostics's lifecycle: reset to a
// not-yet-decoded sentinel at translation-unit init, overwritten at the end
// of every raw_decode_and_process call. g_have_color_diagnostics is the
// sentinel (rather than reusing RawDecodeDiagnostics::frontend the way
// raw_last_diagnostics does) because struct_size is always non-zero here by
// construction, so it cannot double as "nothing recorded yet".
thread_local RawColorDiagnostics g_last_color_diagnostics{};
thread_local bool g_have_color_diagnostics = false;
}

extern "C" {

RAW_FFI_EXPORT DngResult* raw_decode_and_process(const char* file_path,
                                                 int32_t max_dim) {
    DngResult* result = static_cast<DngResult*>(std::calloc(1, sizeof(DngResult)));
    if (!result) return nullptr;

    RawDevelopParams develop{};
    develop.exposure_ev = 0.0f;
    develop.tone_curve_strength = 1.0f;
    develop.output_space = kRawOutputColorSpaceSrgb;
    develop.max_output_long_edge = max_dim > 0 ? static_cast<uint32_t>(max_dim) : 0u;

    RawPipelineResult out;
    const RawErrorCode rc = raw_pipeline_decode_file(file_path, develop, out);
    g_last_diagnostics = out.diag;

    // Round 2 Task 2.4: populate whatever this translation unit can actually
    // see (see the KNOWN GAP comment on raw_last_color_diagnostics in
    // raw_ffi_api.h for what is NOT reachable here and why).
    RawColorDiagnostics color_diag{};
    color_diag.struct_size = static_cast<uint32_t>(sizeof(RawColorDiagnostics));
    color_diag.auto_exposure_ev = 0.0f;
    color_diag.auto_exposure_status = kRawColorAutoExposureStatusUnavailable;
    color_diag.vendor_curve_applied = 0;
    color_diag.matrix_route = kRawCameraMatrixRouteNone;
    color_diag.clamped_mask = 0;
    color_diag.reason[0] = '\0';
    g_last_color_diagnostics = color_diag;
    g_have_color_diagnostics = true;

    result->error_code = static_cast<int32_t>(rc);
    result->decode_ms = out.diag.raw_unpack_ms;
    result->process_ms = out.diag.gpu_process_ms;
    if (rc == kRawSuccess) {
        result->rgba_data = out.rgba_ptr;
        result->width = static_cast<int32_t>(out.width);
        result->height = static_cast<int32_t>(out.height);
    } else if (out.rgba_ptr) {
        // Never hand a partial buffer back; the checkout must not leak either.
        dng_rgba_output_release(out.rgba_ptr);
    }
    return result;
}

RAW_FFI_EXPORT int32_t raw_last_diagnostics(RawDecodeDiagnostics* out) {
    if (!out) return -1;
    if (g_last_diagnostics.frontend == kRawFrontendUnknown) return -1;
    *out = g_last_diagnostics;
    return 0;
}

RAW_FFI_EXPORT int32_t raw_last_color_diagnostics(RawColorDiagnostics* out) {
    if (!out) return -1;
    if (!g_have_color_diagnostics) return -1;
    *out = g_last_color_diagnostics;
    return 0;
}

}  // extern "C"
