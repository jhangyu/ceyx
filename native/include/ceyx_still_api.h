#pragma once

/* Still-image decode FFI surface (2026-08-30, codec expansion).
 *
 * ONE decode surface for every still format ceyx can read, routing internally:
 * HEIC/AVIF delegate to the existing libheif implementation (heif_decode.cpp),
 * WebP to libwebp, JXL to libjxl, JPEG to libjpeg-turbo. The pre-existing
 * heif_probe/heif_decode_rgba/heif_release entries in heif_api.h stay exported
 * and unchanged forever; this surface is additive.
 *
 * STILL IMAGES ONLY. An animated WebP, an AVIF sequence or an animated JXL
 * decodes to its FIRST frame and returns success. No error code signals "this
 * was animated" -- the caller cannot act on it, and Halcyon is a still-photo
 * triage tool (supported_photo_formats.dart:35-36).
 *
 * No exception may cross this boundary; every entry validates its pointers.
 */

#include <stdint.h>
#include <stddef.h>

#include "ceyx_encode_api.h"       /* CeyxImageFormat */
#include "ceyx_still_error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Result of ceyx_still_decode_rgba. The caller MUST release it with
/// ceyx_still_release().
///
/// ABI contract: 6 fields, byte-identical layout to HeifResult
/// (heif_api.h:16-23) ON PURPOSE, so the Dart side can share one struct
/// definition and one layout test. Do NOT reorder or insert fields.
/// Layout on 64-bit: sizeof==32, error_code@0, width@4, height@8,
/// orientation@12, rgba@16, rgba_len@24.
typedef struct {
  int32_t  error_code;  ///< 0 = success, negative = a CeyxStillErrorCode
  uint32_t width;       ///< post-transform width in pixels
  uint32_t height;      ///< post-transform height in pixels
  int32_t  orientation; ///< always 1 -- see the orientation contract below
  uint8_t *rgba;        ///< RGBA8 interleaved (width*height*4 bytes), or NULL
  int64_t  rgba_len;    ///< exactly width*height*4 on success, else 0
} CeyxStillResult;

/// Returns 1 if this build can DECODE `format`, 0 if it cannot, or
/// kCeyxStillErrBadFormat for an unrecognised value.
int32_t ceyx_still_decode_supports(int32_t format);

/// Reads only metadata and reports the POST-TRANSFORM extent, cheaply enough
/// to run before every preview decode.
///
/// `format_hint` is a CeyxImageFormat; kCeyxFormatUnknown (0) means "sniff by
/// content". Halcyon routes by extension but passes 0, so a mislabelled file
/// still decodes.
///
/// ORIENTATION CONTRACT (inherited verbatim from heif_api.h:31-37): the
/// decoder applies container transforms during decode, so delivered pixels are
/// already display-ready and orientation is always 1. The field exists so a
/// later decision about a separate EXIF Orientation tag can change behaviour
/// without an ABI break. Stated limitation: a file carrying ONLY an EXIF
/// Orientation tag and no container transform displays unrotated.
///
/// Returns 0 on success or a negative CeyxStillErrorCode. On any failure the
/// out-parameters are left UNTOUCHED.
int32_t ceyx_still_probe(const char *path, int32_t format_hint,
                         uint32_t *width, uint32_t *height,
                         int32_t *orientation);

/// Decodes the PRIMARY/FIRST frame of `path` to interleaved RGBA8.
///
/// max_dim <= 0 means full size. Otherwise the LONG edge is capped at max_dim
/// with the aspect ratio preserved -- a REQUEST, not a guarantee, matching
/// heif_decode_rgba (heif_api.h:49-52). Callers MUST read back out->width and
/// out->height rather than assume they got what they asked for.
///
/// Returns 0 on success or a negative CeyxStillErrorCode; the same value is
/// also written to out->error_code. `out` must be non-NULL and is fully
/// overwritten.
int32_t ceyx_still_decode_rgba(const char *path, int32_t format_hint,
                               int32_t max_dim, CeyxStillResult *out);

/// Frees the RGBA buffer owned by *r and zeroes the struct. Safe on NULL and
/// safe to call twice. Does NOT free `r` itself -- the struct is
/// caller-allocated (usually calloc on the Dart side), the same contract
/// heif_release states at heif_api.h:58-60.
void ceyx_still_release(CeyxStillResult *r);

#ifdef __cplusplus
}
#endif
