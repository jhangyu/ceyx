import 'dart:io';

import 'package:ceyx/src/encode_bindings.dart';
import 'package:flutter_test/flutter_test.dart';

/// Parses `enum CeyxEncodeErrorCode { kName = -40x, ... }` out of the C
/// header and asserts the Dart mirror agrees. Restating the numbers here
/// instead would make this test pass forever while the two sides diverge --
/// same shape as still_error_codes_test.dart.
Map<String, int> _parseHeader(String source) {
  final start = source.indexOf('enum CeyxEncodeErrorCode');
  expect(start, isNot(-1), reason: 'enum CeyxEncodeErrorCode not found');
  final body = source.substring(
    source.indexOf('{', start) + 1,
    source.indexOf('}', start),
  );
  final re = RegExp(r'(kCeyxEncode\w+)\s*=\s*(-?\d+)');
  return {
    for (final m in re.allMatches(body)) m.group(1)!: int.parse(m.group(2)!),
  };
}

void main() {
  late Map<String, int> cValues;

  setUpAll(() {
    final header =
        File('../native/include/ceyx_encode_api.h').readAsStringSync();
    cValues = _parseHeader(header);
  });

  test('every C code has a Dart mirror with the same value', () {
    final dartValues = <String, int>{
      'kCeyxEncodeSuccess': CeyxEncodeErrorCode.success,
      'kCeyxEncodeErrNullArg': CeyxEncodeErrorCode.nullArg,
      'kCeyxEncodeErrBadDimensions': CeyxEncodeErrorCode.badDimensions,
      'kCeyxEncodeErrBadQuality': CeyxEncodeErrorCode.badQuality,
      'kCeyxEncodeErrAllocationFailed': CeyxEncodeErrorCode.allocationFailed,
      'kCeyxEncodeErrEncodeFailed': CeyxEncodeErrorCode.encodeFailed,
      'kCeyxEncodeErrUnsupported': CeyxEncodeErrorCode.unsupported,
      'kCeyxEncodeErrUnknownException': CeyxEncodeErrorCode.unknownException,
      'kCeyxEncodeErrBadOptions': CeyxEncodeErrorCode.badOptions,
      'kCeyxEncodeErrMetadataRejected': CeyxEncodeErrorCode.metadataRejected,
      'kCeyxEncodeErrBadFormat': CeyxEncodeErrorCode.badFormat,
      'kCeyxEncodeErrLosslessUnsupported':
          CeyxEncodeErrorCode.losslessUnsupported,
    };
    expect(cValues.length, 12, reason: 'header gained or lost a code');
    for (final entry in cValues.entries) {
      expect(dartValues, contains(entry.key),
          reason: '${entry.key} exists in C but has no Dart mirror');
      expect(dartValues[entry.key], entry.value,
          reason: '${entry.key} value drifted');
    }
  });

  test('encode codes are disjoint from every other scale', () {
    // -401..-411 is encode, -501..-511 still-decode, -301..-310 heif,
    // <= -201 raw -- a shared int32 error field must never be ambiguous.
    for (final v in cValues.values.where((v) => v != 0)) {
      expect(v, lessThanOrEqualTo(-401));
      expect(v, greaterThanOrEqualTo(-411));
    }
  });
}
