/// Dart mirror of the generic-RAW error contract.
///
/// Source of truth: `native/include/raw_pipeline_contract.h`
/// (enum `RawErrorCode` and `raw_error_name()`). Any value or spelling change
/// there MUST be reflected here. `test/raw_error_codes_test.dart` pins the
/// twelve values mirrored below against a hand-written expectation table; it
/// is a drift guard for those twelve, NOT a completeness check — a code ADDED
/// to the C enum cannot make it fail.
///
/// Currently mirrored: twelve of the fourteen native enumerators (success plus
/// eleven of the thirteen error codes). The two not named
/// here are `kRawErrSizedUnsupported` (-212) and `kRawErrDstTooSmall` (-213).
/// -213 is internal by the WP10 boundary ruling: `ceyx_decode_into_ffi.cpp`
/// rewrites it to `kCeyxErrDstTooSmall` (-301) before it crosses the FFI, so
/// it cannot reach Dart. -212 CAN reach Dart (it is returned verbatim, on
/// builds with no scaled AOT — split Vulkan/Android/Linux); it is still
/// classified RAW by [RawErrorCode.isRawError] and still surfaces as a
/// `RawDecodeException`, only without a name/message of its own.
///
/// RAW codes start at -201 precisely so they can never collide with
/// `DngErrorCode` (0, -1..-8, -100, -101) inside the shared
/// `DngResult.error_code` field.
library;

abstract final class RawErrorCode {
  static const int success = 0;
  static const int nullPath = -201;
  static const int probeFailed = -202;
  static const int parseFailed = -203;
  static const int unpackFailed = -204;
  static const int layoutUnsupported = -205;
  static const int metadataInvalid = -206;
  static const int gpuUnavailable = -207;
  static const int kernelFailed = -208;
  static const int allocationFailed = -209;
  static const int sizeOverflow = -210;
  static const int cancelled = -211;

  /// Mirrors `raw_error_name()` string for string over the twelve codes named
  /// above, including the fallback, so Dart-side telemetry is comparable with
  /// native log lines. One known divergence: -212 returns `kRawErrUnknown`
  /// here while native prints `kRawErrSizedUnsupported`. (-213 is `kRawErrUnknown`
  /// on both sides — native's `raw_error_name()` has no case for it either —
  /// and cannot reach Dart regardless; see the library doc comment.)
  static String name(int code) {
    switch (code) {
      case success:
        return 'kRawSuccess';
      case nullPath:
        return 'kRawErrNullPath';
      case probeFailed:
        return 'kRawErrProbeFailed';
      case parseFailed:
        return 'kRawErrParseFailed';
      case unpackFailed:
        return 'kRawErrUnpackFailed';
      case layoutUnsupported:
        return 'kRawErrLayoutUnsupported';
      case metadataInvalid:
        return 'kRawErrMetadataInvalid';
      case gpuUnavailable:
        return 'kRawErrGpuUnavailable';
      case kernelFailed:
        return 'kRawErrKernelFailed';
      case allocationFailed:
        return 'kRawErrAllocationFailed';
      case sizeOverflow:
        return 'kRawErrSizeOverflow';
      case cancelled:
        return 'kRawErrCancelled';
      default:
        return 'kRawErrUnknown';
    }
  }

  /// True when [code] belongs to the RAW block: the closed range -201 .. -300
  /// (i.e. `code <= -201 && code > -301`).
  ///
  /// Deliberately not limited to the twelve named values: a native code below
  /// -211 must still be classified RAW rather than misread as a DNG error.
  /// This is not hypothetical — -212 (`kRawErrSizedUnsupported`) exists in the
  /// C enum today and is unnamed here; it is this range test, not the name
  /// table, that routes it to `RawDecodeException`. The range is nonetheless
  /// bounded at the bottom, because -301 and
  /// below is the HEIF block (`HeifErrorCode`,
  /// native/include/heif_error_codes.h). An open-ended `code <= -201` also
  /// claimed every HEIF code, which defeats the point of allocating disjoint
  /// blocks: with two open-ended predicates a value in the shared `int32_t`
  /// error field cannot be attributed to exactly one subsystem by inspection.
  static bool isRawError(int code) => code <= -201 && code > -301;
}

/// A generic-RAW decode returned a non-zero [errorCode].
class RawDecodeException implements Exception {
  /// Native `RawErrorCode` value.
  final int errorCode;

  /// `RawErrorCode.name(errorCode)`, e.g. `kRawErrParseFailed`.
  final String errorName;

  /// Human-readable explanation of this specific code.
  final String message;

  RawDecodeException(this.errorCode, this.errorName, this.message);

  /// True for `kRawErrCancelled` (-211): a caller-requested cancellation, not
  /// a decode failure. Kept as a getter so callers never string-match.
  bool get isCancelled => errorCode == RawErrorCode.cancelled;

  @override
  String toString() => 'RawDecodeException($errorCode $errorName): $message';
}

/// The loaded native library cannot decode generic RAW: it does not export the
/// decode-into pair (`ceyx_probe_output_size` + `ceyx_decode_into_buffer`).
/// An old dylib, or one built with `-DDNG_ENABLE_GENERIC_RAW=OFF`.
///
/// WP5: the condition was previously stated in terms of the legacy allocating
/// RAW entry. That entry is deleted, so the old wording described a state that
/// is now true of EVERY build while RAW decoding works — it would have sent a
/// reader hunting for a symbol whose absence is normal.
///
/// Thrown instead of crashing, and instead of silently falling back to the
/// DNG parser — a RAW file fed to the DNG parser fails with a misleading code.
class RawUnavailableException implements Exception {
  final String filePath;

  RawUnavailableException(this.filePath);

  @override
  String toString() =>
      'RawUnavailableException: the loaded native library does not export the '
      'decode-into pair (ceyx_probe_output_size + ceyx_decode_into_buffer); '
      'cannot decode $filePath';
}
