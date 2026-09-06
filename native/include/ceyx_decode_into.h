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

/// Orientation-aware sibling of ceyx_decode_into_buffer. Identical contract
/// EXCEPT: the pixels written to dst have EXIF `exif_orientation` already
/// applied, and result->width/height are the ORIENTED extent.
/// exif_orientation outside 1..8 is treated as 1 (identity), matching the
/// host's own table; it is never an error.
/// ADDITIVE: older binaries lack this symbol. Callers MUST resolve it
/// defensively and fall back to ceyx_decode_into_buffer + host-side rotation.
///
/// DEGRADATION (contractual, not a bug): the transposing orientations (5..8)
/// need one scratch frame, taken from the SAME RGBA pool the decoders use. If
/// that checkout fails, this entry decodes UNORIENTED into dst and reports
/// SUCCESS with the UNSWAPPED extent. Memory pressure must never turn into
/// "the photo will not open"; the caller detects the degradation by comparing
/// the returned extent against the orientation it asked for (that is exactly
/// what the Dart side's appliedOrientation consistency check does).
DngResult *ceyx_decode_into_buffer_oriented(const char *file_path,
                                            int32_t max_dim, uint8_t *dst,
                                            size_t dst_capacity,
                                            int32_t exif_orientation);

/// TEST HOOK — not part of the shipping contract, and deliberately named so.
/// When set non-zero, the scratch checkout inside
/// ceyx_decode_into_buffer_oriented is forced to fail, which is the only way
/// to exercise the degradation arm above without exhausting real memory
/// (AC-2.6). Returns the previous value. Always 0 unless a test set it.
int32_t ceyx_debug_force_scratch_failure(int32_t enable);

#ifdef __cplusplus
}
#endif
