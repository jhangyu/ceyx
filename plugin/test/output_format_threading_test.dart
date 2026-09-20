import 'package:ceyx/ceyx.dart';
import 'package:flutter_test/flutter_test.dart';

import 'decode_pool_test.dart' show fakePoolWorker;

/// mem8 T14 (SR-9b) — the output format threaded through the decode pool.
///
/// F3 IS THE LOAD-BEARING CASE. The pool deduplicates on TWO keys that fail in
/// opposite ways when a dimension is forgotten:
///
/// * the job key is a POSITIONAL RECORD — a missing element is a compile
///   error at every construction site, so it needs no test;
/// * the size-cache key is a STRING — an rgba8 probe and a yuv420 probe for
///   the same path+maxDim collide SILENTLY and the second is served the
///   first's byte count. A yuv420 decode handed a 4 B/px figure merely wastes
///   memory; an rgba8 decode handed a 1.5 B/px figure OVERFLOWS ITS SLOT.
///
/// That asymmetry is why the string key gets a dedicated red-first case and
/// the record key gets only the cheap F4 sanity check.
void main() {
  group('CeyxOutputFormat byte counts', () {
    test('TC-1316 (F1): rgba8 is 4 bytes per pixel', () {
      expect(
        ceyxOutputFormatByteCount(CeyxOutputFormat.rgba8, 4080, 3056),
        4080 * 3056 * 4,
      );
    });

    test('TC-1317 (F2): yuv420 even dimensions are 1.5 bytes per pixel', () {
      // 4080*3056 luma = 12,468,480; chroma 2 * (2040*1528) = 6,234,240.
      expect(
        ceyxOutputFormatByteCount(CeyxOutputFormat.yuv420, 4080, 3056),
        12468480 + 2 * (2040 * 1528),
      );
    });

    test('TC-1318 (F2b): yuv420 ODD dimensions round chroma planes UP', () {
      // Hand-computed literals, not a re-implementation of the formula.
      // 5x3: luma 15; chroma planes ceil(5/2) x ceil(3/2) = 3x2 = 6 each.
      // A (w/2)*(h/2) implementation gives 2x1=2 each and UNDER-allocates,
      // which is a heap overrun at decode time, not a miscount.
      expect(ceyxOutputFormatByteCount(CeyxOutputFormat.yuv420, 5, 3), 15 + 12);
      expect(ceyxOutputFormatByteCount(CeyxOutputFormat.yuv420, 4, 3), 12 + 8);
      expect(ceyxOutputFormatByteCount(CeyxOutputFormat.yuv420, 5, 4), 20 + 12);
    });
  });

  group('coalescing keys carry the format', () {
    test(
      'TC-1319 (F3): the size cache does not collide across formats',
      () async {
        final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
        addTearDown(pool.dispose);

        pool.debugSeedSizeCache(
          '/tmp/a.dng',
          2800,
          100,
          CeyxOutputFormat.yuv420,
        );

        expect(
          pool.debugSizeCacheLookup('/tmp/a.dng', 2800, CeyxOutputFormat.rgba8),
          isNull,
          reason:
              'an rgba8 lookup served the yuv420 entry would size a 4 B/px '
              'decode with a 1.5 B/px figure and overflow the slot',
        );
        expect(
          pool.debugSizeCacheLookup(
            '/tmp/a.dng',
            2800,
            CeyxOutputFormat.yuv420,
          ),
          100,
        );
      },
    );

    test('TC-1320 (F4): job keys in different formats do not coalesce', () {
      final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      addTearDown(pool.dispose);

      final a = pool.debugJobKey(
        CeyxPoolJobType.decode,
        '/tmp/a.dng',
        2800,
        CeyxOutputFormat.rgba8,
      );
      final b = pool.debugJobKey(
        CeyxPoolJobType.decode,
        '/tmp/a.dng',
        2800,
        CeyxOutputFormat.yuv420,
      );

      expect(a, isNot(equals(b)));
    });
  });
}
