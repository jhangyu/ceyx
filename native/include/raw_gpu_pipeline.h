#ifndef RAW_GPU_PIPELINE_H_
#define RAW_GPU_PIPELINE_H_

// Layout dispatch -> fused normalize+demosaic AOT -> SHARED Stage4 -> the
// CALLER's RGBA buffer. WP5: there is no native RGBA output pool; every full
// resolution RGBA address in the process is owned by the caller.
//
// No decoder-specific handle type appears in this header: the GPU layer sees
// only the plain-C contract (spec section 13.1). The one non-contract include
// below exists solely for the RawForcedBackend enum, which names no vendor
// type. Spec section 13.1 checks that boundary with a grep over this file, so
// the prose here deliberately never spells a vendor type name either.

#include <cstddef>
#include <cstdint>

#include "libraw_frontend.h"       // RawForcedBackend only (test-only override)
#include "raw_ffi_api.h"           // C4 (plan §6.4/§6.7): RawTimingDiagnostics
// Negative-space note (round-1 review, 08-24-style): because raw_ffi_api.h is
// included above, any file-local constant or macro declared in this header
// that happens to share a name with one of raw_ffi_api.h's own top-level
// names will collide at this translation unit's scope -- there is no
// namespace boundary between the two. Nothing here does today; a future edit
// adding a same-named file-local symbol is the failure mode this note exists
// to head off.
#include "raw_pipeline_contract.h"

// Round 2 Task 2.6: colour-pipeline diagnostics that RawDecodeDiagnostics
// (frozen, Dart-visible) cannot carry. Additive field on RawPipelineResult,
// not on the frozen struct. auto_exposure_status/matrix_route are stored as
// plain uint32_t mirroring RawAutoExposureStatus (raw_auto_exposure.h) and
// LibRawGpuInputAdapter's private kRawCameraMatrixRoute* by VALUE (0/1/2) --
// this header must not include either owner's header (raw_auto_exposure.h is
// round-2-owned by a parallel task; the matrix-route constants are
// deliberately file-local to libraw_gpu_input_adapter.cpp, see that file's
// comment on why). raw_ffi_api.cpp casts these straight through to
// RawColorDiagnostics's identically-shaped fields.
struct RawColorPipelineDiagnostics {
    float    auto_exposure_ev = 0.0f;
    uint32_t auto_exposure_status = 0;
    char     auto_exposure_reason[96] = {};  // matches RawAutoExposureResult::reason
    uint32_t matrix_route = 0;
    uint32_t vendor_curve_applied = 0;
    // C4 (plan §6.2 item 2 / §6.3 / §6.7): wall time of the CPU auto-exposure
    // estimator call inside LibRawGpuInputAdapter::build (raw_auto_exposure_
    // estimate, libraw_gpu_input_adapter.cpp:576 CFA / :593 linear-RGB).
    // Reports 0.0 when auto-exposure did not run (mode off, or a layout the
    // adapter does not attempt). Internal-only: NOT forwarded by
    // raw_ffi_api.cpp's RawColorDiagnostics conversion, read only by
    // decodeFileImpl to fill RawPipelineResult::timing.auto_exposure_ms.
    double   auto_exposure_estimator_ms = 0.0;
};

// Round 2 Task 2.6: reads back the diagnostics the most recent
// LibRawGpuInputAdapter::build() call on THIS THREAD computed as locals
// (auto-exposure status/reason, matrix route) but had no way to return
// directly -- libraw_gpu_input_adapter.h is outside this task's file
// ownership this round, so widening its build() signature was avoided; the
// adapter's .cpp (which this task does own) stores the values in
// thread-local state instead. Valid immediately after any build() call,
// success or failure; call it right after build(), before anything else on
// this thread could call build() again.
RawColorPipelineDiagnostics raw_adapter_last_color_diagnostics();

struct RawPipelineResult {
    uint8_t* rgba_ptr = nullptr;   // pool-owned UNLESS caller_dst was set; see below
    size_t   rgba_size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    RawDecodeDiagnostics diag{};
    RawColorPipelineDiagnostics color_diag{};
    // C4 (plan §6.4/§6.7): host/device copy + auto-exposure sub-timings.
    // ABI WARNING (round-4 review B2): embedded BY VALUE, and NOT the last
    // member of this struct (error/caller_dst follow) -- RawTimingDiagnostics
    // growing (e.g. R4-T4's source_mosaic_copy_milliseconds field) shifts
    // every member below it and changes sizeof(RawPipelineResult) itself.
    // RawPipelineResult is passed by pointer across the FFI/dylib boundary,
    // so this is NOT freely extensible the way a struct_size-versioned
    // out-param is: a binary compiled against an older header declaring
    // `RawPipelineResult result{}` on its stack, then calling into a NEWER
    // dylib built with a larger RawTimingDiagnostics, overflows that stack
    // object (reproduced this round: test_concurrent_raw_decode_wrapped
    // SIGABRT'd until rebuilt in lockstep with the dylib). Every producer AND
    // consumer of RawPipelineResult must be recompiled together whenever
    // RawTimingDiagnostics's size changes.
    RawTimingDiagnostics timing{};
    RawErrorCode error = kRawSuccess;

