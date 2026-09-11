import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ceyx/ceyx.dart';
// WP10 wire tag. Imported from src with an explicit `show` rather than added
// to the package barrel: `plugin/lib/ceyx.dart` is outside this task's file
// ownership (plan A2.13), and the `show` keeps the import unambiguous against
// the barrel above. Precedent: encode_service_test.dart:8.
import 'package:ceyx/src/decode_pool.dart' show kMsgResize;
import 'package:ffi/ffi.dart' show calloc, malloc;
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
      // R4: every pooled allocation is rounded UP to a 16384-byte multiple
      // (the zero-copy alignment contract), so a request has to cross a
      // 16384 boundary to actually exceed an idle buffer's real capacity --
      // 64 and 4096 both round to the SAME 16384-byte slot and would
      // legitimately be reused. 20000 rounds to 32768, which genuinely
      // exceeds the first slot's rounded 16384 capacity.
      final small = await pool.acquire(64);
      pool.release(small);
      final big = await pool.acquire(20000);
      expect(big.capacity, greaterThanOrEqualTo(20000));
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

  // -------------------------------------------------------------------
  // WP10 (plan AMENDMENT 2 Task 12 + AMENDMENT 2b addendum): the pooled
  // decode-into route. The slot is acquired on the MAIN isolate and its
  // ADDRESS travels in the job message; the worker writes into it and never
  // touches CeyxNativeBufferPool.
  //
  // Every one of these asserts `debugFinalizerReleases == 0` (AC12.6): the
  // safety net firing means something escaped the explicit release path.
  // -------------------------------------------------------------------

  // AC12.1 — TC-1070
  test(
    'TC-1070: the pre-acquired address travels in the job message and the slot '
    'stays checked out until consumption ends',
    () async {
      final buffers = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(buffers.debugDisposeIdle);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(() => CeyxDecodePool.nativeBufferPool = null);
      CeyxDecodePool.debugDecodeIntoAvailable = true;
      addTearDown(() => CeyxDecodePool.debugDecodeIntoAvailable = null);

      final pool = CeyxDecodePool(width: 1, entryPoint: _dstAddressWorker);
      addTearDown(pool.dispose);
      pool.debugSeedSizeCache('a.dng', null, 2 * 2 * 4);

      final outcome = await pool.submit(
        CeyxPoolJobType.decode,
        'a.dng',
        generation: pool.generation,
      );
      final image = outcome.value! as DngImage;

      expect(
        image.nativeAddress,
        isNot(0),
        reason: 'the worker must have been given an address',
      );
      expect(buffers.debugCheckedOut, 1, reason: 'still being consumed');

      final second = await buffers.acquire(2 * 2 * 4);
      expect(
        second.address,
        isNot(image.nativeAddress),
        reason: 'a live slot must not be handed out twice',
      );
      buffers.release(second);

      image.releaseToPool();
      expect(buffers.debugExplicitReleases, 2);
      expect(
        buffers.debugFinalizerReleases,
        0,
        reason: 'safety net must not fire',
      );
    },
  );

  // AC12.2 — TC-1071
  test(
    'TC-1071: a resize response releases the undersized slot, re-acquires the '
    'exact size, and re-dispatches exactly once',
    () async {
      final buffers = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(buffers.debugDisposeIdle);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(() => CeyxDecodePool.nativeBufferPool = null);
      CeyxDecodePool.debugDecodeIntoAvailable = true;
      addTearDown(() => CeyxDecodePool.debugDecodeIntoAvailable = null);

      final pool = CeyxDecodePool(width: 1, entryPoint: _resizeOnceWorker);
      addTearDown(pool.dispose);
      pool.debugSeedSizeCache('b.dng', null, 2 * 2 * 4); // deliberately stale

      final outcome = await pool.submit(
        CeyxPoolJobType.decode,
        'b.dng',
        generation: pool.generation,
      );
      final image = outcome.value! as DngImage;

      expect(
        image.rgbaData.length,
        4 * 4 * 4,
        reason: 'the retry used the real size',
      );
      expect(
        pool.debugDispatchCountFor('b.dng'),
        2,
        reason: 'exactly one retry',
      );
      expect(
        buffers.debugCheckedOut,
        1,
        reason: 'only the retry slot is live',
      );
      expect(
        pool.debugSizeCacheFor('b.dng', null),
        4 * 4 * 4,
        reason: 'the cache learned the real size',
      );

      image.releaseToPool();
      expect(buffers.debugFinalizerReleases, 0);
    },
  );

  // AC12.3 / R10.5 — TC-1072, REWRITTEN under erratum E-WP10-1072.
  //
  // The original version asserted that a second refusal throws. That was
  // written when the only source of a "too small" refusal was the cheap
  // pre-check, so a double refusal implied a lying probe. Under AMENDMENT 3
  // there is a SECOND source — the RAW post-unpack capacity backstop, for
  // formats whose geometry shifts during unpack (R11.1, X3F/Foveon) — so a
  // double refusal is a legitimate outcome on a real file.
  //
  // Failing the job there would mean a photo that opened fine BEFORE WP10
  // stops opening because of WP10. The bound that matters is "the pooled
  // retry happens at most once", not "the decode dies"; so the second refusal
  // now falls back to the ordinary allocating route and the photo still opens.
  //
  // The double refusal here is modelled realistically: this worker refuses
  // only when it was actually GIVEN a buffer, and decodes normally when it was
  // not — which is exactly how the native entry behaves.
  test(
    'TC-1072: a second resize falls back to the unpooled route so the decode '
    'still succeeds, with the pooled retry still bounded at one',
    () async {
      final buffers = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(buffers.debugDisposeIdle);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(() => CeyxDecodePool.nativeBufferPool = null);
      CeyxDecodePool.debugDecodeIntoAvailable = true;
      CeyxDecodePool.debugNativeFree =
          (address) => calloc.free(Pointer<Uint8>.fromAddress(address));
      addTearDown(() {
        CeyxDecodePool.debugDecodeIntoAvailable = null;
        CeyxDecodePool.debugNativeFree = null;
      });

      final pool = CeyxDecodePool(
        width: 1,
        entryPoint: _twoSourceRefusalWorker,
      );
      addTearDown(pool.dispose);
      pool.debugSeedSizeCache('c.dng', null, 2 * 2 * 4);

      final outcome = await pool.submit(
        CeyxPoolJobType.decode,
        'c.dng',
        generation: pool.generation,
      );
      final image = outcome.value! as DngImage;

      expect(
        image.decodeMs,
        5,
        reason: 'the final dispatch carried NO dstAddress: the fallback took '
            'the ordinary allocating route',
      );
      expect(
        image.rgbaData.length,
        2 * 2 * 4,
        reason: 'the photo still opens — this is the regression this erratum '
            'exists to prevent',
      );
      expect(
        pool.debugDispatchCountFor('c.dng'),
        3,
        reason: 'initial + ONE pooled retry + the unpooled fallback',
      );
      expect(
        buffers.debugCheckedOut,
        0,
        reason: 'both slots were returned; the fallback holds none',
      );
      expect(buffers.debugFinalizerReleases, 0);
    },
  );

  // R10.5 retained — TC-1078.
  //
  // TC-1072 no longer proves termination, because its worker stops refusing
  // once the buffer is withdrawn. This one keeps that guarantee against a
  // PATHOLOGICAL worker that refuses unconditionally — including on the
  // unpooled fallback, where it was given no buffer to refuse. That is a
  // broken worker rather than a real file, and it must still terminate: an
  // unbounded resize loop hangs a decode forever.
  test(
    'TC-1078: a worker that refuses even the unpooled fallback fails the job '
    'rather than looping forever',
    () async {
      final buffers = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(buffers.debugDisposeIdle);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(() => CeyxDecodePool.nativeBufferPool = null);
      CeyxDecodePool.debugDecodeIntoAvailable = true;
      addTearDown(() => CeyxDecodePool.debugDecodeIntoAvailable = null);

      final pool = CeyxDecodePool(width: 1, entryPoint: _alwaysResizeWorker);
      addTearDown(pool.dispose);
      pool.debugSeedSizeCache('p.dng', null, 2 * 2 * 4);

      await expectLater(
        pool.submit(
          CeyxPoolJobType.decode,
          'p.dng',
          generation: pool.generation,
        ),
        throwsA(isA<StateError>()),
      );
      expect(
        pool.debugDispatchCountFor('p.dng'),
        3,
        reason: 'initial + pooled retry + unpooled fallback, then it stops',
      );
      expect(
        buffers.debugCheckedOut,
        0,
        reason: 'every slot was returned on the failure path',
      );
      expect(buffers.debugFinalizerReleases, 0);
    },
  );

  // AC12.4 / A2.6 — TC-1073
  test(
    'TC-1073: with the native entry absent, no address is sent and no slot is '
    'taken',
    () async {
      final buffers = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(buffers.debugDisposeIdle);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(() => CeyxDecodePool.nativeBufferPool = null);
      CeyxDecodePool.debugDecodeIntoAvailable = false;
      addTearDown(() => CeyxDecodePool.debugDecodeIntoAvailable = null);
      // The degraded route makes the WORKER own its allocation, exactly as in
      // production; the seam frees it so this test leaks nothing.
      CeyxDecodePool.debugNativeFree =
          (address) => calloc.free(Pointer<Uint8>.fromAddress(address));
      addTearDown(() => CeyxDecodePool.debugNativeFree = null);

      final pool = CeyxDecodePool(width: 1, entryPoint: _dstAddressWorker);
      addTearDown(pool.dispose);

      final outcome = await pool.submit(
        CeyxPoolJobType.decode,
        'd.dng',
        generation: pool.generation,
      );
      final image = outcome.value! as DngImage;

      expect(
        image.decodeMs,
        5,
        reason: 'the job message had exactly 5 elements: no dstAddress, no '
            'dstCapacity (see _dstAddressWorker second signal)',
      );
      expect(buffers.debugCheckedOut, 0, reason: 'no slot may be taken');
      expect(buffers.debugAllocations, 0);
      expect(
        pool.debugProbeSizeCount,
        0,
        reason: 'no probe on the degraded route',
      );
      expect(buffers.debugFinalizerReleases, 0);
    },
  );

  // AC12.5 — TC-1074
  test(
    'TC-1074: the size cache is populated by one probe and reused by later '
    'decodes',
    () async {
      final buffers = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(buffers.debugDisposeIdle);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(() => CeyxDecodePool.nativeBufferPool = null);
      CeyxDecodePool.debugDecodeIntoAvailable = true;
      addTearDown(() => CeyxDecodePool.debugDecodeIntoAvailable = null);

      final pool = CeyxDecodePool(
        width: 1,
        entryPoint: _probeSizeThenDecodeWorker,
      );
      addTearDown(pool.dispose);

      for (var i = 0; i < 2; i++) {
        final outcome = await pool.submit(
          CeyxPoolJobType.decode,
          'e.dng',
          generation: pool.generation,
        );
        (outcome.value! as DngImage).releaseToPool();
      }
      expect(
        pool.debugProbeSizeCount,
        1,
        reason: 'the second decode must reuse the cached extent',
      );
      expect(buffers.debugFinalizerReleases, 0);
    },
  );

  // AMENDMENT 3 AC16.2 — TC-1076.
  //
  // Replaces the 2b mixed-availability test that held this number. Under one
  // format-agnostic entry pair the mixed state is unreachable by construction,
  // so the property worth pinning inverted: BOTH formats must reach the pooled
  // route through the SAME binding. A regression that reintroduced per-format
  // routing in Dart shows up here as one of the two decodes silently
  // degrading.
  test(
    'TC-1076: a .dng and a .arw both take the pooled route through the one '
    'format-agnostic binding',
    () async {
      final buffers = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(buffers.debugDisposeIdle);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(() => CeyxDecodePool.nativeBufferPool = null);
      CeyxDecodePool.debugDecodeIntoAvailable = true;
      addTearDown(() => CeyxDecodePool.debugDecodeIntoAvailable = null);

      final pool = CeyxDecodePool(width: 1, entryPoint: _dstAddressWorker);
      addTearDown(pool.dispose);
      pool.debugSeedSizeCache('f.dng', null, 2 * 2 * 4);
      pool.debugSeedSizeCache('f.arw', null, 2 * 2 * 4);

      for (final path in const <String>['f.dng', 'f.arw']) {
        final outcome = await pool.submit(
          CeyxPoolJobType.decode,
          path,
          generation: pool.generation,
        );
        final image = outcome.value! as DngImage;
        expect(
          image.decodeMs,
          8,
          reason: '$path must be pooled: 5 base fields + the encodeArgs '
              'placeholder + dstAddress + dstCapacity',
        );
        expect(
          buffers.ownsAddress(image.nativeAddress),
          isTrue,
          reason: '$path payload must sit in a pool slot',
        );
        expect(buffers.debugCheckedOut, 1, reason: '$path is being consumed');
        image.releaseToPool();
      }

      expect(buffers.debugCheckedOut, 0);
      expect(
        buffers.debugFinalizerReleases,
        0,
        reason: 'AC16.2: the safety net must not fire on either route',
      );
    },
  );

  // AMENDMENT 3 — TC-1077, the DNG_ENABLE_GENERIC_RAW=OFF build.
  //
  // In that configuration BOTH symbols still exist (they live in an
  // always-compiled TU), so availability is legitimately TRUE — but a generic
  // RAW input has no decoder, and the native probe says so with
  // kCeyxErrFormatUnsupportedInBuild. This case is what REPLACES 2b's second
  // availability flag: probe and decode fail TOGETHER, so no slot is ever
  // acquired and there is no window in which the pool believes a buffer is in
  // use while the worker ignores its address.
  //
  // The sentinel is what makes it cheap: a path this build cannot probe costs
  // ONE probe for the session, not one per decode.
  test(
    'TC-1077: on a build with no generic-RAW decoder, an .arw probe reports no '
    'extent, no slot is taken, the decode falls back, and the miss is cached',
    () async {
      final buffers = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(buffers.debugDisposeIdle);
      CeyxDecodePool.nativeBufferPool = buffers;
      addTearDown(() => CeyxDecodePool.nativeBufferPool = null);
      // Availability is TRUE: an OFF build still EXPORTS both symbols.
      CeyxDecodePool.debugDecodeIntoAvailable = true;
      CeyxDecodePool.debugNativeFree =
          (address) => calloc.free(Pointer<Uint8>.fromAddress(address));
      addTearDown(() {
        CeyxDecodePool.debugDecodeIntoAvailable = null;
        CeyxDecodePool.debugNativeFree = null;
      });

      final pool = CeyxDecodePool(
        width: 1,
        entryPoint: _formatUnsupportedProbeWorker,
      );
      addTearDown(pool.dispose);

      for (var i = 0; i < 2; i++) {
        final outcome = await pool.submit(
          CeyxPoolJobType.decode,
          'g.arw',
          generation: pool.generation,
        );
        final image = outcome.value! as DngImage;
        expect(
          image.decodeMs,
          5,
          reason: 'decode ${i + 1} must fall back to the allocating route: no '
              'dstAddress and no dstCapacity in the message',
        );
      }

      expect(
        buffers.debugCheckedOut,
        0,
        reason: 'a format this build cannot decode must never take a slot',
      );
      expect(
        buffers.debugAllocations,
        0,
        reason: 'and must never cause a pool allocation',
      );
      expect(
        pool.debugProbeSizeCount,
        1,
        reason: 'the negative result is cached: ONE probe across two decodes, '
            'not one per decode',
      );
      expect(buffers.debugFinalizerReleases, 0);
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

  // -------------------------------------------------------------------
  // AC-S1 (pool idle-shrink campaign) — shrink to floor after a quiet
  // window, with hysteresis, never touching a checked-out buffer.
  //
  // Time is INJECTED (`CeyxNativeBufferPool.debugClock` + a manual timer
  // factory): a test that slept through the real 5s window would assert the
  // wall clock, not the policy.
  // -------------------------------------------------------------------
  group('AC-S1 idle shrink', () {
    late DateTime fakeNow;

    setUp(() {
      fakeNow = DateTime.utc(2026, 9, 12, 12);
      CeyxNativeBufferPool.debugClock = () => fakeNow;
      CeyxNativeBufferPool.debugPressureReliefOverride = null;
    });

    tearDown(() {
      CeyxNativeBufferPool.debugClock = DateTime.now;
      CeyxNativeBufferPool.debugPressureReliefOverride = null;
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

    test('TC-1250: shrink frees idle buffers down to the floor, once', () async {
      var reliefCalls = 0;
      CeyxNativeBufferPool.debugPressureReliefOverride = () {
        reliefCalls++;
        return 0;
      };
      final pool = CeyxNativeBufferPool(maxBuffers: 6, idleFloor: 2);
      addTearDown(pool.debugDisposeIdle);
      await fillIdle(pool, 6);
      expect(pool.debugIdleCount, 6);

      final freed = pool.shrinkToFloor();

      expect(freed, 4);
      expect(pool.debugIdleCount, 2);
      expect(pool.debugLiveBuffers, 2);
      expect(pool.debugShrinkEvents, 1);
      expect(pool.debugBuffersFreedByShrink, 4);
      // EXACTLY once per batch, not once per freed buffer.
      expect(reliefCalls, 1);
      expect(pool.debugPressureReliefCalls, 1);

      // Already at the floor: a second call frees nothing and is not an event.
      expect(pool.shrinkToFloor(), 0);
      expect(pool.debugShrinkEvents, 1);
      expect(reliefCalls, 1);
    });

    test('TC-1251: a shrink never frees a checked-out buffer', () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 2);
      addTearDown(pool.debugDisposeIdle);
      await fillIdle(pool, 4);
      final held = await pool.acquire(4096);
      addTearDown(() => pool.release(held));

      final freed = pool.shrinkToFloor();

      // Refused outright: something is checked out, and a worker isolate may
      // still be writing into it.
      expect(freed, 0);
      expect(pool.debugShrinkRefusals, 1);
      expect(pool.debugShrinkEvents, 0);
      expect(pool.debugLiveBuffers, 4);
      expect(pool.debugLiveAddresses, contains(held.address));
      expect(held.released, isFalse);
    });

    test('TC-1260: an adopted buffer counts as outstanding and blocks a shrink',
        () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 2);
      addTearDown(pool.debugDisposeIdle);
      await fillIdle(pool, 4);
      // The worker-allocated escape hatch: not pooled, occupies no slot, but
      // it IS live RGBA a DngImage is still reading. Agreed ruling: strict —
      // it counts as outstanding for both quiescence and the shrink guard.
      final adopted = pool.adoptUnpooled(malloc<Uint8>(4096).address, 4096);
      expect(pool.hasOutstandingCheckouts, isTrue);

      expect(pool.shrinkToFloor(), 0);
      expect(pool.debugShrinkRefusals, 1);
      expect(pool.debugIdleCount, 4);

      pool.release(adopted); // frees it: unpooled buffers are not idled
      expect(pool.hasOutstandingCheckouts, isFalse);
      expect(pool.shrinkToFloor(), 2);
    });

    test('TC-1261: every shrink batch either calls the relief or records a skip',
        () async {
      // Environment-independent: whether this host resolves the dylib decides
      // WHICH counter moves, but exactly one of them must move, exactly once.
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 2);
      addTearDown(pool.debugDisposeIdle);
      await fillIdle(pool, 4);

      expect(pool.shrinkToFloor(), 2);

      expect(pool.debugPressureReliefCalls + pool.debugPressureReliefSkips, 1);
    });

    test('TC-1252: a shrink refuses while a waiter is queued', () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 2, idleFloor: 1);
      addTearDown(pool.debugDisposeIdle);
      final a = await pool.acquire(4096);
      final b = await pool.acquire(4096);
      final pending = pool.acquire(4096); // no slot: queues a waiter
      expect(pool.debugWaiterCount, 1);

      expect(pool.shrinkToFloor(), 0);
      expect(pool.debugShrinkRefusals, 1);

      pool.release(a);
      final served = await pending;
      pool.release(served);
      pool.release(b);
    });

    test('TC-1253: demand regrows the pool to the cap after a shrink', () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 2);
      addTearDown(pool.debugDisposeIdle);
      await fillIdle(pool, 4);
      expect(pool.shrinkToFloor(), 2);
      expect(pool.debugLiveBuffers, 2);

      final held = <CeyxNativeBuffer>[];
      for (var i = 0; i < 4; i++) {
        held.add(await pool.acquire(4096));
      }
      // Regrew to the cap with no waiting and no unpooled escape.
      expect(pool.debugLiveBuffers, 4);
      expect(pool.debugWaitsForCapacity, 0);
      expect(pool.debugUnpooledAllocations, 0);
      for (final b in held) {
        pool.release(b);
      }
    });

    test('TC-1254: a shrink invalidates the warm-up memo', () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 2);
      addTearDown(pool.debugDisposeIdle);
      await pool.warmUpFor(4096);
      expect(pool.debugWarmedBytes, 4096);
      await fillIdle(pool, 4);

      expect(pool.shrinkToFloor(), 2);

      // The warmed pages went with the freed buffer, so the memo must not
      // keep claiming they are committed.
      expect(pool.debugWarmedBytes, isNull);
      final before = pool.debugAllocations;
      await pool.warmUpFor(4096);
      expect(pool.debugWarmedBytes, 4096);
      expect(pool.debugAllocations, greaterThanOrEqualTo(before));
    });

    test('TC-1255: the policy shrinks only after a full quiet window', () async {
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

      // Nothing armed until quiescence is asserted.
      expect(policy.debugArmed, isFalse);
      expect(pool.debugShrinkEvents, 0);

      policy.onQuiescenceChanged(true);
      expect(policy.debugArmed, isTrue);
      expect(scheduler.last.duration, const Duration(seconds: 5));
      // Window not elapsed yet: still no shrink.
      expect(pool.debugShrinkEvents, 0);
      expect(pool.debugIdleCount, 4);

      fakeNow = fakeNow.add(const Duration(seconds: 5));
      scheduler.fireLast();

      expect(pool.debugShrinkEvents, 1);
      expect(pool.debugIdleCount, 2);
    });

    test('TC-1256: activity during the window cancels the pending shrink',
        () async {
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
      final armed = scheduler.last;
      policy.onQuiescenceChanged(false); // a decode started
      expect(armed.isActive, isFalse);
      expect(policy.debugArmed, isFalse);

      // Even if a stale timer callback arrives, the shrink must not run.
      // The clock is advanced FIRST so every other guard (the grow-restart
      // window, the lockout) is satisfied and the only thing that can stop
      // the shrink is the quiescence state itself — otherwise this assertion
      // passes for a reason it is not testing (mutation M6 survived exactly
      // that way).
      fakeNow = fakeNow.add(const Duration(seconds: 30));
      armed.forceFire();
      expect(pool.debugShrinkEvents, 0);

      // The window is CONTINUOUS: quiescence must restart it from scratch.
      policy.onQuiescenceChanged(true);
      expect(scheduler.last.duration, const Duration(seconds: 5));
    });

    test('TC-1257: a grow inside the window restarts it from the grow',
        () async {
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
      // 3s in, something allocated (warm-up, or a decode that came and went
      // without the signal ever being observed false).
      fakeNow = fakeNow.add(const Duration(seconds: 3));
      // Bigger than the idle buffers' capacity (4096 rounds up to the 16384
      // alignment), so this really ALLOCATES instead of reusing — a reuse is
      // not a grow and would not move the hysteresis timestamp.
      await pool.warmUpFor(32768);
      fakeNow = fakeNow.add(const Duration(seconds: 2));
      scheduler.fireLast();

      // 5s of clock, but only 2s since the grow: rearmed for the remainder,
      // no shrink yet.
      expect(pool.debugShrinkEvents, 0);
      expect(policy.debugArmed, isTrue);
      expect(scheduler.last.duration, const Duration(seconds: 3));

      fakeNow = fakeNow.add(const Duration(seconds: 3));
      scheduler.fireLast();
      expect(pool.debugShrinkEvents, 1);
    });

    test('TC-1258: after a shrink, no re-shrink within the grow lockout',
        () async {
      final scheduler = _FakeTimerScheduler();
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 1);
      addTearDown(pool.debugDisposeIdle);
      await fillIdle(pool, 4);
      // quietWindow deliberately SHORTER than the lockout, so the lockout is
      // the binding rule and is observed on its own.
      final policy = CeyxPoolShrinkPolicy(
        pool,
        quietWindow: const Duration(milliseconds: 100),
        growLockout: const Duration(seconds: 1),
        timerFactory: scheduler.create,
      );
      addTearDown(policy.dispose);

      policy.onQuiescenceChanged(true);
      fakeNow = fakeNow.add(const Duration(seconds: 30));
      scheduler.fireLast();
      expect(pool.debugShrinkEvents, 1);
      expect(pool.debugIdleCount, 1);

      // Demand regrows the pool, then the app goes quiet again immediately.
      // TWO concurrent checkouts, because the pool is back at its floor of 1:
      // the first acquire reuses the surviving buffer (not a grow), the second
      // is the real allocation.
      final first = await pool.acquire(4096);
      final second = await pool.acquire(4096);
      pool.release(first);
      pool.release(second);
      policy.onQuiescenceChanged(false);
      policy.onQuiescenceChanged(true);
      fakeNow = fakeNow.add(const Duration(milliseconds: 100));
      scheduler.fireLast();

      // Quiet window satisfied, but only 100ms since the grow: refused and
      // rearmed for the remaining lockout.
      expect(pool.debugShrinkEvents, 1);
      expect(scheduler.last.duration, const Duration(milliseconds: 900));

      fakeNow = fakeNow.add(const Duration(milliseconds: 900));
      scheduler.fireLast();
      expect(pool.debugShrinkEvents, 2);
    });

    test('TC-1259: a false quiescence re-check at fire time vetoes the shrink',
        () async {
      final scheduler = _FakeTimerScheduler();
      final pool = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 2);
      addTearDown(pool.debugDisposeIdle);
      await fillIdle(pool, 4);
      var quiescentNow = false;
      final policy = CeyxPoolShrinkPolicy(
        pool,
        quietWindow: const Duration(seconds: 5),
        timerFactory: scheduler.create,
        isQuiescentNow: () => quiescentNow,
      );
      addTearDown(policy.dispose);

      policy.onQuiescenceChanged(true);
      fakeNow = fakeNow.add(const Duration(seconds: 5));
      scheduler.fireLast();
      // The pushed signal said quiet, the authoritative source says otherwise:
      // the decode pool wins.
      expect(pool.debugShrinkEvents, 0);
      expect(pool.debugIdleCount, 4);

      quiescentNow = true;
      policy.onQuiescenceChanged(false);
      policy.onQuiescenceChanged(true);
      fakeNow = fakeNow.add(const Duration(seconds: 5));
      scheduler.fireLast();
      expect(pool.debugShrinkEvents, 1);
    });
  });
}

