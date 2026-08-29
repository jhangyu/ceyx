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
  kCeyxEncodeErrUnknownException = -407 /**< a C++ exception reached the ABI */
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

/** Releases a buffer handed out by either encoder. NULL-safe. */
void ceyx_encode_free(uint8_t *buffer);

#ifdef __cplusplus
}
#endif
