import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_decoder_service.dart';
import 'package:ceyx/src/native_buffer_pool.dart';

/// AC-C1 (parking-lot round, 2026-09-20 contract): a decode failure must
/// surface a distinct, non-zero error code through the FFI contract, never a
/// silently-produced "successful" all-black frame.
///
/// This exercises the REAL dylib (not a mock), forcing a failure with a
/// nonexistent DNG path so parseDngFile's `dng_file_stream` open fails before
/// any pixel work runs. The contract under test:
///   1. `_finishPointerTransfer` (dng_decoder_service.dart) throws whenever
///      `result.errorCode != 0`, before any width/height/pixel data can reach
///      the caller — so a caller can never mistake a failed decode's
///      (unwritten, zero-filled) destination buffer for a real black image.
///   2. No native buffer escapes the failure path (pool stays empty).
///
/// `flutter test` runs with cwd == package root (plugin/).
void main() {
  final dylibPath = File(
    'macos/Libraries/libdng_decoder_native.dylib',
  ).absolute.path;

  setUpAll(() {
    expect(
      File(dylibPath).existsSync(),
      isTrue,
      reason: 'missing fixture: $dylibPath',
    );
  });

  test(
    'a nonexistent DNG file throws DngDecodeException with a non-zero '
    'error code instead of silently returning a black frame',
    () {
      final service = DngDecoderService(libraryPath: dylibPath);
      expect(
        () => service.decode('/definitely/missing/does_not_exist.dng'),
        throwsA(
          isA<DngDecodeException>()
              .having((e) => e.errorCode, 'errorCode', isNot(0))
              .having((e) => e.message, 'message', isNotEmpty),
        ),
      );
      // Failure path: no pixel buffer was ever handed to the caller, so
      // nothing is live in the native buffer pool — the strongest available
      // proxy for "no black frame escaped as if it were a successful decode".
      expect(CeyxNativeBufferPool.debugTotalLiveAddresses, 0);
    },
  );
}
