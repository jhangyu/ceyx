#pragma once

/* RGBA8 -> compressed still-image encode FFI surface (2026-08-30).
 *
 * Motivation: the Flutter host needs an in-process re-encode of a decoded
 * full-size frame (~12.5MP) fast enough to sit inside an interactive path.
 * A pure-Dart JPEG encode of that frame measured 4102 ms; libjpeg-turbo, which
 * this repo already vendors for the DNG SDK's lossy path, measured 62 ms for
 * the same work. WebP is provided for capability parity, not because it is on
 * the hot path.
 *
 * These entries only ENCODE. They never touch the decode pipeline, hold no
 * global state, take no locks, and are safe to call concurrently from
 * different threads (libjpeg's and libwebp's per-call contexts are local).
 *
 * MEMORY OWNERSHIP: on success `*out` is a heap buffer owned by the CALLER and
 * MUST be released with ceyx_encode_free() — not free(), not WebPFree(), so
 * the allocator stays an implementation detail. On any failure `*out` is set
 * to NULL and `*out_len` to 0, so a caller that frees unconditionally is safe.
 *
 * No exception may cross this boundary; every entry validates its pointers.
 * This mirrors the contract src/ffi/dng_ffi_api.cpp and heif_ffi_api.cpp use.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encode error contract.
 *
 * The -401 scale is chosen so these can never collide with DngErrorCode
 * (0, -1..-8, -100, -101), RawErrorCode (<= -201) or HeifErrorCode (-301..-310)
 * inside a shared int32_t error field — the disjointness rule stated in
 * raw_pipeline_contract.h:12-13 and heif_error_codes.h.
 *
 * Any value or spelling change here MUST be mirrored on the Dart side. */
enum CeyxEncodeErrorCode {
  kCeyxEncodeSuccess = 0,
  kCeyxEncodeErrNullArg = -401,       /**< null rgba / out / out_len pointer */
  kCeyxEncodeErrBadDimensions = -402, /**< non-positive or overflowing w/h */
  kCeyxEncodeErrBadQuality = -403,    /**< quality outside 1..100 */
  kCeyxEncodeErrAllocationFailed = -404, /**< out of memory */
  kCeyxEncodeErrEncodeFailed = -405,  /**< the codec itself refused the frame */
  kCeyxEncodeErrUnsupported = -406,   /**< codec not compiled into this build */
  kCeyxEncodeErrUnknownException = -407, /**< a C++ exception reached the ABI */
  /* --- appended 2026-08-30, codec expansion. Append-only: never renumber. --- */
  kCeyxEncodeErrBadOptions       = -408, /**< opts NULL, bad struct_size, reserved != 0 */
  kCeyxEncodeErrMetadataRejected = -409, /**< the container refused the metadata block */
  kCeyxEncodeErrBadFormat        = -410, /**< format is not a CeyxImageFormat value */
  kCeyxEncodeErrLosslessUnsupported = -411 /**< lossless requested, codec cannot */
};

/** Mirrors the spelling used by the Dart side, for comparable log lines. */
const char *ceyx_encode_error_name(int32_t code);

/** Encodes `width` x `height` interleaved RGBA8 (4 bytes/pixel, tightly
 * packed, `width*height*4` bytes readable) as a baseline JPEG.
 *
 * The alpha channel is discarded — JPEG has no alpha. `quality` is the
 * standard libjpeg 1..100 scale; 80 is the host's default.
 *
 * Returns kCeyxEncodeSuccess (0) or a negative CeyxEncodeErrorCode. */
int32_t ceyx_encode_jpeg_rgba8(const uint8_t *rgba, int32_t width,
                               int32_t height, int32_t quality, uint8_t **out,
                               size_t *out_len);

/** Same contract and same pixel layout as ceyx_encode_jpeg_rgba8, producing a
 * lossy WebP (alpha preserved). `quality` is libwebp's 1..100 quality factor.
 *
 * Returns kCeyxEncodeErrUnsupported when the build was configured without the
 * vendored libwebp dist (CEYX_ENABLE_WEBP=0). */
int32_t ceyx_encode_webp_rgba8(const uint8_t *rgba, int32_t width,
                               int32_t height, int32_t quality, uint8_t **out,
                               size_t *out_len);

/* --- Generic multi-format encode surface (2026-08-30) --------------------- */

/** Format selector shared by the encode and still-decode surfaces.
 * Values are append-only and are never reused. */
enum CeyxImageFormat {
  kCeyxFormatUnknown = 0, /**< decode: sniff by content. encode: invalid. */
  kCeyxFormatJpeg    = 1,
  kCeyxFormatWebp    = 2,
  kCeyxFormatHeic    = 3, /**< HEVC in an ISO-BMFF/HEIF container */
  kCeyxFormatAvif    = 4, /**< AV1 in an ISO-BMFF/HEIF container */
  kCeyxFormatJxl     = 5  /**< JPEG XL */
};

