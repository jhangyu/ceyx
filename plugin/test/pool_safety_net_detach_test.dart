// Round-1 review F1 (BLOCKER, memory safety): the pool safety-net Finalizer was
// attached with a detach token that nothing ever used. After an explicit
// releaseToPool the buffer returns to the free list and can be re-acquired; the
// OLD image's typed list is then collected and its still-armed finalizer
// releases the address a second time — under a live new owner.
//
// GC cannot be forced from a test, so the finalizer's LATE FIRING is simulated
// by calling the pool entry point the finalizer calls, with the token the
// finalizer captured. That is deterministic and tests the actual property:
// a stale safety net must not reclaim a buffer that has since been handed out
// again.
import 'package:ceyx/src/native_buffer_pool.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('a stale safety net cannot release a re-acquired buffer', () async {
    final freed = <int>[];
    CeyxNativeBufferPool.debugFreeHook = freed.add;
    addTearDown(() => CeyxNativeBufferPool.debugFreeHook = null);

    final pool = CeyxNativeBufferPool(maxBuffers: 1);
    final first = await pool.acquire(1024);
    // What an attach site captures alongside the typed list.
    final capturedBuffer = first;
    final capturedGeneration = first.checkoutGeneration;

    // The host releases explicitly — the buffer goes back to the free list.
    pool.release(first);

    // A later decode re-acquires the SAME underlying buffer object: the pool
    // reuses instances, so object identity alone cannot distinguish the two
    // checkouts. Only the checkout generation can.
    final second = await pool.acquire(1024);
    expect(identical(second, capturedBuffer), isTrue,
        reason: 'the pool reuses the instance — identity is not a guard');
    expect(second.checkoutGeneration, isNot(capturedGeneration));

    // NOW image A's typed list is collected and the stale finalizer fires.
    final acted = pool.releaseFromFinalizer(capturedBuffer, capturedGeneration);

    expect(acted, isFalse,
        reason: 'a finalizer from a previous checkout must not reclaim the '
            'buffer its successor is reading');
    expect(pool.debugCheckedOut, 1, reason: 'the live owner still holds it');
    expect(pool.debugFinalizerReleases, 0);
    expect(freed, isEmpty);

    pool.release(second);
    pool.debugDisposeIdle();
  });

  test('a genuine safety net still reclaims a forgotten buffer', () async {
    final pool = CeyxNativeBufferPool(maxBuffers: 1);
    final buffer = await pool.acquire(1024);
    final generation = buffer.checkoutGeneration;

    // Nobody released it: the safety net is exactly the path that must work.
    final acted = pool.releaseFromFinalizer(buffer, generation);

    expect(acted, isTrue);
    expect(pool.debugFinalizerReleases, 1);
    expect(pool.debugCheckedOut, 0);
    pool.debugDisposeIdle();
  });
}