/// A [Timer] the test fires by hand, so the 5s/1s windows are asserted as
/// policy rather than as elapsed wall-clock time.
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

  /// Normal expiry: a cancelled timer does nothing.
  void fire() {
    if (!_active) return;
    _active = false;
    _callback();
  }

  /// Delivers the callback EVEN IF cancelled — models the real
  /// already-queued-callback race, which the policy must survive.
  void forceFire() {
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

// --- WP10 test doubles ---------------------------------------------------
// These differ from [_addressWorker] in one deliberate way: they read the
// address out of the JOB MESSAGE rather than out of the path string, because
// "the address travels in the job message" is precisely the wiring under test.
//
// Job wire shape (additive):
//   [kMsgJob, id, typeIndex, path, maxDim, encodeArgs?, dstAddress, dstCapacity]
// The two WP10 fields are present only when the main isolate pre-acquired a
// slot, so `message.length > 6` IS the degradation check.

/// Echoes the pre-acquired address back as the result address.
///
/// When no address was sent it does what the REAL worker does on the degraded
/// route (`decodeForPointerTransfer`): allocates its own buffer. Answering 0
/// instead would make the pool materialise a view over a null pointer, so
/// "no address" has to be observed some other way — hence the second signal
/// below.
///
/// SECOND SIGNAL: the received message LENGTH is reported back in the
/// `decodeMs` field. No other assertion in this file reads `decodeMs`, and it
/// lets a test assert the wire shape MECHANICALLY (`length == 5` means the two
/// WP10 fields are genuinely absent) instead of inferring absence from a
/// sentinel address. Absence is what TC-1073/TC-1076 are actually about.
void _dstAddressWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final jobs = ReceivePort('dst-address-worker');
  jobs.listen((Object? message) {
    if (message is! List || message.isEmpty) return;
    if (message[0] == kMsgShutdown) {
      jobs.close();
      return;
    }
    if (message[0] != kMsgJob) return;
    final requestId = message[1] as int;
    final observedLength = message.length.toDouble();
    final dstAddress = message.length > 6
        ? message[6] as int
        : calloc<Uint8>(2 * 2 * 4).address;
    poolPort.send(<Object?>[
      kMsgResult,
      requestId,
      dstAddress,
      2,
      2,
      observedLength,
      2.0,
    ]);
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

/// Refuses the FIRST decode with `kMsgResize(4x4)` — the stale-prediction case
/// the native `kDngErrDstTooSmall` / `kRawErrDstTooSmall` refusal produces —
/// and accepts the retry.
void _resizeOnceWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  var refused = false;
  final jobs = ReceivePort('resize-once-worker');
  jobs.listen((Object? message) {
    if (message is! List || message.isEmpty) return;
    if (message[0] == kMsgShutdown) {
      jobs.close();
      return;
    }
    if (message[0] != kMsgJob) return;
    final requestId = message[1] as int;
    if (!refused) {
      refused = true;
      poolPort.send(<Object?>[kMsgResize, requestId, 4, 4]);
      return;
    }
    final dstAddress = message.length > 6 ? message[6] as int : 0;
    poolPort.send(<Object?>[kMsgResult, requestId, dstAddress, 4, 4, 1.0, 2.0]);
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

/// Models the TWO-SOURCE refusal the A3 native entry can produce: refuses
/// whenever it was handed a buffer (standing in for the pre-check refusal on
/// the first attempt and the RAW post-unpack backstop on the second), and
/// decodes normally when it was handed none.
///
/// That second refusal is the realistic case E-WP10-1072 addresses: the retry
/// was sized from the extent the FIRST refusal reported, and the geometry
/// moved again during unpack.
void _twoSourceRefusalWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final jobs = ReceivePort('two-source-refusal-worker');
  jobs.listen((Object? message) {
    if (message is! List || message.isEmpty) return;
    if (message[0] == kMsgShutdown) {
      jobs.close();
      return;
    }
    if (message[0] != kMsgJob) return;
    final requestId = message[1] as int;
    final dstAddress = message.length > 6 ? message[6] as int : 0;
    if (dstAddress != 0) {
      // Given a buffer -> refuse it, reporting a larger extent each time.
      poolPort.send(<Object?>[kMsgResize, requestId, 4, 4]);
      return;
    }
    // Given no buffer -> the ordinary allocating route, which always works.
    poolPort.send(<Object?>[
      kMsgResult,
      requestId,
      calloc<Uint8>(2 * 2 * 4).address,
      2,
      2,
      message.length.toDouble(),
      2.0,
    ]);
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

/// A PATHOLOGICAL worker: refuses unconditionally, including on the unpooled
/// fallback where it was handed no buffer at all. Not a real file — a broken
/// worker. Proves the resize path terminates rather than looping forever
/// (R10.5), which TC-1072 no longer proves now that it falls back.
void _alwaysResizeWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final jobs = ReceivePort('always-resize-worker');
  jobs.listen((Object? message) {
    if (message is! List || message.isEmpty) return;
    if (message[0] == kMsgShutdown) {
      jobs.close();
      return;
    }
    if (message[0] != kMsgJob) return;
    poolPort.send(<Object?>[kMsgResize, message[1] as int, 4, 4]);
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

/// Answers a `probeSize` job with a 2x2 extent and a decode job like
/// [_dstAddressWorker]. Used by TC-1074 to prove the size cache absorbs the
/// second decode's probe.
void _probeSizeThenDecodeWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final jobs = ReceivePort('probe-size-worker');
  jobs.listen((Object? message) {
    if (message is! List || message.isEmpty) return;
    if (message[0] == kMsgShutdown) {
      jobs.close();
      return;
    }
    if (message[0] != kMsgJob) return;
    final requestId = message[1] as int;
    final type = CeyxPoolJobType.values[message[2] as int];
    if (type == CeyxPoolJobType.probeSize) {
      poolPort.send(<Object?>[kMsgResult, requestId, 2, 2]);
      return;
    }
    final dstAddress = message.length > 6 ? message[6] as int : 0;
    poolPort.send(<Object?>[kMsgResult, requestId, dstAddress, 2, 2, 1.0, 2.0]);
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

/// Stands in for a `DNG_ENABLE_GENERIC_RAW=OFF` dylib: the format-agnostic
/// symbols ARE present, but the probe reports no extent for a format this
/// build has no decoder for (native `kCeyxErrFormatUnsupportedInBuild`, which
/// `probeOutputSize` surfaces as null and the worker sends as 0x0).
///
/// Decodes are answered like [_dstAddressWorker], so the fallback still
/// produces a real image — which is the point: absence degrades, it does not
/// break.
void _formatUnsupportedProbeWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final jobs = ReceivePort('format-unsupported-probe-worker');
  jobs.listen((Object? message) {
    if (message is! List || message.isEmpty) return;
    if (message[0] == kMsgShutdown) {
      jobs.close();
      return;
    }
    if (message[0] != kMsgJob) return;
    final requestId = message[1] as int;
    final type = CeyxPoolJobType.values[message[2] as int];
    if (type == CeyxPoolJobType.probeSize) {
      poolPort.send(<Object?>[kMsgResult, requestId, 0, 0]);
      return;
    }
    final observedLength = message.length.toDouble();
    final dstAddress = message.length > 6
        ? message[6] as int
        : calloc<Uint8>(2 * 2 * 4).address;
    poolPort.send(<Object?>[
      kMsgResult,
      requestId,
      dstAddress,
      2,
      2,
      observedLength,
      2.0,
    ]);
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
