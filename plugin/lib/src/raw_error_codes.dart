/// Dart mirror of the generic-RAW error contract.
///
/// Source of truth: `native/include/raw_pipeline_contract.h`
/// (enum `RawErrorCode` and `raw_error_name()`). Any value or spelling change
/// there MUST be reflected here — `test/raw_error_codes_test.dart` enforces it.
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

  /// Mirrors `raw_error_name()` string for string, including the fallback,
  /// so Dart-side telemetry is comparable with native log lines.
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

  /// True when [code] belongs to the RAW scale (<= -201). Deliberately not
  /// limited to the twelve named values: a future native code below -211 must
  /// still be classified RAW rather than misread as a DNG error.
  static bool isRawError(int code) => code <= -201;
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

/// The loaded native library does not export `raw_decode_and_process`
/// (an old dylib, or one built with `-DDNG_ENABLE_GENERIC_RAW=OFF`).
///
/// Thrown instead of crashing, and instead of silently falling back to the
/// DNG parser — a RAW file fed to the DNG parser fails with a misleading code.
class RawUnavailableException implements Exception {
  final String filePath;

  RawUnavailableException(this.filePath);

  @override
  String toString() =>
      'RawUnavailableException: the loaded native library does not export '
      'raw_decode_and_process; cannot decode $filePath';
}
