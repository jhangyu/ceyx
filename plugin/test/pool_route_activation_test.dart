import 'dart:io';

import 'package:ceyx/ceyx.dart';
import 'package:ceyx/src/dng_bindings.dart';
import 'package:flutter_test/flutter_test.dart';

/// R6 pool activation (Task #9) — proves the POOLED DECODE ROUTE actually
/// executes end to end through [CeyxDecodePool], with a real dylib, a real
/// sample, and a real [CeyxNativeBufferPool].
///
/// Why this test exists and why the existing suite did not already cover it:
/// * `native_buffer_pool_test.dart` drives the pool with fakes through the
///   `debugNativeFree` seam — it proves the bookkeeping, not that a decode
///   ever takes the route.
/// * `wp10_activation_proof_test.dart` proves the FFI pair works, but it
///   pre-acquires its buffer with a bare `malloc` and calls
///   `decodeIntoPointer` directly — the decode pool and the buffer pool are
///   both absent from that path.
/// So both were green while `CeyxDecodePool.nativeBufferPool` was never
/// assigned in production and the pooled route carried zero traffic. This test
/// is the one that goes red in that state.
///
/// The assertion is on the buffer pool's OWN counters — an allocation
/// happened, the checkout came back, and the finalizer safety net never fired
/// — not on the absence of an error, because a decode that silently fell back
/// to the allocating route also produces no error.
///
/// flutter test runs with cwd == package root (plugin/), so the paths below
/// resolve relative to Directory.current.
void main() {
  final dylibPath = File(
    Platform.environment['CEYX_WP10_DYLIB'] ??
        '../native/build/libdng_decoder_native.dylib',
  ).absolute.path;

  final dngPath = File('../image_samples/lossless_dng_sample.dng').absolute.path;

  late bool skip;
  String skipReason = '';

  setUpAll(() {
    skip = false;
    if (!File(dylibPath).existsSync()) {
      skip = true;
      skipReason =
          'reason: no freshly-built dylib at $dylibPath — set CEYX_WP10_DYLIB '
          'or build native/build via cmake first';
      return;
    }
    if (!File(dngPath).existsSync()) {
      skip = true;
      skipReason = 'reason: sample corpus missing ($dngPath)';
      return;
    }
    final bindings = DngNativeBindings.fromPath(dylibPath);
    if (!bindings.decodeIntoBufferAvailable) {
      skip = true;
      skipReason =
          'reason: dylib at $dylibPath does not export the WP10 decode-into '
          'pair, so the pooled route is unreachable by design';
    }
  });

  test(
    'R6-AC1: a real decode through CeyxDecodePool takes the pooled route — '
    'the buffer pool allocates a slot, the checkout returns, and the '
    'finalizer safety net never fires',
    () async {
      if (skip) {
        // A silently skipped case produces a report indistinguishable from a
        // full run, so say so out loud.
        // ignore: avoid_print
        print('[SKIP] R6-AC1 $skipReason');
        return;
      }

      final buffers = CeyxNativeBufferPool(maxBuffers: 2);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(() {
        CeyxDecodePool.nativeBufferPool = null;
        buffers.debugDisposeIdle();
      });

      final pool = CeyxDecodePool(width: 1, libraryPath: dylibPath);
      addTearDown(pool.dispose);

      expect(
        buffers.debugAllocations,
        0,
        reason: 'precondition: nothing allocated before the decode',
      );

      final image = await pool.decode(dngPath);

      expect(
        pool.debugProbeSizeCount,
        greaterThanOrEqualTo(1),
        reason:
            'the pooled route probes the output extent before acquiring a '
            'slot; zero probes means the decode took the allocating route',
      );
      expect(
        buffers.debugAllocations,
        greaterThanOrEqualTo(1),
        reason: 'a pooled slot must have been allocated for this decode',
      );
      expect(
        buffers.debugCheckedOut,
        1,
        reason: 'the decode holds exactly one slot until it is released',
      );
      expect(image.nativeAddress, isNot(0));
      expect(image.rgbaData.length, image.width * image.height * 4);

      image.releaseToPool();

      expect(
        buffers.debugCheckedOut,
        0,
        reason: 'releaseToPool must return the slot immediately',
      );
      expect(
        buffers.debugExplicitReleases,
        greaterThanOrEqualTo(1),
        reason: 'the return must go through the explicit path',
      );
      expect(
        buffers.debugFinalizerReleases,
        0,
        reason:
            'the finalizer is a safety net for paths that forget; a non-zero '
            'count here is a defect signal, not normal operation',
      );
    },
    timeout: const Timeout(Duration(minutes: 2)),
  );
}
