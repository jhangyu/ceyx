#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Still-image decode error contract.
///
/// The -501 scale is chosen so these can never collide with DngErrorCode
/// (0, -1..-8, -100, -101), RawErrorCode (<= -201), HeifErrorCode (-301..-310)
/// or CeyxEncodeErrorCode (-401..-411) inside a shared int32_t error field --
/// the disjointness rule stated in raw_pipeline_contract.h:12-13 and
/// heif_error_codes.h:11-14.
///
/// Any value or spelling change here MUST be mirrored in
/// plugin/lib/src/still_error_codes.dart; plugin/test/still_error_codes_test.dart
/// enforces it by parsing THIS file.
enum CeyxStillErrorCode {
  kCeyxStillSuccess             = 0,
  kCeyxStillErrNullPath         = -501, /**< null/empty path or null out-parameter */
  kCeyxStillErrOpenFailed       = -502, /**< unreadable, or not the claimed container */
  kCeyxStillErrBadFormat        = -503, /**< format_hint is not a CeyxImageFormat value */
  kCeyxStillErrUnsupported      = -504, /**< codec not compiled into this build */
  kCeyxStillErrNoPrimaryItem    = -505, /**< container carries no primary image */
  kCeyxStillErrDecodeFailed     = -506, /**< the codec itself refused the bitstream */
  kCeyxStillErrColorConversion  = -507, /**< conversion to interleaved RGBA8 failed */
  kCeyxStillErrAllocationFailed = -508, /**< out of memory */
  kCeyxStillErrSizeOverflow     = -509, /**< extent overflows int64 pixel arithmetic */
  kCeyxStillErrMetadataInvalid  = -510, /**< non-positive or inconsistent dimensions */
  kCeyxStillErrUnknownException = -511  /**< a C++ exception reached the C ABI */
};

/** Mirrors the spelling used by the Dart side, for comparable log lines. */
const char *ceyx_still_error_name(int32_t code);

#ifdef __cplusplus
}
#endif
