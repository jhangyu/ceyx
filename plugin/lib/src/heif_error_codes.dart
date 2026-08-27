/// Dart mirror of the HEIF decode error contract.
///
/// Source of truth: `native/include/heif_error_codes.h` (enum `HeifErrorCode`
/// and `heif_error_name()`). Any value or spelling change there MUST be
/// reflected here — `test/heif_error_codes_test.dart` enforces it.
///
/// HEIF codes start at -301 precisely so they can never collide with
/// [DngErrorCode] (0, -1..-8, -100, -101) or [RawErrorCode] (<= -201) inside a
/// shared `int32_t` error field.
library;

abstract final class HeifErrorCode {
  static const int success = 0;
  static const int nullPath = -301;
  static const int openFailed = -302;
  static const int noPrimaryItem = -303;
  static const int unsupportedCodec = -304;
  static const int decodeFailed = -305;
  static const int colorConversion = -306;
  static const int allocationFailed = -307;
  static const int sizeOverflow = -308;
  static const int metadataInvalid = -309;
  static const int unknownException = -310;

  /// Mirrors `heif_error_name()` string for string, including the fallback, so
  /// Dart-side telemetry is comparable with native log lines.
  static String name(int code) {
    switch (code) {
      case success:
        return 'kHeifSuccess';
      case nullPath:
        return 'kHeifErrNullPath';
      case openFailed:
        return 'kHeifErrOpenFailed';
      case noPrimaryItem:
        return 'kHeifErrNoPrimaryItem';
      case unsupportedCodec:
        return 'kHeifErrUnsupportedCodec';
      case decodeFailed:
        return 'kHeifErrDecodeFailed';
      case colorConversion:
        return 'kHeifErrColorConversion';
      case allocationFailed:
        return 'kHeifErrAllocationFailed';
      case sizeOverflow:
        return 'kHeifErrSizeOverflow';
      case metadataInvalid:
        return 'kHeifErrMetadataInvalid';
      case unknownException:
        return 'kHeifErrUnknownException';
      default:
        return 'kHeifErrUnknown';
    }
  }

  /// True when [code] belongs to the HEIF scale (<= -301). Deliberately not
  /// limited to the named values: a future native code below -310 must still
  /// be attributed to this subsystem rather than to an unknown one.
  static bool isHeifError(int code) => code <= -301;
}

/// Thrown when a HEIC decode fails for a reason the native side reported.
class HeifDecodeException implements Exception {
  HeifDecodeException(this.errorCode, this.name, this.message);

  final int errorCode;
  final String name;
  final String message;

  @override
  String toString() => 'HeifDecodeException($errorCode $name): $message';
}

/// Thrown when this build of the native library has no HEIF route at all
/// (built with `-DDNG_ENABLE_HEIF=OFF`, or an older dylib).
///
/// This is deliberately NOT the same thing as Halcyon's D3 "no native decoder"
/// state, which stays reserved for a null decoder: a HEIC on a HEIF-less build
/// must degrade to the ordinary permanent miss, not to a whole-app "decoding
/// unavailable" banner.
class HeifUnavailableException implements Exception {
  HeifUnavailableException(this.path);

  final String path;

  @override
  String toString() =>
      'HeifUnavailableException: this build of dng_decoder_native exports no '
      'HEIF entry points; cannot decode $path';
}
