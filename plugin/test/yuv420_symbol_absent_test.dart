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
    CeyxDecodePool.debugYuv420UpconvertAvailable = null;
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

  // ---------------------------------------------------------------------
  // mem8 v3 T15a step 6 — the SAME failure, raised at decode-service
  // CONSTRUCTION instead of at first decode. The host cannot ask "can this
  // library service yuv420?" before submitting without a public probe, and a
  // failure that only appears on the first photo has already cost the user a
  // load by the time it is reported.
  //
  // Both halves get their own case because they are independently resolved
  // symbol sets: naming the wrong one sends a maintainer to the wrong half of
  // the library, and "decode present, upconvert absent" is a reachable state.
  // ---------------------------------------------------------------------

  test(
    'TC-1350 (T15a.6): checkYuv420Supported throws at CONSTRUCTION time, '
    'naming the decode entry, when the decode half is absent',
    () {
      // setUp already forced both halves absent.
      final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      addTearDown(pool.dispose);

      expect(pool.yuv420DecodeAvailable, isFalse);
      expect(pool.yuv420Available, isFalse);

      Object? caught;
      try {
        pool.checkYuv420Supported();
      } catch (e) {
        caught = e;
      }
      expect(caught, isA<CeyxFormatUnsupportedException>());
      final e = caught! as CeyxFormatUnsupportedException;
      expect(e.format, CeyxOutputFormat.yuv420);
      expect(
        e.missingSymbol,
        'ceyx_decode_into_buffer_format',
        reason:
            'the decode half is checked first: without it there is nothing to '
            'upconvert, so it is the more useful diagnosis when both are gone',
      );
      expect(e.libraryPath, isNotEmpty);
    },
  );

  test(
    'TC-1351 (T15a.6): decode present but upconvert absent names the '
    'UPCONVERT symbol — the half a single bool would have hidden',
    () {
      CeyxDecodePool.debugYuv420Available = true;
      CeyxDecodePool.debugYuv420UpconvertAvailable = false;
      final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      addTearDown(pool.dispose);

      expect(pool.yuv420DecodeAvailable, isTrue);
      expect(pool.yuv420UpconvertAvailable, isFalse);
      expect(
        pool.yuv420Available,
        isFalse,
        reason: 'the ANDed form must not call a half-equipped library usable',
      );

      Object? caught;
      try {
        pool.checkYuv420Supported();
      } catch (e) {
        caught = e;
      }
      expect(caught, isA<CeyxFormatUnsupportedException>());
      expect(
        (caught! as CeyxFormatUnsupportedException).missingSymbol,
        'ceyx_yuv420_to_rgba8',
        reason:
            'reporting the decode entry here would send a maintainer to the '
            'wrong half of the library',
      );
    },
  );

  test(
    'TC-1352 (T15a.6): with both halves present the check is silent and the '
    'getters agree',
    () {
      CeyxDecodePool.debugYuv420Available = true;
      final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      addTearDown(pool.dispose);

      expect(pool.yuv420DecodeAvailable, isTrue);
      expect(
        pool.yuv420UpconvertAvailable,
        isTrue,
        reason:
            'the upconvert half falls back to debugYuv420Available when its '
            'own override is null, so the existing seam still governs both',
      );
      expect(pool.yuv420Available, isTrue);
      expect(pool.checkYuv420Supported, returnsNormally);
    },
  );
}
