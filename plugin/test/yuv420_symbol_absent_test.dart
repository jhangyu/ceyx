import 'package:ceyx/ceyx.dart';
import 'package:flutter_test/flutter_test.dart';

import 'decode_pool_test.dart' show fakePoolWorker;

/// mem8 T14 — R-J, the absent-symbol rule INVERTED from T2's.
///
/// Both halves live in ONE file on purpose, so "yuv420 throws" and "rgba8 is
/// unaffected" cannot drift apart in separate files.
///
/// Why this is the opposite rule to `ceyx_native_idle_shrink`'s (mem8 T2,
/// native_buffer_pool.dart): that symbol is an OPTIMISATION, so a dylib
/// without it leaves the app correct and merely heavier. The yuv420 entries
/// are LOAD-BEARING FOR CORRECTNESS — a library without them cannot produce
/// the pixels the caller is about to interpret as planar yuv, so a silent
/// rgba8 fallback hands back a wrong image with no error, which is strictly
/// worse than a crash. The one-line test: if this symbol is missing, is the
/// app still correct? Yes -> tolerate. No -> throw.
void main() {
  setUp(() {
    // "This library predates T12/T13": the guarded lookups found nothing.
    CeyxDecodePool.debugYuv420Available = false;
  });

  tearDown(() {
    CeyxDecodePool.debugYuv420Available = null;
    CeyxDecodePool.debugDecodeIntoAvailable = null;
  });

  test(
    'TC-1321 (R-J): a yuv420 request on a pre-T12 library throws, never '
    'falls back',
    () async {
      final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      addTearDown(pool.dispose);

      await expectLater(
        pool.decode('/tmp/a.dng', format: CeyxOutputFormat.yuv420),
        throwsA(isA<CeyxFormatUnsupportedException>()),
        reason:
            'a silent rgba8 fallback would hand the caller bytes in a layout '
            'it is about to misinterpret — corrupt, not degraded',
      );
    },
  );

  test(
    'TC-1322 (R-J): the throw names the format, the symbol and the loaded '
    'library path',
    () async {
      final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      addTearDown(pool.dispose);

      Object? caught;
      try {
        await pool.decode('/tmp/a.dng', format: CeyxOutputFormat.yuv420);
      } catch (e) {
        caught = e;
      }

      expect(caught, isA<CeyxFormatUnsupportedException>());
      final e = caught! as CeyxFormatUnsupportedException;
      expect(e.format, CeyxOutputFormat.yuv420);
      expect(
        e.missingSymbol,
        'ceyx_decode_into_buffer_format',
        reason: 'the message must name the entry the library lacks',
      );
      // The path is the field without which the exception's own advice
      // ("re-run the pinned build") cannot be acted on: the libraries are
      // placed per-machine and are not in version control.
      expect(e.libraryPath, isNotEmpty);
      expect(e.toString(), contains('yuv420'));
      expect(e.toString(), contains('ceyx_decode_into_buffer_format'));
    },
  );

  test(
    'TC-1323 (R-B): an rgba8 request on the same library is unaffected',
    () async {
      // The degraded (non-pooled) route, which is what every currently pinned
      // build is actually in — the point is that it still completes.
      CeyxDecodePool.debugDecodeIntoAvailable = false;
      final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      addTearDown(pool.dispose);

      await expectLater(
        pool.submit(CeyxPoolJobType.probe, 'sample.dng'),
        completes,
      );
    },
  );

  test(
    'TC-1324 (R-J): the upconvert throws rather than returning silently',
    () {
      expect(
        () => ceyxYuv420ToRgba8(
          srcAddress: 1,
          srcCapacity: 24,
          dstAddress: 2,
          dstCapacity: 64,
          width: 4,
          height: 4,
        ),
        throwsA(isA<CeyxFormatUnsupportedException>()),
        reason:
            'SR-11: there is ONE upconvert implementation and it is native; '
            'Dart must never open-code a fallback',
      );
    },
  );
}
