import 'dart:async';

import 'package:ceyx/ceyx.dart';
import 'package:flutter_test/flutter_test.dart';

/// mem8 T2 (SR-1) — the plugin half of the arena idle release.
///
/// WHAT IS BEING PROVED: the native idle funnel `ceyx_native_idle_shrink`
/// (mem8 T1) is reached from exactly one place — the tail of
/// [CeyxNativeBufferPool.shrinkToFloor] — with the pool's own `idleFloor`,
/// after the pressure relief and before the host's `onShrink` listener, and
/// never on the refusal or already-at-the-floor paths.
///
/// The highest-severity failure this wiring can have is A3's: a call placed
/// above the refusal guard would release arena device regions while a decode
/// may still hold a slot, and the native per-lane refusal is documented as a
/// backstop, not a lock (native/include/raw_ffi_api.h — "the caller must
/// guarantee decode quiescence").
///
/// No dylib is needed: `debugArenaIdleShrinkOverride` stands in for the
/// symbol and `debugPressureReliefOverride` for its neighbour, exactly as the
/// AC-S1 shrink cases in `native_buffer_pool_test.dart` do.
void main() {
  late DateTime fakeNow;

  setUp(() {
    fakeNow = DateTime(2026, 9, 20, 12);
    CeyxNativeBufferPool.debugClock = () => fakeNow;
    CeyxNativeBufferPool.debugPressureReliefOverride = () => 0;
  });

  tearDown(() {
    CeyxNativeBufferPool.debugClock = DateTime.now;
    CeyxNativeBufferPool.debugPressureReliefOverride = null;
    CeyxNativeBufferPool.debugArenaIdleShrinkOverride = null;
    CeyxNativeBufferPool.debugFreeHook = null;
    // The resolution latch is process-wide; without this a null override in
    // one case leaks whatever the first resolution attempt in this process
    // found into the next.
    CeyxNativeBufferPool.debugResetNativeBindingsCache();
  });

  /// Brings [pool] to `count` idle pooled buffers by checking them all out at
  /// once (so the pool must really allocate `count` slots) and returning them.
  Future<void> fillIdle(CeyxNativeBufferPool pool, int count) async {
    final held = <CeyxNativeBuffer>[];
    for (var i = 0; i < count; i++) {
      held.add(await pool.acquire(4096));
    }
    for (final b in held) {
      pool.release(b);
    }
  }

  test(
    'TC-1310 (A1): a full quiet window drives the arena shrink exactly once, '
    'with the pool idleFloor',
    () async {
      final scheduler = _FakeTimerScheduler();
      final floors = <int>[];
      CeyxNativeBufferPool.debugArenaIdleShrinkOverride = (int floor) {
        floors.add(floor);
        return 4096;
      };
      // idleFloor 2 is production's `shared` pool value; asserting the
      // ARGUMENT (not just "it was called") is what fails a wiring that
      // passes maxBuffers instead.
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 2);
      addTearDown(pool.debugDisposeIdle);
      await fillIdle(pool, 4);
      final policy = CeyxPoolShrinkPolicy(
        pool,
        quietWindow: const Duration(seconds: 5),
        timerFactory: scheduler.create,
      );
      addTearDown(policy.dispose);

      policy.onQuiescenceChanged(true);
      expect(floors, isEmpty, reason: 'window not elapsed yet');

      fakeNow = fakeNow.add(const Duration(seconds: 5));
      scheduler.fireLast();

      expect(pool.debugShrinkEvents, 1, reason: 'precondition: shrink ran');
      expect(floors, <int>[2]);
      expect(pool.debugArenaIdleShrinkCalls, 1);
      expect(pool.debugLastArenaIdleShrinkBytes, 4096);
      expect(pool.debugArenaIdleShrinkSkips, 0);
    },
  );

  test(
    'TC-1311 (A2): an absent symbol is tolerated — zero calls, one skip, '
    'no throw',
    () async {
      // Override null AND bindings override null: the dylib-predates-T1 case.
      CeyxNativeBufferPool.debugArenaIdleShrinkOverride = null;
      CeyxNativeBufferPool.debugNativeBindingsOverride = null;
      CeyxNativeBufferPool.debugResetNativeBindingsCache();
      addTearDown(CeyxNativeBufferPool.debugResetNativeBindingsCache);
      final scheduler = _FakeTimerScheduler();
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 2);
      addTearDown(pool.debugDisposeIdle);
      await fillIdle(pool, 4);
      final policy = CeyxPoolShrinkPolicy(
        pool,
        quietWindow: const Duration(seconds: 5),
        timerFactory: scheduler.create,
      );
      addTearDown(policy.dispose);

      policy.onQuiescenceChanged(true);
      fakeNow = fakeNow.add(const Duration(seconds: 5));

      // The whole point: this must not throw. `ceyx_native_idle_shrink` is an
      // OPTIMISATION — a dylib without it means "no idle shrink", and the app
      // is still fully correct. (mem8 T14's yuv420 entry is load-bearing for
      // correctness and takes the opposite rule.)
      expect(scheduler.fireLast, returnsNormally);

      expect(pool.debugShrinkEvents, 1);
      expect(pool.debugArenaIdleShrinkCalls, 0);
      expect(pool.debugArenaIdleShrinkSkips, 1);
      expect(pool.debugLastArenaIdleShrinkBytes, isNull);
    },
  );

  test(
    'TC-1312 (A3): the refusal path never reaches the arena — an outstanding '
    'checkout means a decode may still be live',
    () async {
      var calls = 0;
      CeyxNativeBufferPool.debugArenaIdleShrinkOverride = (int floor) {
        calls++;
        return 0;
      };
      final scheduler = _FakeTimerScheduler();
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 2);
      addTearDown(pool.debugDisposeIdle);
      await fillIdle(pool, 4);
      final held = await pool.acquire(4096);
      addTearDown(() => pool.release(held));
      final policy = CeyxPoolShrinkPolicy(
        pool,
        quietWindow: const Duration(seconds: 5),
        timerFactory: scheduler.create,
      );
      addTearDown(policy.dispose);

      policy.onQuiescenceChanged(true);
      fakeNow = fakeNow.add(const Duration(seconds: 5));
      scheduler.fireLast();

      expect(pool.debugShrinkRefusals, 1);
      expect(pool.debugShrinkEvents, 0);
      expect(
        calls,
        0,
        reason:
            'releasing arena regions under a live checkout is the '
            'frame-corruption path this guard exists for',
      );
      expect(pool.debugArenaIdleShrinkSkips, 0);
    },
  );

  test(
    'TC-1313 (A4): the arena release happens BEFORE the host onShrink '
    'listener, and after the pressure relief',
    () async {
      final order = <String>[];
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 2);
      addTearDown(pool.debugDisposeIdle);
      await fillIdle(pool, 4);
      CeyxNativeBufferPool.debugFreeHook = (_) => order.add('free');
      CeyxNativeBufferPool.debugPressureReliefOverride = () {
        order.add('relief');
        return 0;
      };
      CeyxNativeBufferPool.debugArenaIdleShrinkOverride = (int floor) {
        order.add('arena:$floor');
        return 0;
      };
      pool.onShrink = (int freed) => order.add('onShrink:$freed');

      expect(pool.shrinkToFloor(), 2);

      // Halcyon couples its Windows working-set trim to onShrink; if the trim
      // ran before the arena handed its pages back, it would trim the wrong
      // working set.
      expect(order, <String>[
        'free',
        'free',
        'relief',
        'arena:2',
        'onShrink:2',
      ]);
    },
  );

  test(
    'TC-1314 (A5): a shrink that frees nothing does not call the arena — no '
    'per-tick FFI call on an idle process',
    () async {
      var calls = 0;
      CeyxNativeBufferPool.debugArenaIdleShrinkOverride = (int floor) {
        calls++;
        return 0;
      };
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 2);
      addTearDown(pool.debugDisposeIdle);
      await fillIdle(pool, 2); // already at the floor

      expect(pool.shrinkToFloor(), 0, reason: 'precondition: freed == 0');
      expect(pool.debugShrinkEvents, 0);
      expect(calls, 0);
      expect(pool.debugArenaIdleShrinkSkips, 0);
    },
  );
}

class _FakeTimer implements Timer {
  _FakeTimer(this.duration, this._callback);

  final Duration duration;
  final void Function() _callback;
  bool _active = true;

  @override
  bool get isActive => _active;

  @override
  int get tick => 0;

  @override
  void cancel() => _active = false;

  void fire() {
    if (!_active) return;
    _active = false;
    _callback();
  }
}

class _FakeTimerScheduler {
  final List<_FakeTimer> timers = <_FakeTimer>[];

  Timer create(Duration duration, void Function() callback) {
    final timer = _FakeTimer(duration, callback);
    timers.add(timer);
    return timer;
  }

  _FakeTimer get last => timers.last;

  void fireLast() => last.fire();
}
