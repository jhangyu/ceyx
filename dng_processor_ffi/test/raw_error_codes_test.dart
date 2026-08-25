// Keeps the Dart mirror RawErrorCode in lockstep with the C source of truth
// dng_processor/native/include/raw_pipeline_contract.h. Pure Dart: no dylib.
import 'package:flutter_test/flutter_test.dart';

import 'package:dng_processor_ffi/src/raw_error_codes.dart';

void main() {
  group('RawErrorCode Dart/C mirror contract', () {
    test('values match native/include/raw_pipeline_contract.h', () {
      const expected = <String, int>{
        'success': 0,
        'nullPath': -201,
        'probeFailed': -202,
        'parseFailed': -203,
        'unpackFailed': -204,
        'layoutUnsupported': -205,
        'metadataInvalid': -206,
        'gpuUnavailable': -207,
        'kernelFailed': -208,
        'allocationFailed': -209,
        'sizeOverflow': -210,
        'cancelled': -211,
      };

      final actual = <String, int>{
        'success': RawErrorCode.success,
        'nullPath': RawErrorCode.nullPath,
        'probeFailed': RawErrorCode.probeFailed,
        'parseFailed': RawErrorCode.parseFailed,
        'unpackFailed': RawErrorCode.unpackFailed,
        'layoutUnsupported': RawErrorCode.layoutUnsupported,
        'metadataInvalid': RawErrorCode.metadataInvalid,
        'gpuUnavailable': RawErrorCode.gpuUnavailable,
        'kernelFailed': RawErrorCode.kernelFailed,
        'allocationFailed': RawErrorCode.allocationFailed,
        'sizeOverflow': RawErrorCode.sizeOverflow,
        'cancelled': RawErrorCode.cancelled,
      };

      expect(actual, expected);
    });

    test('name() mirrors raw_error_name()', () {
      expect(RawErrorCode.name(0), 'kRawSuccess');
      expect(RawErrorCode.name(-201), 'kRawErrNullPath');
      expect(RawErrorCode.name(-202), 'kRawErrProbeFailed');
      expect(RawErrorCode.name(-203), 'kRawErrParseFailed');
      expect(RawErrorCode.name(-204), 'kRawErrUnpackFailed');
      expect(RawErrorCode.name(-205), 'kRawErrLayoutUnsupported');
      expect(RawErrorCode.name(-206), 'kRawErrMetadataInvalid');
      expect(RawErrorCode.name(-207), 'kRawErrGpuUnavailable');
      expect(RawErrorCode.name(-208), 'kRawErrKernelFailed');
      expect(RawErrorCode.name(-209), 'kRawErrAllocationFailed');
      expect(RawErrorCode.name(-210), 'kRawErrSizeOverflow');
      expect(RawErrorCode.name(-211), 'kRawErrCancelled');
      expect(RawErrorCode.name(-999), 'kRawErrUnknown');
    });

    test('isRawError separates RAW codes from DNG codes', () {
      for (final dngCode in <int>[0, -1, -2, -3, -4, -5, -6, -7, -8, -100, -101]) {
        expect(RawErrorCode.isRawError(dngCode), isFalse,
            reason: 'DNG code $dngCode must not be classified RAW');
      }
      for (var code = -201; code >= -211; code--) {
        expect(RawErrorCode.isRawError(code), isTrue);
      }
      // Codes below the current floor stay RAW-classified by design.
      expect(RawErrorCode.isRawError(-300), isTrue);
    });

    test('cancellation is distinguishable from decode failure', () {
      final cancelled = RawDecodeException(
        RawErrorCode.cancelled,
        RawErrorCode.name(RawErrorCode.cancelled),
        'Decode cancelled by request',
      );
      final failed = RawDecodeException(
        RawErrorCode.parseFailed,
        RawErrorCode.name(RawErrorCode.parseFailed),
        'RAW container parse failed',
      );
      expect(cancelled.isCancelled, isTrue);
      expect(failed.isCancelled, isFalse);
      expect(cancelled.toString(), contains('kRawErrCancelled'));
    });

    test('RawUnavailableException names the offending file', () {
      expect(
        RawUnavailableException('/x/y.raf').toString(),
        contains('/x/y.raf'),
      );
    });
  });
}
