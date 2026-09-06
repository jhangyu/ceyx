import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ceyx/ceyx.dart';
import 'package:ffi/ffi.dart' show calloc;
import 'package:flutter_test/flutter_test.dart';

/// WP6 Step 7.1-7.3: the fixed-slot native buffer pool.
///
/// These tests are written BEFORE the implementation exists (plan Step 7.1);
/// the recorded red is a compile error naming `CeyxNativeBufferPool`.
///
/// Every allocation here is a REAL native allocation (the pool mallocs), so
/// each test releases everything it acquires — an unreleased buffer is a leak
/// in the test process, not just a failed assertion.
void main() {
  // -------------------------------------------------------------------
  // AC7.1 / AC7.2 — reuse, not reallocation; explicit releases counted and
  // the safety-net finalizer never involved.
  // -------------------------------------------------------------------
  test(
    'TC-1060: a pooled buffer is reused, not reallocated',
    () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(pool.debugDisposeIdle);
      for (var i = 0; i < 6; i++) {
        final b = await pool.acquire(1024);
        pool.release(b);
      }
      expect(pool.debugAllocations, lessThanOrEqualTo(2));
      expect(pool.debugExplicitReleases, 6);
      expect(pool.debugFinalizerReleases, 0);
      expect(pool.debugCheckedOut, 0);
    },
  );

  test(
    'TC-1061: allocations stay bounded by maxBuffers over 3x maxBuffers cycles',
    () async {
      const maxBuffers = 3;
      final pool = CeyxNativeBufferPool(maxBuffers: maxBuffers);
      addTearDown(pool.debugDisposeIdle);
      for (var i = 0; i < 3 * maxBuffers; i++) {
        final b = await pool.acquire(4096);
        pool.release(b);
      }
      // Sequential acquire/release can never need more than ONE buffer; the
      // bound that matters is that it never grows past the cap.
      expect(pool.debugAllocations, lessThanOrEqualTo(maxBuffers));
      expect(pool.debugExplicitReleases, 3 * maxBuffers);
      expect(pool.debugFinalizerReleases, 0);
    },
  );

  // -------------------------------------------------------------------
  // AC7.3 — exhaustion WAITS, it never allocates past maxBuffers.
  // -------------------------------------------------------------------
  test(
    'TC-1062: exhaustion waits instead of allocating',
    () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(pool.debugDisposeIdle);
      final a = await pool.acquire(1024);
      final b = await pool.acquire(1024);
      var third = false;
      CeyxNativeBuffer? thirdBuffer;
      unawaited(
        pool.acquire(1024).then((buf) {
          third = true;
          thirdBuffer = buf;
        }),
      );
      await Future<void>.delayed(Duration.zero);
      expect(third, isFalse, reason: 'the third acquire must not complete');
      expect(pool.debugWaitsForCapacity, 1);
      expect(pool.debugAllocations, 2);

      pool.release(a);
      await Future<void>.delayed(Duration.zero);
      expect(third, isTrue, reason: 'a released buffer must satisfy the waiter');
      expect(pool.debugAllocations, 2, reason: 'no allocation past maxBuffers');

      pool.release(b);
      pool.release(thirdBuffer!);
    },
  );

  test(
    'TC-1063: a waiter is handed the returned buffer directly, and a buffer '
    'handed to a waiter is NOT counted as idle',
    () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 1);
      addTearDown(pool.debugDisposeIdle);
      final a = await pool.acquire(64);
      final pending = pool.acquire(64);
      await Future<void>.delayed(Duration.zero);
      pool.release(a);
      final handed = await pending;
      expect(handed.address, a.address, reason: 'the same slot is reused');
      expect(handed.released, isFalse, reason: 're-checked-out, not idle');
      expect(pool.debugCheckedOut, 1);
      expect(pool.debugIdleCount, 0);
      pool.release(handed);
      expect(pool.debugCheckedOut, 0);
    },
  );

  // -------------------------------------------------------------------
  // AC7.4 — a double release is a no-op, not a double free.
  // -------------------------------------------------------------------
  test(
    'TC-1064: a double release is a no-op',
    () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 1);
      addTearDown(pool.debugDisposeIdle);
      final b = await pool.acquire(1024);
      pool.release(b);
      pool.release(b);
      expect(pool.debugExplicitReleases, 1);
      expect(pool.debugCheckedOut, 0);
      expect(pool.debugIdleCount, 1, reason: 'returned exactly once');
    },
  );

  // -------------------------------------------------------------------
  // Oversize: allocated OUTSIDE the pool, freed on release rather than
  // returned, and never counted against the fixed slot budget.
  // -------------------------------------------------------------------
  test(
    'TC-1065: an oversize request is served unpooled and freed on release',
    () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 1, maxBufferBytes: 4096);
      addTearDown(pool.debugDisposeIdle);
      final big = await pool.acquire(8192);
      expect(big.pooled, isFalse);
      expect(big.capacity, greaterThanOrEqualTo(8192));
      // A pooled slot is still available: the oversize buffer took none.
      final small = await pool.acquire(64);
      expect(small.pooled, isTrue);
      pool.release(big);
      pool.release(small);
      expect(pool.debugExplicitReleases, 2);
      expect(pool.debugIdleCount, 1, reason: 'only the pooled one came back');
    },
  );

  test(
    'TC-1066: a request larger than an idle buffer allocates a new slot rather '
    'than handing back an undersized one',
    () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(pool.debugDisposeIdle);
      final small = await pool.acquire(64);
      pool.release(small);
      final big = await pool.acquire(4096);
      expect(big.capacity, greaterThanOrEqualTo(4096));
      expect(big.address, isNot(small.address));
      pool.release(big);
    },
  );

  // -------------------------------------------------------------------
  // MANDATORY use-after-free probe (task brief): the buffer backing a decode
  // payload must NOT be freed while the payload is still being consumed.
  // Driven through the existing `CeyxDecodePool.debugNativeFree` seam, which
  // is where every free the pool would perform is observable.
  // -------------------------------------------------------------------
  test(
    'TC-1067: use-after-free probe — no free is observed until consumption of '
    'the decode payload completes',
    () async {
      final freed = <int>[];
      CeyxNativeBufferPool.debugFreeHook = freed.add;
      CeyxDecodePool.debugNativeFree = freed.add;
      addTearDown(() {
        CeyxNativeBufferPool.debugFreeHook = null;
        CeyxDecodePool.debugNativeFree = null;
      });

      final pool = CeyxDecodePool(width: 1, entryPoint: _probeWorker);
      addTearDown(pool.dispose);

      final outcome = await pool.submit(
        CeyxPoolJobType.decode,
        'probe.dng',
        generation: pool.generation,
      );
      final image = outcome.value as DngImage;

      // CONSUMPTION WINDOW. The pixels must still be readable here; a free
      // during this window is exactly the defect this probe exists to catch.
      expect(freed, isEmpty, reason: 'freed before consumption completed');
      expect(image.rgbaData.length, 2 * 2 * 4);
      var checksum = 0;
      for (final byte in image.rgbaData) {
        checksum += byte;
      }
      expect(checksum, 0, reason: 'calloc-zeroed buffer read back intact');
      expect(freed, isEmpty, reason: 'freed mid-read');

      // End of consumption: the explicit release entry is the ONLY thing that
      // may reclaim it, and it is idempotent.
      image.releaseToPool();
      image.releaseToPool();
      expect(freed.length, lessThanOrEqualTo(1));
    },
  );

  // -------------------------------------------------------------------
  // The pooled route end to end, on the wiring that Step 7.4 would feed in
  // production: the buffer is acquired on the MAIN isolate, the worker writes
  // into that pre-acquired address, and end-of-consumption returns the slot for
  // immediate reuse instead of waiting for GC.
  // -------------------------------------------------------------------
  test(
    'TC-1069: a pool-owned decode payload is returned by releaseToPool and the '
    'slot is reusable immediately, with the safety net never firing',
    () async {
      final buffers = CeyxNativeBufferPool(maxBuffers: 1);
      addTearDown(buffers.debugDisposeIdle);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(() => CeyxDecodePool.nativeBufferPool = null);

      final slot = await buffers.acquire(2 * 2 * 4);
      final pool = CeyxDecodePool(width: 1, entryPoint: _addressWorker);
      addTearDown(pool.dispose);

      final outcome = await pool.submit(
        CeyxPoolJobType.decode,
        'addr:${slot.address}.dng',
        generation: pool.generation,
      );
      final image = outcome.value as DngImage;
      expect(image.nativeAddress, slot.address);
      expect(buffers.debugCheckedOut, 1, reason: 'still being consumed');
      expect(image.rgbaData.length, 2 * 2 * 4);

      image.releaseToPool();
      expect(buffers.debugExplicitReleases, 1);
      expect(buffers.debugCheckedOut, 0);
      expect(buffers.debugIdleCount, 1, reason: 'reusable immediately');
      expect(buffers.debugFinalizerReleases, 0, reason: 'safety net must not fire');

      // Idempotent, and no second reclaim of the same slot.
      image.releaseToPool();
      expect(buffers.debugExplicitReleases, 1);

      final again = await buffers.acquire(2 * 2 * 4);
      expect(again.address, slot.address, reason: 'the slot was reused');
      expect(buffers.debugAllocations, 1, reason: 'no reallocation');
      buffers.release(again);
    },
  );

  test(
    'TC-1068: releaseToPool on a Dart-heap-backed DngImage is a safe no-op',
    () async {
      final freed = <int>[];
      CeyxNativeBufferPool.debugFreeHook = freed.add;
      addTearDown(() => CeyxNativeBufferPool.debugFreeHook = null);
      final image = DngImage(
        rgbaData: Uint8List(16),
        width: 2,
        height: 2,
        decodeMs: 0,
        processMs: 0,
      );
      image.releaseToPool();
      image.releaseToPool();
      expect(freed, isEmpty);
    },
  );
}

