import 'dart:ffi';

import 'package:ceyx/ceyx.dart';
import 'package:ffi/ffi.dart' show calloc;
import 'package:flutter_test/flutter_test.dart';

import 'decode_pool_test.dart' show fakePoolWorker;

/// AC-S2 — `CeyxDecodePool.isQuiescent` and its notification seam.
///
/// WHAT IS BEING PROVED, and why each case exists rather than "it returns the
/// right bool": the idle buffer-pool shrink frees memory on the strength of
/// this predicate alone. A FALSE POSITIVE (claiming quiescence while a worker
/// isolate can still write into a borrowed buffer, or while an owner still
/// holds one) frees memory that is being written — a corrupted frame, not a
/// slow one. Every case below therefore pins a state in which a NAIVE
/// predicate (`queuedCount == 0 && inFlightCount == 0`) would wrongly say
/// "quiescent", and asserts this one says false.
///
/// The whole suite runs with the `fakePoolWorker` wire-protocol fake — no
/// dylib, no corpus, no timing luck. `slow:<ms>:` paths hold a job in flight
/// for a controlled window.
void main() {
  final freedAddresses = <int>[];

  setUp(() {
    freedAddresses.clear();
    // Fake decode payloads are real `calloc` allocations; without this seam
    // the pool would try to open a dylib to free them.
    CeyxDecodePool.debugNativeFree = (int address) {
      freedAddresses.add(address);
      calloc.free(Pointer<Uint8>.fromAddress(address));
    };
  });

  tearDown(() {
    CeyxDecodePool.debugNativeFree = null;
    CeyxDecodePool.nativeBufferPool = null;
    CeyxDecodePool.debugDecodeIntoAvailable = null;
  });

  test('AC-S2/Q1: a pool that has never run anything, with no buffer pool '
      'attached, is quiescent', () async {
    CeyxDecodePool.nativeBufferPool = null;
    final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
    addTearDown(pool.dispose);

    expect(pool.isQuiescent, isTrue);
  });

  test(
    'AC-S2/Q2 (RACE CASE): isQuiescent is false SYNCHRONOUSLY on submit — '
    'before the future returns and before the job reaches the queue',
    () async {
      CeyxDecodePool.nativeBufferPool = null;
      final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      addTearDown(pool.dispose);

      expect(pool.isQuiescent, isTrue, reason: 'precondition');

      // NOT awaited: the assertion below runs in the SAME turn as the submit,
      // which is the exact window a queue-depth-only predicate misses (the
      // job is in `_byKey` and nowhere else while `_prepareAndEnqueue`
      // awaits its size probe — and it may already hold a buffer by then).
      final inFlight = pool.submit(
        CeyxPoolJobType.probe,
        'slow:120:sample.dng',
      );

      expect(
        pool.isQuiescent,
        isFalse,
        reason:
            'admitted work must defeat quiescence in the same turn it is '
            'admitted; anything later is a window in which the shrink can '
            'free a buffer the decode is about to be handed',
      );

      await inFlight;
      await _settle();

      expect(
        pool.isQuiescent,
        isTrue,
        reason: 'the job completed and released everything it held',
      );
    },
  );

  test(
    'AC-S2/Q3: a job QUEUED BUT NOT STARTED (no idle worker) is not quiescent',
    () async {
      CeyxDecodePool.nativeBufferPool = null;
      // width 1 => exactly one worker => the second submit must wait in
      // `_queue` with no worker holding it and no requestId assigned.
      final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      addTearDown(pool.dispose);

      final first = pool.submit(CeyxPoolJobType.probe, 'slow:150:a.dng');
      final second = pool.submit(CeyxPoolJobType.probe, 'slow:150:b.dng');

      // Let the first reach a worker so the second is genuinely parked in the
      // queue rather than merely un-dispatched.
      await Future<void>.delayed(const Duration(milliseconds: 30));
      expect(
        pool.queuedCount + pool.inFlightCount,
        greaterThan(0),
        reason: 'precondition: work is outstanding',
      );
      expect(pool.isQuiescent, isFalse);

      await Future.wait([first, second]);
      await _settle();
      expect(pool.isQuiescent, isTrue);
    },
  );

  test(
    'AC-S2/Q4 (THE ONE THAT MATTERS): a CHECKED-OUT BUFFER defeats quiescence '
    'even though no decode is queued or in flight',
    () async {
      // On success the decode pool deliberately does NOT release the slot —
      // ownership passes to the DngImage and returns at releaseToPool(),
      // arbitrarily later. The synchronous DngDecoderService.decode route
      // borrows without touching the decode pool at all. In both states the
      // decode pool's own counters read "idle" while memory is live.
      final buffers = CeyxNativeBufferPool(maxBuffers: 2);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(buffers.debugDisposeIdle);

      final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      addTearDown(pool.dispose);

      expect(pool.isQuiescent, isTrue, reason: 'precondition: nothing held');

      final borrowed = buffers.acquireOrNull(64 * 1024)!;
      expect(
        pool.queuedCount,
        0,
        reason: 'instrument check: the naive predicate sees an idle pool…',
      );
      expect(pool.inFlightCount, 0);
      expect(
        pool.isQuiescent,
        isFalse,
        reason:
            '…but a borrowed buffer is exactly the memory the shrink would '
            'free; freeing it under its owner is the frame-corruption path',
      );

      buffers.release(borrowed);
      expect(pool.isQuiescent, isTrue);
    },
  );

  test(
    'AC-S2/Q5: a buffer-pool WAITER defeats quiescence — capacity is already '
    'spoken for, so shrinking would free what someone is blocked on',
    () async {
      final buffers = CeyxNativeBufferPool(maxBuffers: 1);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(buffers.debugDisposeIdle);

      final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      addTearDown(pool.dispose);

      final held = buffers.acquireOrNull(64 * 1024)!;
      // At the cap with nothing idle: this one parks on the waiter queue.
      final blocked = buffers.acquire(64 * 1024);
      await _settle();
      expect(pool.isQuiescent, isFalse);

      buffers.release(held);
      final served = await blocked;
      expect(pool.isQuiescent, isFalse, reason: 'the waiter now holds it');
      buffers.release(served);
      expect(pool.isQuiescent, isTrue);
    },
  );

  test(
    'AC-S2/Q6: quiescenceChanges emits TRANSITIONS only — false on admission, '
    'true when the last job and the last buffer are gone',
    () async {
      final buffers = CeyxNativeBufferPool(maxBuffers: 2);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(buffers.debugDisposeIdle);

      final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      addTearDown(pool.dispose);

      final events = <bool>[];
      final sub = pool.quiescenceChanges.listen(events.add);
      addTearDown(sub.cancel);
      await _settle();

      expect(
        events,
        isEmpty,
        reason: 'no initial event: a subscriber reads isQuiescent itself',
      );

      await pool.submit(CeyxPoolJobType.probe, 'slow:40:a.dng');
      await _settle();

      expect(
        events,
        <bool>[false, true],
        reason: 'exactly one leave and one return, no intermediate chatter',
      );

      // A buffer taken OUTSIDE any decode must still move the signal — this is
      // the edge the decode pool cannot see on its own, and the reason the
      // buffer pool carries a checkout hook.
      final borrowed = buffers.acquireOrNull(64 * 1024)!;
      await _settle();
      expect(events, <bool>[false, true, false]);

      buffers.release(borrowed);
      await _settle();
      expect(events, <bool>[false, true, false, true]);
    },
  );

  test('AC-S2/Q7: the pooled decode route holds its slot across the whole job, '
      'and quiescence returns only after the payload is freed', () async {
    final buffers = CeyxNativeBufferPool(maxBuffers: 2);
    CeyxDecodePool.nativeBufferPool = buffers;
    CeyxDecodePool.debugDecodeIntoAvailable = true;
    addTearDown(buffers.debugDisposeIdle);

    final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
    addTearDown(pool.dispose);

    final job = pool.submit(CeyxPoolJobType.probe, 'slow:60:a.dng');
    expect(pool.isQuiescent, isFalse);
    await job;
    await _settle();
    expect(pool.isQuiescent, isTrue);
  });

  test(
    'AC-S2/Q8 (POLICY WIRING): a shrink-capable buffer pool gets a default '
    'policy with no host wiring, and it is ARMED once work settles',
    () async {
      // idleFloor < maxBuffers == "this pool was built to shrink", which is
      // the activation condition. Production reaches it via
      // CeyxNativeBufferPool.shared (idleFloor: 2).
      final buffers = CeyxNativeBufferPool(maxBuffers: 2, idleFloor: 1);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(buffers.debugDisposeIdle);

      final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      // Disposing the pool is what cancels the policy's timer; without it the
      // test would end with a pending Timer.
      addTearDown(pool.dispose);

      expect(
        pool.debugShrinkPolicy,
        isNull,
        reason:
            'bound on first submit, not at construction: the host assigns '
            'nativeBufferPool after constructing the pool',
      );

      await pool.submit(CeyxPoolJobType.probe, 'slow:20:a.dng');
      await _settle();

      expect(pool.debugShrinkPolicy, isNotNull);
      expect(
        pool.debugShrinkPolicy!.debugArmed,
        isTrue,
        reason:
            'the job finished and nothing is checked out, so the quiet window '
            'must be running — this is the end-to-end proof that the stream '
            'reaches the policy with no Halcyon-side wiring',
      );
      expect(identical(pool.debugShrinkPolicy!.pool, buffers), isTrue);
    },
  );

  test('AC-S2/Q9: a pool that CANNOT shrink (idleFloor == maxBuffers) gets no '
      'policy and therefore no timer', () async {
    // The default constructor leaves idleFloor == maxBuffers. This is what
    // keeps every pre-existing test — and any host pool not opted in — free
    // of a live timer and unchanged in behaviour.
    final buffers = CeyxNativeBufferPool(maxBuffers: 2);
    CeyxDecodePool.nativeBufferPool = buffers;
    addTearDown(buffers.debugDisposeIdle);

    final pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
    addTearDown(pool.dispose);

    await pool.submit(CeyxPoolJobType.probe, 'a.dng');
    await _settle();

    expect(pool.debugShrinkPolicy, isNull);
  });
}

/// Drains pending microtasks AND the event-loop turn, so a
/// microtask-coalesced quiescence note has actually published before an
/// assertion reads it.
Future<void> _settle() async {
  await Future<void>.delayed(Duration.zero);
  await Future<void>.delayed(Duration.zero);
}
