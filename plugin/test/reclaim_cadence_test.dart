import 'dart:async';

import 'package:ceyx/ceyx.dart';
import 'package:flutter_test/flutter_test.dart';

/// Gate G1 (memreclaim spec §5): the idle funnel (and therefore layer-B step 5,
/// which runs inside every funnel call) fires ONLY after decode quiescence —
/// 5 s normally, >= 1 s for a host request — never from navigation cadence.
/// Fake clock + fake timers: logic, not elapsed real time.
class _FakeTimer implements Timer {
  _FakeTimer(this.fireAt, this.callback);
  final DateTime fireAt;
  final void Function() callback;
  bool cancelled = false;
  bool fired = false;
  @override
  void cancel() => cancelled = true;
  @override
  bool get isActive => !cancelled && !fired;
  @override
  int get tick => 0;
}

class _Harness {
  _Harness() {
    pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 1);
    policy = CeyxPoolShrinkPolicy(
      pool,
      isQuiescentNow: () => quiescent,
      timerFactory: (d, cb) {
        final t = _FakeTimer(now.add(d), cb);
        timers.add(t);
        return t;
      },
    );
  }

  DateTime now = DateTime(2026, 10, 4, 12);
  final timers = <_FakeTimer>[];
  bool quiescent = false;
  late final CeyxNativeBufferPool pool;
  late final CeyxPoolShrinkPolicy policy;

  void setQuiescent(bool q) {
    quiescent = q;
    policy.onQuiescenceChanged(q);
  }

  /// Moves the clock forward, firing due timers in order.
  void advance(Duration d) {
    final end = now.add(d);
    while (true) {
      final due = timers.where((t) => t.isActive && !t.fireAt.isAfter(end)).toList()
        ..sort((a, b) => a.fireAt.compareTo(b.fireAt));
      if (due.isEmpty) break;
      final t = due.first;
      now = t.fireAt;
      t.fired = true;
      t.callback();
    }
    now = end;
  }

  /// One decode: busy edge, [work] of work, quiet edge.
  void decode([Duration work = const Duration(milliseconds: 100)]) {
    setQuiescent(false);
    advance(work);
    setQuiescent(true);
  }
}

void main() {
  late int calls;
  late _Harness h;

  setUp(() {
    calls = 0;
    CeyxNativeBufferPool.debugArenaIdleShrinkOverride = (floor, addresses, bytes) {
      calls++;
      return 0;
    };
    h = _Harness();
    CeyxNativeBufferPool.debugClock = () => h.now;
    h.setQuiescent(true);
  });

  tearDown(() {
    CeyxNativeBufferPool.debugArenaIdleShrinkOverride = null;
    CeyxNativeBufferPool.debugClock = DateTime.now;
    CeyxNativeBufferPool.debugResetNativeBindingsCache();
  });

  test('a: submits every 250 ms..4.9 s for 60 s simulated -> 0 funnel calls; b: then 5 s quiet -> exactly 1', () {
    const gaps = [250, 1000, 2500, 4900];
    var elapsed = 0;
    var i = 0;
    while (elapsed < 60000) {
      h.decode();
      final gap = gaps[i++ % gaps.length];
      h.advance(Duration(milliseconds: gap - 100));
      elapsed += gap;
    }
    expect(calls, 0);
    h.advance(const Duration(seconds: 5));
    expect(calls, 1);
  });

  test('c: 10 requestReclaim() while busy -> exactly 1 pass, >= 1 s after quiescence', () {
    h.setQuiescent(false);
    for (var i = 0; i < 10; i++) {
      h.policy.requestReclaim();
    }
    h.advance(const Duration(seconds: 30));
    expect(calls, 0); // busy: nothing may run
    h.setQuiescent(true);
    h.advance(const Duration(milliseconds: 999));
    expect(calls, 0);
    h.advance(const Duration(milliseconds: 1));
    expect(calls, 1);
    h.advance(const Duration(seconds: 30));
    expect(calls, 1); // pending consumed; the 5 s window does not run it again
  });

  test('d: request, then a submit inside the window -> 0 funnel calls', () {
    h.policy.requestReclaim();
    h.advance(const Duration(milliseconds: 500));
    h.decode();
    h.advance(const Duration(milliseconds: 800));
    expect(calls, 0);
  });

  test('e: dwell 3 s on each of 20 photos -> 0 funnel runs (so 0 step-5 runs)', () {
    for (var i = 0; i < 20; i++) {
      h.decode();
      h.advance(const Duration(milliseconds: 2900));
    }
    expect(calls, 0);
  });
}
