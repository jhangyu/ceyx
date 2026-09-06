#pragma once

// Format-agnostic reusable-buffer decode entries (WP10, AMENDMENT 3).
//
// ONE entry pair covers every route. The caller does not know, and does not
// need to know, whether a path is a DNG or a generic RAW: ceyx_probe_output_size
// and ceyx_decode_into_buffer route internally on raw_probe_file.
//
// WHY THESE ARE ALWAYS PRESENT. The translation unit that defines them
// (src/ffi/ceyx_decode_into_ffi.cpp) is always compiled; only the generic-RAW
// ARM inside it is guarded by DNG_ENABLE_GENERIC_RAW. A build with generic RAW
// compiled out therefore still EXPORTS both symbols, and a RAW input to that
// build gets kCeyxErrFormatUnsupportedInBuild. That is deliberately better than
// omitting the symbols: a Dart guarded lookup that finds nothing ships a
// feature which is silently absent and green everywhere, whereas a specific
// error code is diagnosable.

#include <stddef.h>
#include <stdint.h>

#include "dng_ffi_api.h"  // DngResult, dng_free_result

#ifdef __cplusplus
extern "C" {
#endif

/// Codes for the format-agnostic layer. Deliberately disjoint from BOTH the
/// DNG scale (0..-101, dng_error_codes.h) and the RAW scale (-201..-212,
/// raw_pipeline_contract.h:112-125), so a Dart caller can tell which layer
/// spoke without knowing which route the file took.
enum CeyxDecodeIntoError {
  kCeyxErrDstTooSmall              = -301,
  kCeyxErrFormatUnsupportedInBuild = -302,
  kCeyxErrProbeFailed              = -303,
};

/// Format-agnostic. Probes the OUTPUT extent of a decode WITHOUT decoding.
/// Routes on raw_probe_file: DNG inputs take the DNG metadata probe (parse,
/// no ReadStage1Image); generic-RAW inputs take the LibRaw metadata probe
/// (open_file, no unpack). Both skip the expensive step by construction.
/// Byte requirement for the matching decode is (*out_width)*(*out_height)*4.
/// Returns 0, or a negative code: kCeyxErrDstTooSmall is never returned here;
/// kCeyxErrFormatUnsupportedInBuild means this build has no decoder for that
/// format (generic RAW compiled out).
int32_t ceyx_probe_output_size(const char *file_path, int32_t max_dim,
                               int32_t *out_width, int32_t *out_height);

/// Format-agnostic decode into a CALLER-OWNED buffer.
/// OWNERSHIP: dst is never freed, never released to any pool, never retained.
/// On success result->rgba_data == dst (pointer identity is the contract).
/// The caller MUST clear result->rgba_data before dng_free_result().
/// dst NULL / capacity 0 / capacity < w*h*4 -> kCeyxErrDstTooSmall with
/// width and height FILLED IN and rgba_data NULL, before any pixel work.
/// Returns a heap-allocated DngResult; free with dng_free_result().
DngResult *ceyx_decode_into_buffer(const char *file_path, int32_t max_dim,
                                   uint8_t *dst, size_t dst_capacity);

#ifdef __cplusplus
}
#endif