/// Stands in for the Step 7.4 worker: the RGBA output address is PRE-ACQUIRED
/// on the main isolate and travels in the job (here through the path string,
/// which is the only field this test double needs); the worker allocates
/// nothing and frees nothing.
void _addressWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final jobs = ReceivePort('address-worker');
  jobs.listen((Object? message) {
    if (message is! List || message.isEmpty) return;
    if (message[0] == kMsgShutdown) {
      jobs.close();
      return;
    }
    if (message[0] != kMsgJob) return;
    final requestId = message[1] as int;
    final path = message[3] as String;
    final address = int.parse(path.split(':')[1].split('.')[0]);
    poolPort.send(<Object?>[kMsgResult, requestId, address, 2, 2, 1.0, 2.0]);
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

/// Minimal pointer-transfer worker: answers one decode with a real `calloc`
/// allocation using the H2-A wire shape
/// (`[address, width, height, decodeMs, processMs]`).
void _probeWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final jobs = ReceivePort('probe-worker');
  jobs.listen((Object? message) {
    if (message is! List || message.isEmpty) return;
    if (message[0] == kMsgShutdown) {
      jobs.close();
      return;
    }
    if (message[0] != kMsgJob) return;
    final requestId = message[1] as int;
    final buf = calloc<Uint8>(2 * 2 * 4);
    poolPort.send(<Object?>[kMsgResult, requestId, buf.address, 2, 2, 1.0, 2.0]);
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}