/** Encode knobs.
 *
 * The caller MUST zero the whole struct, then set `struct_size` to
 * sizeof(CeyxEncodeOptions) before setting anything else. That field is the
 * forward-compatibility handshake: a smaller value than this build understands
 * is filled with defaults; a larger one is rejected with
 * kCeyxEncodeErrBadOptions. This is what lets a field be appended later
 * without an ABI break.
 *
 * Layout on 64-bit: sizeof == 20, struct_size@0, quality@4, lossless@8,
 * effort@12, reserved0@16. Mirrored by CeyxEncodeOptions in
 * plugin/lib/src/encode_options.dart; tests/test_abi_layout.cpp pins it. */
typedef struct {
  uint32_t struct_size; /**< = sizeof(CeyxEncodeOptions) */
  int32_t  quality;     /**< 1..100. IGNORED when lossless != 0. */
  int32_t  lossless;    /**< 0 = lossy; non-zero = mathematically lossless */
  int32_t  effort;      /**< 0 = codec default; else 1 (fast) .. 10 (slow) */
  int32_t  reserved0;   /**< MUST be 0 */
} CeyxEncodeOptions;

/** Metadata to embed in the produced file.
 *
 * Every pointer/length pair may be (NULL, 0), meaning "do not embed this kind".
 * A non-NULL pointer with zero length, or NULL with a non-zero length, is
 * kCeyxEncodeErrNullArg.
 *
 * exif: the raw EXIF payload starting at the TIFF header ("II*\0" or "MM\0*").
 *       NOT prefixed with the JPEG APP1 "Exif\0\0" marker and NOT prefixed with
 *       JPEG XL's 4-byte tiff-header-offset field -- each format's writer adds
 *       whatever wrapper its own container requires. This one representation
 *       was chosen because all four containers can be derived from it.
 * xmp:  a UTF-8 XMP packet, no wrapper.
 * icc:  raw ICC profile bytes.
 *
 * None of these buffers is retained past the call; the caller may free them
 * immediately on return.
 *
 * Layout on 64-bit: sizeof == 56, struct_size@0 (+4 pad), exif@8, exif_len@16,
 * xmp@24, xmp_len@32, icc@40, icc_len@48. */
typedef struct {
  uint32_t       struct_size; /**< = sizeof(CeyxEncodeMetadata) */
  const uint8_t *exif;
  size_t         exif_len;
  const uint8_t *xmp;
  size_t         xmp_len;
  const uint8_t *icc;
  size_t         icc_len;
} CeyxEncodeMetadata;

/** Capability query. Returns 1 if this build can ENCODE `format`, 0 if it
 * cannot, or kCeyxEncodeErrBadFormat (-410) for an unrecognised value.
 *
 * This is what lets a caller tell "the symbol is missing" (an older library)
 * apart from "the symbol is present but the codec was excluded from this
 * platform's build" WITHOUT paying a full encode. Never allocates, never
 * throws, safe to call from any thread. */
int32_t ceyx_encode_supports(int32_t format);

/** Encodes `width` x `height` interleaved RGBA8 (4 bytes/pixel, tightly
 * packed, width*height*4 bytes readable) as `format`.
 *
 * `opts` MUST be non-NULL. `meta` MAY be NULL, meaning "embed no metadata".
 *
 * MEMORY OWNERSHIP: identical to ceyx_encode_jpeg_rgba8 -- on success `*out` is
 * a heap buffer owned by the CALLER, released ONLY with ceyx_encode_free().
 * On any failure `*out` is NULL and `*out_len` is 0.
 *
 * Alpha: JPEG discards it; WebP, HEIC, AVIF and JXL preserve it (HEIC/AVIF
 * write it as an auxiliary alpha image).
 *
 * Thread safety: no global state, no locks; safe to call concurrently on
 * different threads with different buffers, as ceyx_encode_api.h:12-14 already
 * promises for the two original encoders.
 *
 * Returns kCeyxEncodeSuccess (0) or a negative CeyxEncodeErrorCode. */
int32_t ceyx_encode_rgba8(int32_t format,
                          const uint8_t *rgba, int32_t width, int32_t height,
                          const CeyxEncodeOptions *opts,
                          const CeyxEncodeMetadata *meta,
                          uint8_t **out, size_t *out_len);

/** Releases a buffer handed out by either encoder. NULL-safe. */
void ceyx_encode_free(uint8_t *buffer);

#ifdef __cplusplus
}
#endif
