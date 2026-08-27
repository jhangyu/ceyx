#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// HEIF decode error contract.
///
/// The -301 scale is chosen precisely so these can never collide with
/// DngErrorCode (0, -1..-8, -100, -101) or RawErrorCode (<= -201) inside a
/// shared int32_t error field — the same disjointness rule
/// raw_pipeline_contract.h:12-13 states for the RAW scale.
///
/// Any value or spelling change here MUST be mirrored in
/// plugin/lib/src/heif_error_codes.dart; plugin/test/heif_error_codes_test.dart
/// enforces it.
enum HeifErrorCode {
  kHeifSuccess = 0,
  kHeifErrNullPath = -301,          ///< null/empty path or null out-parameter
  kHeifErrOpenFailed = -302,        ///< file unreadable or not an ISO-BMFF/HEIF container
  kHeifErrNoPrimaryItem = -303,     ///< no `pitm` primary image item
  kHeifErrUnsupportedCodec = -304,  ///< no decoder for the coded item
  kHeifErrDecodeFailed = -305,      ///< the HEVC decode itself failed
  kHeifErrColorConversion = -306,   ///< YUV -> interleaved RGBA conversion failed
  kHeifErrAllocationFailed = -307,  ///< out of memory
  kHeifErrSizeOverflow = -308,      ///< extent exceeds the decoded-pixel ceiling
  kHeifErrMetadataInvalid = -309,   ///< non-positive or inconsistent dimensions
  kHeifErrUnknownException = -310   ///< a C++ exception reached the C ABI boundary
};

/// Mirrors the spelling used by the Dart side, for comparable log lines.
const char *heif_error_name(int32_t code);

#ifdef __cplusplus
}
#endif
