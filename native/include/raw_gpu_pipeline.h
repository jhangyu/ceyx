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

// Absolute ceiling on width*height, checked BEFORE any allocation
// (spec section 10.1). 2^28 pixels (268435456) still leaves a 6.7x margin
// over the largest frame in the corpus (7752x5178 = 40.1 MP) while staying
// well clear of 32-bit element indexing for the RGBA output. Typed (not a
// macro) so it does not pollute every translation unit that includes this
// C++-only header (libraw_frontend.h below is not C-compatible either).
constexpr uint64_t kRawMaxPixelCount = 268435456ull;

// 1 when a GPU backend is usable, 0 otherwise. Returns 0 unconditionally when
// the environment variable DNG_RAW_FORCE_GPU_UNAVAILABLE is set to "1", which
// is the only way to exercise the GPU-mandatory contract (spec section 2.6) on
// a machine that does have a GPU. There is no CPU render fallback, so an
// unusable GPU must be a clean, specific failure and never a different code
// path.
int raw_pipeline_gpu_available();

// Non-zero return requests cancellation. Polled between open_file and unpack,
// after unpack, and before GPU dispatch. Deliberately a plain function pointer:
// no lock on the hot path.
typedef int (*RawCancelCallback)(void* user_data);

struct RawCancelToken {
    RawCancelCallback callback = nullptr;
    void* user_data = nullptr;
};

// Cancellable form. A cancellation seen at any poll point returns
// kRawErrKernelFailed with no GPU work started. Cancellation is deliberately
// NOT honoured once the dispatch is entered: the shared Stage4 call blocks
// until the GPU command completes, and returning earlier would free borrowed
// decoder pixels the GPU is still reading (spec section 5.2.5). The last poll
// therefore sits immediately before the dispatch.
RawErrorCode raw_pipeline_decode_file_cancellable(const char* file_path,
                                                  const RawDevelopParams& develop,
                                                  const RawCancelToken& cancel,
                                                  RawPipelineResult& out);

#endif  // RAW_GPU_PIPELINE_H_
