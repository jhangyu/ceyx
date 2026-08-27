#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Result of heif_decode_rgba. The caller MUST free it with heif_release().
///
/// ABI contract: 6 fields; do NOT reorder or insert fields in the middle.
/// Layout on 64-bit: sizeof==32, error_code@0, width@4, height@8,
/// orientation@12, rgba@16, rgba_len@24.
/// Any change here MUST be mirrored in HeifResult (heif_bindings.dart).
/// See also: Gotcha #58 (memory.md) — a field-count mismatch was a past bug.
typedef struct {
  int32_t error_code;  ///< 0 = success, negative = a HeifErrorCode
  uint32_t width;      ///< post-transform width in pixels
  uint32_t height;     ///< post-transform height in pixels
  int32_t orientation; ///< always 1 — see the orientation contract below
  uint8_t *rgba;       ///< RGBA8 interleaved (width*height*4 bytes), or NULL
  int64_t rgba_len;    ///< exactly width*height*4 on success, else 0
} HeifResult;

/// Reads only the metadata boxes of the primary item and reports its
/// POST-TRANSFORM extent, cheaply enough to run before every preview decode.
///
/// Used by the Dart loader to satisfy the decoded-pixel budget check and to
/// fill NativeImageNeedsRawDecode.exifOrientation before any decode happens.
///
/// ORIENTATION CONTRACT (phase 2 decision, deliberately narrow):
/// libheif applies the container's `irot`/`imir` transform properties during
/// decode, so the pixels this API delivers are ALREADY display-ready and
/// orientation is always 1. The field exists so that a later decision about
/// a HEIC's separate EXIF Orientation tag can change behaviour without an ABI
/// break. Stated limitation: a HEIC carrying ONLY an EXIF Orientation tag and
/// no irot property will display unrotated.
///
/// Returns 0 on success or a negative HeifErrorCode. On any failure the
/// out-parameters are left untouched.
int32_t heif_probe(const char *path, uint32_t *width, uint32_t *height,
                   int32_t *orientation);

/// Decodes the PRIMARY item of a HEIC/HEIF file to interleaved RGBA8.
///
/// Multi-image files (bursts, Live Photos, depth/auxiliary images) yield the
/// `pitm` primary item only; HDR gain maps and depth maps are ignored.
///
/// max_dim <= 0 means full size. Otherwise the LONG edge is capped at max_dim
/// with the aspect ratio preserved — a REQUEST, not a guarantee, matching
/// DngSizedDecoder's documented semantics. Callers must read back
/// out->width/out->height rather than assume they got what they asked for.
///
/// Returns 0 on success or a negative HeifErrorCode; the same value is also
/// written to out->error_code. `out` must be non-NULL and is fully overwritten.
int32_t heif_decode_rgba(const char *path, int32_t max_dim, HeifResult *out);

/// Frees the RGBA buffer owned by *r and zeroes the struct. Safe on a NULL
/// pointer and safe to call twice. Does NOT free `r` itself — the struct is
/// caller-allocated (usually on the Dart side via calloc), unlike DngResult.
void heif_release(HeifResult *r);

#ifdef __cplusplus
}
#endif
