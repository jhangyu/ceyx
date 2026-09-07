// WP2 Task 2.5: the replacement for the native
// dng_rgba_output_checked_out_count() gauge, at the same strength. The native
// gauge answered ONE question — "is anything, anywhere in the process, still
// checked out right now?" — and that answer was the leak assertion in
// test_concurrent_decode.cpp. Per-buffer identity accounting answers a weaker
// question ("did THIS buffer come back?"), so the strength is restored here, on
// the Dart side, where ownership now lives.
import 'dart:math';

import 'package:ceyx/src/native_buffer_pool.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('a leaked acquire is visible in the process-wide gauge', () async {
    final pool = CeyxNativeBufferPool(maxBuffers: 2);
    final before = CeyxNativeBufferPool.debugTotalLiveAddresses;
    final leaked = await pool.acquire(1024); // deliberately never released
    expect(
      CeyxNativeBufferPool.debugTotalLiveAddresses,
      before + 1,
      reason: 'the gauge must see a checkout that never came back',
    );
    expect(pool.debugLiveAddresses, contains(leaked.address));
    pool.release(leaked);
    expect(CeyxNativeBufferPool.debugTotalLiveAddresses, before);
    expect(pool.debugLiveAddresses, isEmpty);
    pool.debugDisposeIdle();
  });

  test('debugLiveAddresses tracks debugCheckedOut through a randomised '
      'acquire/release/adopt/oversize sequence', () async {
    // Fixed seed: an unreproducible red is a report, not a bug.
    final rng = Random(20260907);
    final freed = <int>[];
    CeyxNativeBufferPool.debugFreeHook = freed.add;
    addTearDown(() => CeyxNativeBufferPool.debugFreeHook = null);

    final pool = CeyxNativeBufferPool(maxBuffers: 3, maxBufferBytes: 4096);
    final checkedOut = <CeyxNativeBuffer>[];
    final adoptedRaw = <int>[];

    for (var step = 0; step < 200; step++) {
      switch (rng.nextInt(4)) {
        case 0: // ordinary pooled acquire (never blocks: capacity guarded)
          if (checkedOut.length < pool.maxBuffers) {
            checkedOut.add(await pool.acquire(1 + rng.nextInt(4096)));
          }
        case 1: // oversize acquire -> unpooled but pool-owned
          checkedOut.add(await pool.acquire(4097 + rng.nextInt(1024)));
        case 2: // adoption of a foreign address
          final fake = 0x100000 + step * 0x1000;
          adoptedRaw.add(fake);
          checkedOut.add(pool.adoptUnpooled(fake, 64));
        case 3: // release something, if anything is out
          if (checkedOut.isNotEmpty) {
            pool.release(checkedOut.removeAt(rng.nextInt(checkedOut.length)));
          }
      }
      expect(
        pool.debugLiveAddresses.length,
        pool.debugCheckedOut,
        reason: 'gauge and counter disagreed at step $step',
      );
    }

    for (final buffer in checkedOut.toList()) {
      pool.release(buffer);
    }
    expect(pool.debugCheckedOut, 0);
    expect(pool.debugLiveAddresses, isEmpty);
    pool.debugDisposeIdle();
  });

  // Weak retention cannot be asserted deterministically (GC timing is not ours
  // to force), so what is asserted here is the property that matters for the
  // gauge: a pool that has returned and disposed everything contributes zero.
  test('a pool that has given everything back contributes nothing', () async {
    final before = CeyxNativeBufferPool.debugTotalLiveAddresses;
    final pool = CeyxNativeBufferPool(maxBuffers: 1);
    final buffer = await pool.acquire(64);
    expect(CeyxNativeBufferPool.debugTotalLiveAddresses, before + 1);
    pool.release(buffer);
    pool.debugDisposeIdle();
    expect(CeyxNativeBufferPool.debugTotalLiveAddresses, before);
  });
}
