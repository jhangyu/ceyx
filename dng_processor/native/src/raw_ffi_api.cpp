// Generic RAW C ABI. Reuses the FROZEN DngResult layout, so Dart bindings need
// no struct change (spec section 12.2). error_code carries a RawErrorCode,
// whose values (<= -201) are disjoint from DngErrorCode.
#include <cstdlib>

#include "dng_ffi_api.h"
#include "dng_pipeline.h"
#include "raw_gpu_pipeline.h"

// Same export decoration as src/dng_ffi_api.cpp, so this entry survives any
// future visibility tightening on the dylib.
#if defined(_WIN32)
#define RAW_FFI_EXPORT __declspec(dllexport)
#else
#define RAW_FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

namespace {
thread_local RawDecodeDiagnostics g_last_diagnostics{};
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

}  // extern "C"
