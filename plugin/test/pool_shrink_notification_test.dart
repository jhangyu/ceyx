import 'package:ceyx/ceyx.dart';
import 'package:flutter_test/flutter_test.dart';

/// P5 (win-parity campaign): the pool's shrink-completion notification.
///
/// The host app (Halcyon) is the single writer of
/// [CeyxNativeBufferPool.onShrink]; no ceyx code assigns it. These cases pin
/// the contract the host relies on: exactly one call per batch that really
/// freed buffers, carrying the freed count, fired after the frees and after
/// the pressure relief, and never fired on a refused or empty shrink.
///
/// No dylib is needed: `debugFreeHook` intercepts the frees and
/// `debugPressureReliefOverride` stands in for the native symbol, exactly as
/// the AC-S1 shrink cases in `native_buffer_pool_test.dart` do.
void main() {
  group('onShrink notification', () {
    setUp(() {
      CeyxNativeBufferPool.debugPressureReliefOverride = () => 0;
    });

    tearDown(() {
      CeyxNativeBufferPool.debugPressureReliefOverride = null;
      CeyxNativeBufferPool.debugFreeHook = null;
    });

    /// Brings [pool] to `count` idle pooled buffers by checking them all out
    /// at once (so the pool must really allocate `count` slots) and returning
    /// them.
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
      'TC-1262: onShrink fires once per non-empty shrink, with the freed count',
      () async {
        final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 1);
        addTearDown(pool.debugDisposeIdle);
        final calls = <int>[];
        pool.onShrink = calls.add;
        await fillIdle(pool, 4);

        final freed = pool.shrinkToFloor();

        expect(freed, 3);
        expect(calls, <int>[3]);
      },
    );

    test('TC-1263: a refused shrink does not fire onShrink', () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 1);
      addTearDown(pool.debugDisposeIdle);
      final calls = <int>[];
      pool.onShrink = calls.add;
      await fillIdle(pool, 4);
      final held = await pool.acquire(4096);
      addTearDown(() => pool.release(held));

      final freed = pool.shrinkToFloor();

      expect(freed, 0);
      expect(pool.debugShrinkRefusals, 1);
      expect(calls, isEmpty);
    });

    test('TC-1264: a shrink that frees nothing does not fire onShrink',
        () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 1);
      addTearDown(pool.debugDisposeIdle);
      await fillIdle(pool, 1);
      final calls = <int>[];
      pool.onShrink = calls.add;

      // Already at the floor: the `freed == 0` early return runs.
      expect(pool.shrinkToFloor(), 0);
      expect(pool.debugShrinkEvents, 0);
      expect(calls, isEmpty);
    });

    test('TC-1265: onShrink fires after the frees and after the pressure relief',
        () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 1);
      addTearDown(pool.debugDisposeIdle);
      final order = <String>[];
      await fillIdle(pool, 4);
      CeyxNativeBufferPool.debugFreeHook = (_) => order.add('free');
      CeyxNativeBufferPool.debugPressureReliefOverride = () {
        order.add('relief');
        return 0;
      };
      pool.onShrink = (freed) => order.add('onShrink:$freed');

      final freed = pool.shrinkToFloor();

      expect(freed, 3);
      expect(pool.debugPressureReliefCalls, 1);
      expect(order.last, 'onShrink:3');
      expect(order, <String>['free', 'free', 'free', 'relief', 'onShrink:3']);
    });
  });
}
