/// Dart mirror of `CeyxStillErrorCode` (native/include/ceyx_still_error_codes.h).
/// Any value or spelling change there MUST be reflected here;
/// test/still_error_codes_test.dart enforces it by parsing the header.
abstract final class CeyxStillErrorCode {
  static const int success = 0;
  static const int nullPath = -501;
  static const int openFailed = -502;
  static const int badFormat = -503;
  static const int unsupported = -504;
  static const int noPrimaryItem = -505;
  static const int decodeFailed = -506;
  static const int colorConversion = -507;
  static const int allocationFailed = -508;
  static const int sizeOverflow = -509;
  static const int metadataInvalid = -510;
  static const int unknownException = -511;
}

/// Thrown when a native still-decode call fails.
class CeyxStillDecodeException implements Exception {
  final int errorCode;
  final String errorName;
  CeyxStillDecodeException(this.errorCode, this.errorName);
  @override
  String toString() => 'CeyxStillDecodeException($errorCode $errorName)';
}

/// Thrown when the loaded native library does not export the still-decode
/// symbols (an older drop, or a build without them).
class CeyxStillUnavailableException implements Exception {
  @override
  String toString() =>
      'CeyxStillUnavailableException: native still-decode symbols not found '
      'in the loaded dylib';
}
