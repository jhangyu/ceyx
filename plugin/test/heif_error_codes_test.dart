import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/heif_error_codes.dart';
import 'package:ceyx/src/raw_error_codes.dart';

void main() {
  test('HEIF codes are disjoint from the DNG and RAW scales', () {
    const heifCodes = <int>[
      HeifErrorCode.nullPath,
      HeifErrorCode.openFailed,
      HeifErrorCode.noPrimaryItem,
      HeifErrorCode.unsupportedCodec,
      HeifErrorCode.decodeFailed,
      HeifErrorCode.colorConversion,
      HeifErrorCode.allocationFailed,
      HeifErrorCode.sizeOverflow,
      HeifErrorCode.metadataInvalid,
      HeifErrorCode.unknownException,
    ];
    // DNG occupies 0, -1..-8, -100, -101; RAW occupies <= -201 down to -211.
    // The HEIF block starts at -301 so a value can be attributed to exactly
    // one subsystem by inspection, which is what makes a shared int32 error
    // field safe.
    for (final code in heifCodes) {
      expect(code, lessThanOrEqualTo(-301));
      expect(RawErrorCode.isRawError(code), isFalse,
          reason: '$code must not be claimed by the RAW scale');
      expect(HeifErrorCode.isHeifError(code), isTrue);
    }
    expect(heifCodes.toSet(), hasLength(heifCodes.length),
        reason: 'no two HEIF codes may share a value');
  });

  test('names mirror heif_error_name() spelling for comparable log lines', () {
    expect(HeifErrorCode.name(HeifErrorCode.success), 'kHeifSuccess');
    expect(HeifErrorCode.name(HeifErrorCode.decodeFailed), 'kHeifErrDecodeFailed');
    expect(HeifErrorCode.name(-999), 'kHeifErrUnknown');
  });
}
