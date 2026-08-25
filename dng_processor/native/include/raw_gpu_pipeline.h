#ifndef RAW_GPU_PIPELINE_H_
#define RAW_GPU_PIPELINE_H_

// Layout dispatch -> fused normalize+demosaic AOT -> SHARED Stage4 -> RGBA pool.
//
// No decoder-specific handle type appears in this header: the GPU layer sees
// only the plain-C contract (spec section 13.1). The one non-contract include
// below exists solely for the RawForcedBackend enum, which names no vendor
// type. Spec section 13.1 checks that boundary with a grep over this file, so
// the prose here deliberately never spells a vendor type name either.

#include <cstddef>
#include <cstdint>

#include "libraw_frontend.h"       // RawForcedBackend only (test-only override)
#include "raw_pipeline_contract.h"

struct RawPipelineResult {
    uint8_t* rgba_ptr = nullptr;   // pool-owned; release with dng_rgba_output_release
    size_t   rgba_size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    RawDecodeDiagnostics diag{};
    RawErrorCode error = kRawSuccess;
};

// Dispatches on the VALIDATED layout descriptor only - never on vendor or
// unpack backend (spec section 6.4.5).
RawErrorCode raw_pipeline_decode_to_rgba(const RawGpuInput& input,
                                         const RawDevelopParams& develop,
                                         RawPipelineResult& out);

// Full route including probe, generic unpack and the adapter. The frontend
// context lives on this function's stack and is destroyed only after the
// device->host read has completed (spec section 5.1.5).
RawErrorCode raw_pipeline_decode_file(const char* file_path,
                                      const RawDevelopParams& develop,
                                      RawPipelineResult& out);

// Test-only: same as above with the unpack backend forced.
RawErrorCode raw_pipeline_decode_file_forced(const char* file_path,
                                             const RawDevelopParams& develop,
                                             RawForcedBackend forced,
                                             RawPipelineResult& out);

#endif  // RAW_GPU_PIPELINE_H_
