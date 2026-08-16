// W5 (M-6) regression test: keeps the Dart mirror `DngErrorCode`
// (lib/src/dng_decoder_service.dart) in lockstep with the C source of
// truth `dng_processor/native/include/dng_error_codes.h`.
//
// This test does NOT load the native dylib (pure Dart constant check),
// so it is safe to run in CI / any environment without a native build.
//
// If native/include/dng_error_codes.h changes, this test (and the Dart
// mirror) must be updated in the same change — see the header's own
// "Any addition or value change here MUST be reflected in the Dart
// mirror" comment.
import 'package:flutter_test/flutter_test.dart';
import 'package:dng_processor/dng_processor.dart';

void main() {
  group('DngErrorCode Dart/C mirror contract', () {
    test('values match native/include/dng_error_codes.h', () {
      // Expected values transcribed from the C enum DngErrorCode.
      const expected = <String, int>{
        'success': 0,
        'nullPath': -1,
        'parseFailed': -2,
        'stage3Failed': -3,
        'stage4Failed': -4,
        'stage2HandoffRestoreFailed': -5,
        'gpuUnavailable': -6,
        'rgbaAllocFailed': -7,
        'ol2DispatchFailed': -8,
        'stdException': -100,
        'unknownException': -101,
      };

      final actual = <String, int>{
        'success': DngErrorCode.success,
        'nullPath': DngErrorCode.nullPath,
        'parseFailed': DngErrorCode.parseFailed,
        'stage3Failed': DngErrorCode.stage3Failed,
        'stage4Failed': DngErrorCode.stage4Failed,
        'stage2HandoffRestoreFailed': DngErrorCode.stage2HandoffRestoreFailed,
        'gpuUnavailable': DngErrorCode.gpuUnavailable,
        'rgbaAllocFailed': DngErrorCode.rgbaAllocFailed,
        'ol2DispatchFailed': DngErrorCode.ol2DispatchFailed,
        'stdException': DngErrorCode.stdException,
        'unknownException': DngErrorCode.unknownException,
      };

      expect(actual, expected);
    });

    test('all values are unique (no accidental collisions)', () {
      final values = <int>[
        DngErrorCode.success,
        DngErrorCode.nullPath,
        DngErrorCode.parseFailed,
        DngErrorCode.stage3Failed,
        DngErrorCode.stage4Failed,
        DngErrorCode.stage2HandoffRestoreFailed,
        DngErrorCode.gpuUnavailable,
        DngErrorCode.rgbaAllocFailed,
        DngErrorCode.ol2DispatchFailed,
        DngErrorCode.stdException,
        DngErrorCode.unknownException,
      ];
      expect(values.toSet().length, values.length);
    });

    test('DngDecodeException formats errorCode and message', () {
      final e = DngDecodeException(DngErrorCode.stage3Failed, 'boom');
      expect(e.errorCode, -3);
      expect(e.toString(), contains('-3'));
      expect(e.toString(), contains('boom'));
    });
  });
}