    // WP10 (AMENDMENT 3 / A3.2): set by raw_pipeline_decode_file_into AFTER the
    // internal result reset — never pre-set by a caller. Communicating the
    // caller's buffer through pre-set struct fields is rejected by A3.2,
    // because this pipeline (and the DNG one) resets its result at entry, so a
    // pre-set field is wiped by construction.
    //
    // The pipeline writes RGBA into caller_dst and never frees it. rgba_ptr is
    // set to caller_dst on success, so `rgba_ptr == caller_dst` is the caller's
    // proof that its buffer was used rather than an assumption. WP5: null is no
    // longer a supported value — there is no pool to fall back to, and
    // makeRgbaCheckout refuses a null destination. A caller-supplied capacity is
    // NOT a licence to skip the trust-boundary ceiling: extentWithinCeiling
    // still runs first.
    uint8_t* caller_dst = nullptr;
    size_t   caller_dst_capacity = 0;
};

// Dispatches on the VALIDATED layout descriptor only - never on vendor or
// unpack backend (spec section 6.4.5).
RawErrorCode raw_pipeline_decode_to_rgba(const RawGpuInput& input,
                                         const RawDevelopParams& develop,
                                         RawPipelineResult& out);

// WP3 (lead ruling 2026-09-07, additive-only): caller-buffer sibling of
// raw_pipeline_decode_to_rgba. `dst` is bound to the result HERE, inside the
// pipeline, so the A3.2 rule above ("never pre-set by a caller") stays true.
// On success out.rgba_ptr == dst; `dst` is never freed on any path. Exists
// because the plain entry above was one of the four routes that could still
// reach the checkout's owning mode, which WP3 proved unreachable and collapsed;
// WP5 then deleted the pool itself.
RawErrorCode raw_pipeline_decode_to_rgba_into(const RawGpuInput& input,
                                              const RawDevelopParams& develop,
                                              uint8_t* dst, size_t dst_capacity,
                                              RawPipelineResult& out);

// WP5: the null-destination entries (raw_pipeline_decode_file, _forced and
// _cancellable) are DELETED. Since WP3 collapsed the checkout guard to
// borrow-only, a null destination is refused by makeRgbaCheckout, so none of
// them could succeed on a RAW route. Only the caller-buffer forms remain.

// Full route including probe, generic unpack and the adapter, with the RGBA
// output written into the CALLER's buffer. The frontend context lives on this
// function's stack and is destroyed only after the device->host read has
// completed (spec section 5.1.5). `dst` is passed as a PARAMETER and bound to
// the result AFTER the internal reset, which is what makes it immune to that
// reset rather than patched around it.
//
// On success out.rgba_ptr == dst. `dst` is never freed on any path, including
// every failure path. A capacity shortfall
// discovered after unpack (the extent can move for a few exotic formats — the
// X3F/Foveon raw_pitch adjustment in libraw_frontend.cpp is the in-tree case)
// is refused rather than overrun, so a stale prediction costs a slow retry and
// never corruption.
//
RawErrorCode raw_pipeline_decode_file_into(const char* file_path,
                                           const RawDevelopParams& develop,
                                           uint8_t* dst, size_t dst_capacity,
                                           RawPipelineResult& out);

// WP10: metadata-only output-extent probe. Opens the file through the
// frontend's open-only entry (open_file, NEVER unpack), reads the visible
// extent LibRaw fills into imgdata.sizes at open time, and applies the SAME
// scaledOutputExtent rule the three GPU branches apply. Never allocates an
// output buffer and never dispatches to the GPU.
//
// Performs no route detection: a DNG path handed to this function is opened as
// a generic RAW. Routing stays in the FFI entry, so this layer keeps its shape.
//
// The byte requirement for the matching decode is (*out_width)*(*out_height)*4.
// Returns kRawSuccess, or an error code with both out-params set to 0.
RawErrorCode raw_pipeline_probe_output_size(const char* file_path,
                                            uint32_t max_long_edge,
                                            uint32_t* out_width,
                                            uint32_t* out_height);

// Test-only: as raw_pipeline_decode_file_into with the unpack backend forced.
RawErrorCode raw_pipeline_decode_file_forced_into(const char* file_path,
                                                  const RawDevelopParams& develop,
                                                  RawForcedBackend forced,
                                                  uint8_t* dst, size_t dst_capacity,
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
RawErrorCode raw_pipeline_decode_file_cancellable_into(const char* file_path,
                                                       const RawDevelopParams& develop,
                                                       const RawCancelToken& cancel,
                                                       uint8_t* dst, size_t dst_capacity,
                                                       RawPipelineResult& out);

#endif  // RAW_GPU_PIPELINE_H_
