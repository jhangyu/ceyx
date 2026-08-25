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

// ---------------------------------------------------------------------------
// Pipeline error codes (written by dng_pipeline.cpp)
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
// M-6: DngOl2DispatchError exception class has been removed.
//
// OpcodeList2 GPU dispatch failure is now reported via the
// halide_stage2_ol2_dispatch_failed() status-return flag
// (dng_opcodelist2_halide.h).  The SDK opcode loop (dng_opcode_list.cpp)
// checks the flag after each dispatch call and breaks; dng_pipeline.cpp
// checks it after BuildStage2Image() and maps to kDngErrOl2DispatchFailed.
// ---------------------------------------------------------------------------
