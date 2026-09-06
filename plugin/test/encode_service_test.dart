import 'dart:ffi' as ffi;
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart' show calloc;
import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/decode_pool.dart';
import 'package:ceyx/src/encode_bindings.dart';
import 'package:ceyx/src/encode_service.dart';

/// Covers the RGBA8 -> JPEG/WebP encode surface added in commit 1764a8f
/// (native/include/ceyx_encode_api.h). Runs against the real shipped dylib —
/// flutter test's cwd is the package root (plugin/), matching the convention
/// in dng_sized_decode_active_test.dart.
void main() {
  final dylibPath = File(
    'macos/Libraries/libdng_decoder_native.dylib',
  ).absolute.path;

  setUpAll(() {
    expect(
      File(dylibPath).existsSync(),
      isTrue,
      reason: 'shipped dylib missing at $dylibPath',
    );
  });

  // 2x2 opaque red RGBA8 frame — enough pixels to exercise both codecs
  // without depending on a real decoded fixture.
  Uint8List redFrame() {
    final bytes = Uint8List(2 * 2 * 4);
    for (var i = 0; i < 4; i++) {
      bytes[i * 4 + 0] = 255; // R
      bytes[i * 4 + 1] = 0; // G
      bytes[i * 4 + 2] = 0; // B
      bytes[i * 4 + 3] = 255; // A
    }
    return bytes;
  }

  test('encodeJpegNative produces a buffer starting with the JPEG SOI marker', () async {
    final service = CeyxEncodeService(libraryPath: dylibPath);
    final jpeg = await service.encodeJpegNative(
      redFrame(),
      width: 2,
      height: 2,
      quality: 80,
    );

    expect(jpeg.length, greaterThan(2));
    expect(jpeg[0], equals(0xFF));
    expect(jpeg[1], equals(0xD8));
  });

  test('encodeWebpNative produces a buffer with RIFF/WEBP magic', () async {
    final service = CeyxEncodeService(libraryPath: dylibPath);
    final webp = await service.encodeWebpNative(
      redFrame(),
      width: 2,
      height: 2,
      quality: 80,
    );

    expect(webp.length, greaterThan(12));
    expect(String.fromCharCodes(webp.sublist(0, 4)), equals('RIFF'));
    expect(String.fromCharCodes(webp.sublist(8, 12)), equals('WEBP'));
  });

  test('encodeJpegNative throws CeyxEncodeException on bad dimensions', () async {
    final service = CeyxEncodeService(libraryPath: dylibPath);
    await expectLater(
      service.encodeJpegNative(redFrame(), width: 0, height: 2, quality: 80),
      throwsA(
        isA<CeyxEncodeException>().having(
          (e) => e.errorCode,
          'errorCode',
          equals(CeyxEncodeErrorCode.badDimensions),
        ),
      ),
    );
  });

  test(
    'memoized unavailability short-circuits without spawning a worker isolate',
    () async {
      CeyxEncodeService.resetAvailabilityCacheForTesting();
      addTearDown(CeyxEncodeService.resetAvailabilityCacheForTesting);

      const probePath = '/nonexistent/only-for-this-test.dylib';
      CeyxEncodeService.debugMarkUnavailableForTesting(probePath);
      final before = CeyxEncodeService.debugIsolateSpawnCount;

      final service = CeyxEncodeService(libraryPath: probePath);
      await expectLater(
        service.encodeJpegNative(
          redFrame(),
          width: 2,
          height: 2,
          quality: 80,
        ),
        throwsA(isA<CeyxEncodeUnavailableException>()),
      );
      await expectLater(
        service.encodeWebpNative(
          redFrame(),
          width: 2,
          height: 2,
          quality: 80,
        ),
        throwsA(isA<CeyxEncodeUnavailableException>()),
      );

      expect(
        CeyxEncodeService.debugIsolateSpawnCount,
        equals(before),
        reason:
            'a memoized-unavailable libraryPath must not spawn a worker '
            'isolate to re-probe',
      );
    },
  );

  test(
    'repeated calls against the real dylib each spawn a worker isolate and '
    'return equivalent, independently valid results',
    () async {
      CeyxEncodeService.resetAvailabilityCacheForTesting();
      final service = CeyxEncodeService(libraryPath: dylibPath);
      final before = CeyxEncodeService.debugIsolateSpawnCount;

      final first = await service.encodeJpegNative(
        redFrame(),
        width: 2,
        height: 2,
        quality: 80,
      );
      final second = await service.encodeJpegNative(
        redFrame(),
        width: 2,
        height: 2,
        quality: 80,
      );

      expect(first, equals(second));
      expect(CeyxEncodeService.debugIsolateSpawnCount, equals(before + 2));
    },
  );

  test('encodeJpegNative throws CeyxEncodeException on bad quality', () async {
    final service = CeyxEncodeService(libraryPath: dylibPath);
    await expectLater(
      service.encodeJpegNative(
        redFrame(),
        width: 2,
        height: 2,
        quality: 200,
      ),
      throwsA(
        isA<CeyxEncodeException>().having(
          (e) => e.errorCode,
          'errorCode',
          equals(CeyxEncodeErrorCode.badQuality),
        ),
      ),
    );
  });

  // -------------------------------------------------------------------
  // WP3a: encodeJpegFromNativeRgba — the pointer-accepting encode entry.
  //
  // AC3a.1/AC3a.2 dispatch to `CeyxDecodePool.shared`, a process-wide
  // singleton that resolves its dylib via `DngNativeBindings.load()`'s
  // default candidate search (NOT via `CeyxEncodeService`'s own
  // `libraryPath`, which the pointer path deliberately does not thread —
  // see the class dartdoc). That search does not include the test-process
  // working directory, so these two tests require `DNG_NATIVE_BUILD_DIR` to
  // be set to the directory containing the shipped dylib when invoking
  // `flutter test`, e.g.:
  //   DNG_NATIVE_BUILD_DIR="$(pwd)/macos/Libraries" flutter test
  //       test/encode_service_test.dart
  // The mechanical proof they still ran against the REAL dylib (rather than
  // silently no-op'ing) is the JPEG SOI/EOI byte assertion itself: a stub or
  // an unavailable pool would throw, not return well-formed JPEG bytes.
  // -------------------------------------------------------------------

  ffi.Pointer<ffi.Uint8> callocRedFrame(int width, int height) {
    final buf = calloc<ffi.Uint8>(width * height * 4);
    final view = buf.asTypedList(width * height * 4);
    for (var i = 0; i < width * height; i++) {
      view[i * 4 + 0] = 255;
      view[i * 4 + 1] = 0;
      view[i * 4 + 2] = 0;
      view[i * 4 + 3] = 255;
    }
    return buf;
  }

  test(
    'encodeJpegFromNativeRgba produces a well-formed JPEG from a native '
    'buffer without spawning a per-call isolate',
    () async {
      final buf = callocRedFrame(4, 4);
      addTearDown(() => calloc.free(buf));
      final poolSpawnBefore = CeyxDecodePool.shared.debugIsolateSpawnCount;
      final serviceSpawnBefore = CeyxEncodeService.debugIsolateSpawnCount;

      final service = CeyxEncodeService();
      final jpeg = await service.encodeJpegFromNativeRgba(
        rgbaAddress: buf.address,
        width: 4,
        height: 4,
        quality: 70,
      );

      expect(jpeg.sublist(0, 2), equals([0xFF, 0xD8]));
      expect(jpeg.sublist(jpeg.length - 2), equals([0xFF, 0xD9]));
      expect(
        CeyxEncodeService.debugIsolateSpawnCount,
        equals(serviceSpawnBefore),
        reason:
            'the pointer entry must not spawn through '
            "CeyxEncodeService's own Isolate.run path",
      );

      // A second call must not grow the POOL's spawn count either — the
      // whole point of dispatching to an already-running worker.
      final jpeg2 = await service.encodeJpegFromNativeRgba(
        rgbaAddress: buf.address,
        width: 4,
        height: 4,
        quality: 70,
      );
      expect(jpeg2.sublist(0, 2), equals([0xFF, 0xD8]));
      expect(
        CeyxDecodePool.shared.debugIsolateSpawnCount - poolSpawnBefore,
        lessThanOrEqualTo(1),
        reason:
            'at most ONE worker isolate should ever be spawned for the '
            'pool across any number of encode calls after warmup',
      );
    },
    // Real-dylib-through-the-shared-pool integration test; skipped when the
    // environment override documented above is absent so the suite still
    // runs green on a host that has not set it, rather than failing with a
    // confusing pool-unavailable error.
    skip:
        Platform.environment['DNG_NATIVE_BUILD_DIR'] == null
            ? 'set DNG_NATIVE_BUILD_DIR to macos/Libraries to exercise '
                  'CeyxDecodePool.shared against the real dylib'
            : false,
  );

  test('encodeJpegFromNativeRgba throws ArgumentError for a zero address', () async {
    final service = CeyxEncodeService();
    await expectLater(
      service.encodeJpegFromNativeRgba(
        rgbaAddress: 0,
        width: 4,
        height: 4,
        quality: 70,
      ),
      throwsA(isA<ArgumentError>()),
    );
  });

  test(
    'encodeJpegFromNativeRgba memoizes unavailability and short-circuits '
    'without touching the pool',
    () async {
      CeyxEncodeService.resetAvailabilityCacheForTesting();
      addTearDown(CeyxEncodeService.resetAvailabilityCacheForTesting);

      const probePath = '/nonexistent/only-for-this-test-wp3a.dylib';
      CeyxEncodeService.debugMarkUnavailableForTesting(probePath);
      final serviceSpawnBefore = CeyxEncodeService.debugIsolateSpawnCount;
      final poolSpawnBefore = CeyxDecodePool.shared.debugIsolateSpawnCount;

      final service = CeyxEncodeService(libraryPath: probePath);
      await expectLater(
        service.encodeJpegFromNativeRgba(
          rgbaAddress: 0xDEAD,
          width: 4,
          height: 4,
          quality: 70,
        ),
        throwsA(isA<CeyxEncodeUnavailableException>()),
      );
      // A second call must short-circuit from the memo too.
      await expectLater(
        service.encodeJpegFromNativeRgba(
          rgbaAddress: 0xDEAD,
          width: 4,
          height: 4,
          quality: 70,
        ),
        throwsA(isA<CeyxEncodeUnavailableException>()),
      );

      expect(
        CeyxEncodeService.debugIsolateSpawnCount,
        equals(serviceSpawnBefore),
      );
      expect(
        CeyxDecodePool.shared.debugIsolateSpawnCount,
        equals(poolSpawnBefore),
        reason:
            'a memoized-unavailable libraryPath must not touch the pool at '
            'all, let alone spawn a worker',
      );
    },
  );
}
