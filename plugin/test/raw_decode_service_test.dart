import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:dng_processor_ffi/src/dng_decoder_service.dart';
import 'package:dng_processor_ffi/src/raw_error_codes.dart';

/// Service-layer contract for the generic RAW route (Phase 18 spec §3.2, §4).
///
/// TEST ORDER MATTERS. The zero-copy tests hand a native buffer to a
/// NativeFinalizer, which only runs on GC — so a zero-copy decode leaves the
/// RGBA pool checked out for the rest of the file. Every pool==0 assertion is
/// therefore declared BEFORE the first zero-copy decode. Do not reorder.
///
/// flutter test runs with cwd == package root (dng_processor_ffi/).
void main() {
  final dylibPath = File(
    'macos/Libraries/libdng_decoder_native.dylib',
  ).absolute.path;
  final rafPath = File(
    '../image_samples/raw_corpus/fuji_xt3.raf',
  ).absolute.path;
  final arwPath = File('../image_samples/raw_sample.arw').absolute.path;
  final dngPath = File(
    '../image_samples/lossless_dng_sample.dng',
  ).absolute.path;

  setUpAll(() {
    for (final p in <String>[dylibPath, rafPath, arwPath, dngPath]) {
      expect(File(p).existsSync(), isTrue, reason: 'missing fixture: $p');
    }
  });

  test(
    'decodeOnWorker() renders raw_sample.arw and leaves the pool empty',
    () async {
      final service = DngDecoderService(libraryPath: dylibPath);
      final image = await service.decodeOnWorker(arwPath);

      expect(image.width, greaterThan(0));
      expect(image.height, greaterThan(0));
      expect(image.rgbaData.length, image.width * image.height * 4);
      // The worker path frees the native buffer deterministically in its
      // finally block, so this assertion does not depend on GC.
      expect(service.poolCheckedOut, 0);
    },
    timeout: const Timeout(Duration(minutes: 3)),
  );

  test(
    'a nonexistent RAW file throws RawDecodeException without leaking',
    () {
      final service = DngDecoderService(libraryPath: dylibPath);
      expect(
        () => service.decode('/definitely/missing/file.raf'),
        throwsA(
          isA<RawDecodeException>()
              .having(
                (e) => e.errorName,
                'errorName',
                anyOf(
                  'kRawErrProbeFailed',
                  'kRawErrParseFailed',
                  'kRawErrNullPath',
                ),
              )
              .having((e) => e.isCancelled, 'isCancelled', isFalse),
        ),
      );
      // Failure path: rgba=null, pool untouched (raw_ffi_api.cpp:46-49).
      expect(service.poolCheckedOut, 0);
    },
  );

  test('an unsupported extension throws DngDecodeException with the '
      'supported list', () {
    final service = DngDecoderService(libraryPath: dylibPath);
    expect(
      () => service.decode('/some/photo.jpg'),
      throwsA(
        isA<DngDecodeException>()
            .having((e) => e.errorCode, 'errorCode', DngErrorCode.parseFailed)
            .having((e) => e.message, 'message', contains('raf'))
            .having((e) => e.message, 'message', contains('dng')),
      ),
    );
    expect(service.poolCheckedOut, 0);
  });

  test(
    'decode() renders fuji_xt3.raf through the RAW route',
    () {
      final service = DngDecoderService(libraryPath: dylibPath);
      expect(service.rawDecodeAvailable, isTrue);

      final image = service.decode(rafPath);

      expect(image.width, greaterThan(0));
      expect(image.height, greaterThan(0));
      expect(image.rgbaData.length, image.width * image.height * 4);
      // Diagnostics are thread_local and this decode ran on THIS isolate.
      final diag = service.lastRawDiagnostics;
      expect(diag, isNotNull);
      expect(diag!.totalMs, greaterThan(0));
    },
    timeout: const Timeout(Duration(minutes: 3)),
  );

  test(
    'the DNG route is unchanged',
    () {
      final service = DngDecoderService(libraryPath: dylibPath);
      final image = service.decode(dngPath);
      expect(image.width, greaterThan(0));
      expect(image.height, greaterThan(0));
    },
    timeout: const Timeout(Duration(minutes: 3)),
  );
}
