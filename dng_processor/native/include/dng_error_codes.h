// W5 (M-6): Unified error codes for the DNG pipeline + FFI layer.
//
// Single source of truth for all int32_t error_code values written into
// DngResult / DngPipelineV2Result. Dart mirror: dng_decoder_service.dart
// (DngErrorCode enum). Any addition or value change here MUST be reflected
// in the Dart mirror.
//
// Warmup (dng_decoder_warmup_for_size) uses its own -1/-2 return scale and
// is NOT covered by this enum. Preview (dng_extract_preview_jpeg) mixes
// four independent scales (0, 1, 5, dng_exception, -100); unification is
// deferred.
//
// Positive values (>0) are pass-through DNG SDK dng_exception::ErrorCode().

#pragma once

#ifdef __cplusplus
#include <stdexcept>
#include <string>
#endif

// ---------------------------------------------------------------------------
// Pipeline error codes (written by dng_pipeline_v2.cpp)
// ---------------------------------------------------------------------------
enum DngErrorCode {
  kDngSuccess                        =    0,
  kDngErrNullPath                    =   -1,  // null or empty file_path
  kDngErrParseFailed                 =   -2,  // DNG parse / validation failed
  kDngErrStage3Failed                =   -3,  // Halide Stage3 (demosaic) failed
  kDngErrStage4Failed                =   -4,  // Halide Stage4 (render) failed
  kDngErrStage2HandoffRestoreFailed  =   -5,  // Stage2 device-handoff restore failed
  kDngErrGpuUnavailable              =   -6,  // Metal/Vulkan not available
  kDngErrRgbaAllocFailed             =   -7,  // FFI layer: RGBA pool acquire failed
  kDngErrOl2DispatchFailed           =   -8,  // OpcodeList2 Halide GPU dispatch failed
  kDngErrStdException                = -100,  // caught std::exception
  kDngErrUnknownException            = -101,  // caught (...)
};

// ---------------------------------------------------------------------------
// W5 Option B typed exception: thrown by dng_opcodelist2_halide.cpp when a
// GPU dispatch fails, caught in dng_pipeline_v2.cpp BEFORE the generic
// std::exception handler. The throw is deliberate control flow to bypass
// SDK CPU fallback at the opcode-loop boundary; a status-return redesign
// at that boundary is intentionally deferred.
// ---------------------------------------------------------------------------
#ifdef __cplusplus

class DngOl2DispatchError : public std::runtime_error {
 public:
  explicit DngOl2DispatchError(const std::string &msg)
      : std::runtime_error(msg) {}
  explicit DngOl2DispatchError(const char *msg)
      : std::runtime_error(msg) {}
};

#endif // __cplusplus
