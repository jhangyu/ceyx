import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

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
}
