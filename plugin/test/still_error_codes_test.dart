import 'dart:io';

import 'package:ceyx/src/still_error_codes.dart';
import 'package:flutter_test/flutter_test.dart';

/// Parses `enum CeyxStillErrorCode { kName = -50x, ... }` out of the C header
/// and asserts the Dart mirror agrees. Restating the numbers here instead
/// would make this test pass forever while the two sides diverge.
Map<String, int> _parseHeader(String source) {
  final start = source.indexOf('enum CeyxStillErrorCode');
  expect(start, isNot(-1), reason: 'enum CeyxStillErrorCode not found');
  final body = source.substring(
    source.indexOf('{', start) + 1,
    source.indexOf('}', start),
  );
  final re = RegExp(r'(kCeyxStill\w+)\s*=\s*(-?\d+)');
  return {
    for (final m in re.allMatches(body)) m.group(1)!: int.parse(m.group(2)!),
  };
}

void main() {
  late Map<String, int> cValues;

  setUpAll(() {
    final header =
        File('../native/include/ceyx_still_error_codes.h').readAsStringSync();
    cValues = _parseHeader(header);
  });

  test('every C code has a Dart mirror with the same value', () {
    final dartValues = <String, int>{
      'kCeyxStillSuccess': CeyxStillErrorCode.success,
      'kCeyxStillErrNullPath': CeyxStillErrorCode.nullPath,
      'kCeyxStillErrOpenFailed': CeyxStillErrorCode.openFailed,
      'kCeyxStillErrBadFormat': CeyxStillErrorCode.badFormat,
      'kCeyxStillErrUnsupported': CeyxStillErrorCode.unsupported,
      'kCeyxStillErrNoPrimaryItem': CeyxStillErrorCode.noPrimaryItem,
      'kCeyxStillErrDecodeFailed': CeyxStillErrorCode.decodeFailed,
      'kCeyxStillErrColorConversion': CeyxStillErrorCode.colorConversion,
      'kCeyxStillErrAllocationFailed': CeyxStillErrorCode.allocationFailed,
      'kCeyxStillErrSizeOverflow': CeyxStillErrorCode.sizeOverflow,
      'kCeyxStillErrMetadataInvalid': CeyxStillErrorCode.metadataInvalid,
      'kCeyxStillErrUnknownException': CeyxStillErrorCode.unknownException,
    };
    expect(cValues.length, 12, reason: 'header gained or lost a code');
    for (final entry in cValues.entries) {
      expect(dartValues, contains(entry.key),
          reason: '${entry.key} exists in C but has no Dart mirror');
      expect(dartValues[entry.key], entry.value,
          reason: '${entry.key} value drifted');
    }
  });

  test('still codes are disjoint from every other scale', () {
    // The whole point of the -501 scale: a shared int32 error field must never
    // be ambiguous. -401..-411 is encode, -301..-310 heif, <= -201 raw.
    for (final v in cValues.values.where((v) => v != 0)) {
      expect(v, lessThanOrEqualTo(-501));
      expect(v, greaterThanOrEqualTo(-599));
    }
  });
}
