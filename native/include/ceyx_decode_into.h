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
/// OWNERSHIP: dst is never freed and never retained by the library.
/// On success result->rgba_data == dst (pointer identity is the contract).
/// WP5: the caller no longer needs to clear result->rgba_data before
/// dng_free_result() -- that call frees ONLY the struct now. Clearing it first
/// remains harmless.
/// dst NULL / capacity 0 / capacity < w*h*4 -> kCeyxErrDstTooSmall with
/// width and height FILLED IN and rgba_data NULL, before any pixel work.
/// Returns a heap-allocated DngResult; free with dng_free_result().
DngResult *ceyx_decode_into_buffer(const char *file_path, int32_t max_dim,
                                   uint8_t *dst, size_t dst_capacity);

/// Orientation-aware sibling of ceyx_decode_into_buffer. Identical contract
/// EXCEPT: the pixels written to dst have EXIF `exif_orientation` already
/// applied, and result->width/height are the ORIENTED extent.
/// exif_orientation outside 1..8 is treated as 1 (identity), matching the
/// host's own table; it is never an error.
/// ADDITIVE: older binaries lack this symbol. Callers MUST resolve it
/// defensively and fall back to ceyx_decode_into_buffer + host-side rotation.
///
/// NO DEGRADATION ARM. This entry either returns the correctly ORIENTED extent
/// or it fails. It never silently returns unoriented pixels with the unswapped
/// extent. Two changes removed the arm that used to be documented here: the
/// fused kernel writes the oriented pixels (transposing included) straight into
/// dst, so no scratch frame is taken at all; and an orientation failure is a
/// decode failure rather than a successful unoriented render (R-19). There is
/// no RGBA pool left for a scratch checkout to fail against.
DngResult *ceyx_decode_into_buffer_oriented(const char *file_path,
                                            int32_t max_dim, uint8_t *dst,
                                            size_t dst_capacity,
                                            int32_t exif_orientation);

/// WP5 (user ruling R3): the AC-2.6 scratch-failure test hook is DELETED. It
/// forced a scratch checkout to fail so the degradation arm could be
/// exercised; there is no scratch checkout and no degradation arm.

#ifdef __cplusplus
}
#endif
