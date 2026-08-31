#pragma once

/* INTERNAL, not part of the shipped ABI. Declares the per-codec
 * implementations that src/ffi/still_ffi_api.cpp and
 * src/ffi/encode_ffi_api.cpp dispatch into.
 *
 * Ownership boundary (2026-08-30 codec expansion plan):
 *   webp_codec.cpp  -> ceyx_webp_*_impl      (Task 8)
 *   jxl_codec.cpp   -> ceyx_jxl_*_impl       (Task 9)
 *   heif_encode.cpp -> ceyx_heif_*_impl      (Task 7)
 *
 * Every impl obeys the same contracts as the public entry that calls it:
 * buffers are malloc'd for the caller, *out is NULL and *out_len is 0 on any
 * failure, and no exception crosses back out. */

#include <stdint.h>
#include <stddef.h>

#include "ceyx_encode_api.h"
#include "ceyx_still_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All ceyx_*_impl entry points below are cross-TU-only (still_ffi_api.cpp /
 * encode_ffi_api.cpp dispatch into them, nothing outside this binary does).
 * They carry hidden visibility (2026-09-01 Ruling 6) so they do not appear in
 * the dylib's exported-symbol table, matching CeyxHeifHasDecoderFor below. */

/* --- WebP (Task 8, webp_codec.cpp) --------------------------------------- */
__attribute__((visibility("hidden")))
int32_t ceyx_webp_probe_impl(const char *path, uint32_t *w, uint32_t *h);
__attribute__((visibility("hidden")))
int32_t ceyx_webp_decode_impl(const char *path, int32_t max_dim,
                              CeyxStillResult *out);
__attribute__((visibility("hidden")))
int32_t ceyx_webp_encode_impl(const uint8_t *rgba, int32_t width, int32_t height,
                              const CeyxEncodeOptions *opts,
                              const CeyxEncodeMetadata *meta,
                              uint8_t **out, size_t *out_len);

/* --- JPEG XL (Task 9, jxl_codec.cpp) ------------------------------------- */
__attribute__((visibility("hidden")))
int32_t ceyx_jxl_probe_impl(const char *path, uint32_t *w, uint32_t *h);
__attribute__((visibility("hidden")))
int32_t ceyx_jxl_decode_impl(const char *path, int32_t max_dim,
                             CeyxStillResult *out);
__attribute__((visibility("hidden")))
int32_t ceyx_jxl_encode_impl(const uint8_t *rgba, int32_t width, int32_t height,
                             const CeyxEncodeOptions *opts,
                             const CeyxEncodeMetadata *meta,
                             uint8_t **out, size_t *out_len);

/* --- HEIC / AVIF (Task 7, heif_encode.cpp) ------------------------------- */
/* `format` is kCeyxFormatHeic or kCeyxFormatAvif; it selects
 * heif_compression_HEVC vs heif_compression_AV1. */
__attribute__((visibility("hidden")))
int32_t ceyx_heif_encode_impl(int32_t format,
                              const uint8_t *rgba, int32_t width, int32_t height,
                              const CeyxEncodeOptions *opts,
                              const CeyxEncodeMetadata *meta,
                              uint8_t **out, size_t *out_len);
__attribute__((visibility("hidden")))
int32_t ceyx_heif_still_decode_impl(const char *path, int32_t max_dim,
                                    CeyxStillResult *out);

/* Per-codec runtime queries (Task 6, 2026-09-01 D3-a probe-semantics split):
 * ask the loaded libheif whether it was actually built with the HEVC (HEIC)
 * or AV1 (AVIF) codec, rather than only whether the HEIF route is compiled
 * in. `format` is kCeyxFormatHeic or kCeyxFormatAvif; any other value (and
 * every value when DNG_ENABLE_HEIF=0) returns 0 without touching libheif.
 *
 * D3-a requires NO new exported FFI symbol: hidden visibility keeps these out
 * of the dylib's exported-symbol table (still_ffi_api.cpp / encode_ffi_api.cpp
 * / test_codec_heif.cpp link the same translation unit and see them as
 * ordinary cross-TU C symbols; nothing outside this binary can look them up). */
__attribute__((visibility("hidden"))) int32_t CeyxHeifHasDecoderFor(int32_t format);
__attribute__((visibility("hidden"))) int32_t CeyxHeifHasEncoderFor(int32_t format);

/* Maps a HeifErrorCode (-301..-310) onto the CeyxStillErrorCode (-501..-511)
 * scale. Defined in heif_encode.cpp (Task 7) but called by still_ffi_api.cpp
 * (Task 8) as well, so it is declared here and is NOT a file-local static.
 * Anything unrecognised maps to kCeyxStillErrDecodeFailed. */
int32_t ceyx_map_heif_to_still_error(int32_t heif_code);

#ifdef __cplusplus
}
#endif
