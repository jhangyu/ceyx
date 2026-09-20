import 'dart:ffi' as ffi;
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ceyx/ceyx.dart';
import 'package:ceyx/src/encode_bindings.dart';
import 'package:flutter_test/flutter_test.dart';

/// Opens the process itself. It exports none of the ceyx symbols, so it is a
/// stand-in for "a library that predates the 2026-09-20 direct-encode
/// contract" without needing to build an old dylib.
ffi.DynamicLibrary _emptyLibrary() => ffi.DynamicLibrary.process();

/// Fake pool worker that answers ONLY encode jobs (both the rgba8 4-element
/// shape and the yuv420 6-element/'yuv420'-tagged shape), without loading any
/// dylib — mirrors `fakeEncodeCapablePoolWorker` in decode_pool_test.dart, but
/// distinguishes the two arms so a test can assert each is dispatched
/// independently rather than one clobbering the other's expected result.
void _fakeYuv420EncodeWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final jobs = ReceivePort();
  jobs.listen((Object? message) {
    final msg = message as List<Object?>;
    if (msg[0] == kMsgShutdown) {
      jobs.close();
      return;
    }
    final requestId = msg[1] as int;
    final type = CeyxPoolJobType.values[msg[2] as int];
    if (type != CeyxPoolJobType.encode) return;
    final args = msg[5] as List<Object?>;
    final isYuv420 = args.length >= 6 && args[5] == 'yuv420';
    final marker = isYuv420 ? 0xA5 : 0x5A;
    poolPort.send(<Object?>[
      kMsgResult,
      requestId,
      TransferableTypedData.fromList([
        Uint8List.fromList([0xFF, 0xD8, marker, 0xFF, 0xD9]),
      ]),
    ]);
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

void main() {
  group('CeyxEncodeYuv420Bindings (guarded, independent lookup)', () {
    test('reports unavailable against a symbol-less library', () {
      final b = CeyxEncodeYuv420Bindings.fromLibrary(_emptyLibrary());
      expect(b.available, isFalse);
    });

    test('constructing never throws', () {
      expect(
        () => CeyxEncodeYuv420Bindings.fromLibrary(_emptyLibrary()),
        returnsNormally,
      );
    });

    test(
      'the legacy rgba8/webp group is unaffected by the yuv420 class '
      'existing',
      () {
        // The hazard this guards: had ceyx_encode_jpeg_yuv420 been appended
        // to CeyxEncodeBindings' try/catch group, a dylib WITH rgba8/webp but
        // WITHOUT yuv420 (every currently pinned build) would report the
        // legacy encoders unavailable too.
        final legacy = CeyxEncodeBindings.fromLibrary(_emptyLibrary());
        final yuv420 = CeyxEncodeYuv420Bindings.fromLibrary(_emptyLibrary());
        expect(legacy.available, isFalse); // no symbols in this library
        expect(yuv420.available, isFalse);
        expect(
          () => CeyxEncodeBindings.fromLibrary(_emptyLibrary()),
          returnsNormally,
        );
      },
    );
  });

  test('CeyxEncodeErrorCode.badBufferSize mirrors -412', () {
    expect(CeyxEncodeErrorCode.badBufferSize, -412);
  });

  group('CeyxDecodePool.submitEncodeYuv420 (wire dispatch, no dylib)', () {
    late CeyxDecodePool pool;

    tearDown(() async {
      await pool.dispose();
    });

    test(
      'dispatches with the trailing yuv420 tag and returns the yuv420 arm\'s '
      'bytes, distinct from a concurrent rgba8 submitEncode',
      () async {
        pool = CeyxDecodePool(width: 2, entryPoint: _fakeYuv420EncodeWorker);
        final rgba8 = pool.submitEncode(
          rgbaAddress: 0x1000,
          width: 2,
          height: 2,
          quality: 80,
        );
        final yuv420 = pool.submitEncodeYuv420(
          srcAddress: 0x2000,
          srcCapacity: 6,
          width: 2,
          height: 2,
          quality: 80,
        );
        final results = await Future.wait([rgba8, yuv420]);
        expect(
          results[0],
          equals(Uint8List.fromList([0xFF, 0xD8, 0x5A, 0xFF, 0xD9])),
          reason: 'the rgba8 arm must not receive the yuv420 tag',
        );
        expect(
          results[1],
          equals(Uint8List.fromList([0xFF, 0xD8, 0xA5, 0xFF, 0xD9])),
          reason: 'the yuv420 arm must be recognised by its trailing tag',
        );
      },
    );

    test(
      'two distinct submitEncodeYuv420 calls never coalesce',
      () async {
        pool = CeyxDecodePool(width: 2, entryPoint: _fakeYuv420EncodeWorker);
        final a = pool.submitEncodeYuv420(
          srcAddress: 0x3000,
          srcCapacity: 6,
          width: 2,
          height: 2,
          quality: 0,
        );
        final b = pool.submitEncodeYuv420(
          srcAddress: 0x3000,
          srcCapacity: 6,
          width: 2,
          height: 2,
          quality: 0,
        );
        await Future.wait([a, b]);
        expect(pool.debugCoalescedCount, equals(0));
      },
    );
  });

  group('CeyxEncodeService.encodeJpegFromNativeYuv420', () {
    setUp(CeyxEncodeService.resetYuv420AvailabilityCacheForTesting);
    tearDown(CeyxEncodeService.resetYuv420AvailabilityCacheForTesting);

    test('rejects a zero srcAddress before touching the pool', () {
      final service = CeyxEncodeService();
      expect(
        () => service.encodeJpegFromNativeYuv420(
          srcAddress: 0,
          srcCapacity: 6,
          width: 2,
          height: 2,
          quality: 80,
        ),
        throwsA(isA<ArgumentError>()),
      );
    });

    test(
      'a memoized unavailable outcome fails fast without dispatching to the '
      'pool',
      () async {
        final service = CeyxEncodeService();
        CeyxEncodeService.debugMarkYuv420UnavailableForTesting(null);
        await expectLater(
          service.encodeJpegFromNativeYuv420(
            srcAddress: 0x4000,
            srcCapacity: 6,
            width: 2,
            height: 2,
            quality: 80,
          ),
          throwsA(isA<CeyxFormatUnsupportedException>()),
        );
      },
    );

    test(
      'the yuv420 memoization is independent of the rgba8/webp cache',
      () {
        CeyxEncodeService.debugMarkYuv420UnavailableForTesting(null);
        CeyxEncodeService.resetAvailabilityCacheForTesting();
        // The rgba8 cache reset must not clear the yuv420 cache seeded above,
        // and vice versa — this is the hazard the two SEPARATE maps exist to
        // prevent (encode_service.dart:213-219).
        final service = CeyxEncodeService();
        expect(
          () => service.encodeJpegFromNativeYuv420(
            srcAddress: 0x5000,
            srcCapacity: 6,
            width: 2,
            height: 2,
            quality: 80,
          ),
          throwsA(isA<CeyxFormatUnsupportedException>()),
        );
      },
    );
  });
}
