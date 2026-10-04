import 'dart:async';

import 'package:ceyx/ceyx.dart';
import 'package:flutter_test/flutter_test.dart';

/// memreclaim spec §4.4: the funnel receives the pool's idle slots, and a host
/// can request a debounced reclaim pass (>= 1 s quiet window, decode cancels,
/// grow lockout kept).
class _CancelTimer implements Timer {
  _CancelTimer(this.onCancel);
  final void Function() onCancel;
  @override
  void cancel() => onCancel();
  @override
  bool get isActive => true;
  @override
  int get tick => 0;
}

class _NoopTimer implements Timer {
  @override
  void cancel() {}
  @override
  bool get isActive => false;
  @override
  int get tick => 0;
}

void main() {
  late List<List<int>> addressCalls;
  late List<List<int>> byteCalls;

  setUp(() {
    addressCalls = [];
    byteCalls = [];
    CeyxNativeBufferPool.debugArenaIdleShrinkOverride =
        (int floor, List<int> addresses, List<int> bytes) {
      addressCalls.add(List<int>.of(addresses));
      byteCalls.add(List<int>.of(bytes));
      return 0;
    };
  });

  tearDown(() {
    CeyxNativeBufferPool.debugArenaIdleShrinkOverride = null;
    CeyxNativeBufferPool.debugResetNativeBindingsCache();
  });

  Future<List<CeyxNativeBuffer>> fillIdle(CeyxNativeBufferPool pool, int n) async {
    final held = <CeyxNativeBuffer>[];
    for (var i = 0; i < n; i++) {
      held.add(await pool.acquire(4096));
    }
    for (final b in held) {
      pool.release(b);
    }
    return held;
  }

  test('the funnel receives exactly the buffers left idle after the shrink', () async {
    final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 2);
    addTearDown(pool.debugDisposeIdle);
    final buffers = await fillIdle(pool, 4);

    expect(pool.shrinkToFloor(), 2);

    expect(addressCalls, hasLength(1));
    expect(addressCalls.single, hasLength(2));
    final survivors = {for (final b in buffers) b.address: b.capacity};
    for (var i = 0; i < 2; i++) {
      expect(survivors[addressCalls.single[i]], byteCalls.single[i]);
    }
  });

  test('requestReclaim while quiescent arms the 1 s window and shrinks only on fire', () {
    final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 1);
    final armed = <Duration>[];
    void Function()? fire;
    final policy = CeyxPoolShrinkPolicy(
      pool,
      isQuiescentNow: () => true,
      timerFactory: (d, cb) {
        armed.add(d);
        fire = cb;
        return _NoopTimer();
      },
    );
    policy.onQuiescenceChanged(true); // normal 5 s arm
    policy.requestReclaim();
    expect(addressCalls, isEmpty); // never synchronous
    expect(armed.last, kReclaimRequestQuietWindow);
    expect(armed.last, greaterThanOrEqualTo(const Duration(seconds: 1)));
    fire!();
    expect(addressCalls, hasLength(1));
  });

  test('requestReclaim while busy arms the 1 s window at the next quiet edge, once', () {
    final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 1);
    var quiescent = false;
    final armed = <Duration>[];
    void Function()? fire;
    final policy = CeyxPoolShrinkPolicy(
      pool,
      isQuiescentNow: () => quiescent,
      timerFactory: (d, cb) {
        armed.add(d);
        fire = cb;
        return _NoopTimer();
      },
    );

    policy.requestReclaim();
    expect(addressCalls, isEmpty);
    expect(armed, isEmpty);

    quiescent = true;
    policy.onQuiescenceChanged(true);
    expect(armed.last, kReclaimRequestQuietWindow);
    fire!();
    expect(addressCalls, hasLength(1));

    quiescent = false;
    policy.onQuiescenceChanged(false);
    quiescent = true;
    policy.onQuiescenceChanged(true);
    expect(armed.last, kPoolShrinkQuietWindow); // pending was consumed
  });

  test('a decode inside the window cancels the requested pass; pending re-arms at the next quiet edge', () {
    final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 1);
    var quiescent = true;
    var cancelled = false;
    final armed = <Duration>[];
    final policy = CeyxPoolShrinkPolicy(
      pool,
      isQuiescentNow: () => quiescent,
      timerFactory: (d, cb) {
        armed.add(d);
        cancelled = false;
        return _CancelTimer(() => cancelled = true);
      },
    );
    policy.onQuiescenceChanged(true);
    policy.requestReclaim();
    quiescent = false;
    policy.onQuiescenceChanged(false); // a decode starts
    expect(cancelled, isTrue);
    expect(addressCalls, isEmpty);
    quiescent = true;
    policy.onQuiescenceChanged(true);
    expect(armed.last, kReclaimRequestQuietWindow);
  });

  test('a requested reclaim re-confirms quiescence at fire time', () {
    final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 1);
    var quiescent = false;
    void Function()? fire;
    final policy = CeyxPoolShrinkPolicy(
      pool,
      isQuiescentNow: () => quiescent,
      timerFactory: (d, cb) {
        fire = cb;
        return _NoopTimer();
      },
    );
    policy.requestReclaim();
    quiescent = true;
    policy.onQuiescenceChanged(true);
    quiescent = false; // a decode started between arm and fire
    fire!();
    expect(addressCalls, isEmpty);
  });

  test('CeyxDecodePool.requestReclaim is a no-op for a pool that cannot shrink', () {
    final decodePool = CeyxDecodePool();
    addTearDown(decodePool.dispose);
    final saved = CeyxDecodePool.nativeBufferPool;
    addTearDown(() => CeyxDecodePool.nativeBufferPool = saved);
    CeyxDecodePool.nativeBufferPool = CeyxNativeBufferPool(maxBuffers: 2);
    decodePool.requestReclaim();
    expect(addressCalls, isEmpty);
  });
}
