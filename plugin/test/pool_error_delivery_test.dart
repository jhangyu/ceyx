// 2026-09-20 hang campaign (task #9): EVERY worker-side failure must COMPLETE
// the submitted future. A dropped reply is not a slow decode — it is a UI that
// spins forever and a pool slot that never comes back.
//
// Four failure shapes, one per mechanism that can lose a reply:
//   E1 negative native return code (the real -208 kRawErrKernelFailed from the
//      scaled yuv420 arm) travelling as kMsgError;
//   E2 a result the POOL cannot materialise (malformed payload) — historically
//      threw inside the ReceivePort listener AFTER the job had been removed
//      from every map, so nothing was left to complete it;
//   E3 worker isolate death mid-request;
//   E4 dispose() while a job is parked on `nativeBufferPool.acquire` — a job
//      in that window lives in `_byKey` alone and was reachable by no failure
//      path at all.
//
// Every case also asserts the LANE/SLOT lifecycle: a failed decode must leave
// the buffer pool with nothing checked out, or a run of failing files drains
// the fixed slot set and the NEXT decode parks forever (E4's mechanism, reached
// from E1/E2/E3's leak).
import 'dart:ffi';
import 'dart:isolate';

import 'package:ceyx/ceyx.dart';
import 'package:ffi/ffi.dart' show calloc;
import 'package:flutter_test/flutter_test.dart';

/// Fake worker for the error-delivery cases. No dylib is loaded.
///
/// Path conventions:
///   `nativeerr:<code>:*` -> replies kMsgError carrying a `RawDecodeException`
///     for that native code (the production shape: the worker's own
///     `catch (e)` forwards exactly the object `_throwDecodeError` raised).
///   `badpayload:*` -> replies kMsgResult with a payload the pool cannot
///     materialise (a String where `decodeMs` must be a double).
///   `crash:*` -> throws, killing the worker (errorsAreFatal).
/// `probeSize` always answers a 2x2 extent, so the pooled (slot-carrying)
/// route is the one under test.
void poolErrorDeliveryWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final jobs = ReceivePort();
  jobs.listen((Object? message) {
    final msg = message as List<Object?>;
    if (msg[0] == kMsgShutdown) {
      jobs.close();
      return;
    }
    if (msg[0] == kMsgConfigSlots) {
      poolPort.send(<Object?>[kMsgSlotsAck, msg[1] as int]);
      return;
    }
    final requestId = msg[1] as int;
    final type = CeyxPoolJobType.values[msg[2] as int];
    final path = msg[3] as String;
    if (type == CeyxPoolJobType.probeSize) {
      poolPort.send(<Object?>[kMsgResult, requestId, 2, 2]);
      return;
    }
    if (path.startsWith('crash:')) {
      throw StateError('fake worker crash for $path');
    }
    if (path.startsWith('nativeerr:')) {
      final code = int.parse(path.split(':')[1]);
      poolPort.send(<Object?>[
        kMsgError,
        requestId,
        RawDecodeException(
          code,
          RawErrorCode.name(code),
          'GPU kernel dispatch failed',
        ),
      ]);
      return;
    }
    if (path.startsWith('badpayload:')) {
      final destinationAddress = msg.length > 6 ? msg[6] as int : 0;
      poolPort.send(<Object?>[
        kMsgResult,
        requestId,
        destinationAddress,
        2,
        2,
        'not-a-double', // decodeMs — the pool's cast throws on this.
        2.0,
      ]);
      return;
    }
    final destinationAddress = msg.length > 6 ? msg[6] as int : 0;
    poolPort.send(
      <Object?>[kMsgResult, requestId, destinationAddress, 2, 2, 1.0, 2.0],
    );
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

void main() {
  late CeyxDecodePool pool;
  late CeyxNativeBufferPool bufferPool;

  setUp(() {
    bufferPool = CeyxNativeBufferPool(maxBuffers: 2);
    CeyxDecodePool.nativeBufferPool = bufferPool;
    // Force the POOLED route: the slot is what can leak, so the degraded
    // self-allocating route would test the wrong half.
    CeyxDecodePool.debugDecodeIntoAvailable = true;
  });

  tearDown(() async {
    CeyxDecodePool.debugDecodeIntoAvailable = null;
    await pool.dispose();
    bufferPool.debugDisposeIdle();
    CeyxDecodePool.nativeBufferPool = CeyxNativeBufferPool.shared;
  });

  test(
    'TC-1120 E1: a negative native return code completes the future with a '
    'typed exception naming that code, and returns the slot',
    () async {
      pool = CeyxDecodePool(width: 1, entryPoint: poolErrorDeliveryWorker);
      await expectLater(
        pool.decode('nativeerr:-208:raw_sample.arw'),
        throwsA(
          isA<RawDecodeException>()
              .having((e) => e.errorCode, 'errorCode', -208)
              .having((e) => e.errorName, 'errorName', 'kRawErrKernelFailed'),
        ),
      );
      expect(pool.inFlightCount, 0);
      expect(pool.queuedCount, 0);
      expect(bufferPool.hasOutstandingCheckouts, isFalse);
    },
  );

  test(
    'TC-1121 E1b: a RUN of failing decodes never drains the slot set — the '
    'tenth failure is delivered as promptly as the first',
    () async {
      pool = CeyxDecodePool(width: 1, entryPoint: poolErrorDeliveryWorker);
      for (var i = 0; i < 10; i++) {
        await expectLater(
          pool.decode('nativeerr:-208:file$i.arw'),
          throwsA(isA<RawDecodeException>()),
        );
      }
      expect(bufferPool.hasOutstandingCheckouts, isFalse);
      expect(bufferPool.hasWaiters, isFalse);
    },
  );

  test(
    'TC-1122 E2: a result the pool cannot materialise fails the future '
    'instead of throwing inside the port listener and stranding it',
    () async {
      pool = CeyxDecodePool(width: 1, entryPoint: poolErrorDeliveryWorker);
      await expectLater(
        pool.decode('badpayload:raw_sample.arw'),
        throwsA(isA<Object>()),
      );
      expect(pool.inFlightCount, 0);
      expect(bufferPool.hasOutstandingCheckouts, isFalse);
    },
  );

  test(
    'TC-1123 E3: worker isolate death mid-request fails the in-flight future '
    'and returns its slot',
    () async {
      pool = CeyxDecodePool(width: 1, entryPoint: poolErrorDeliveryWorker);
      await expectLater(
        pool.decode('crash:raw_sample.arw'),
        throwsA(isA<CeyxPoolWorkerDiedException>()),
      );
      expect(pool.inFlightCount, 0);
      expect(bufferPool.hasOutstandingCheckouts, isFalse);
    },
  );

  test(
    'TC-1124 E4: dispose() fails a job parked on slot acquisition — a job in '
    'that window is in no queue and was previously unreachable forever',
    () async {
      pool = CeyxDecodePool(width: 1, entryPoint: poolErrorDeliveryWorker);
      // Exhaust the two-slot pool by hand so the decode below must wait.
      final held = <CeyxNativeBuffer>[
        await bufferPool.acquire(4096),
        await bufferPool.acquire(4096),
      ];
      // The expectation is attached HERE, before dispose(): the fix completes
      // this future synchronously inside dispose(), and a future with no error
      // handler at that instant reports an unhandled error.
      final pending = pool.decode('parked.arw');
      final expectation = expectLater(
        pending,
        throwsA(isA<CeyxPoolUnavailableException>()),
      );
      // Let the probe land and the acquire park.
      await Future<void>.delayed(const Duration(milliseconds: 50));
      expect(bufferPool.hasWaiters, isTrue);

      await pool.dispose();
      await expectation;
      for (final b in held) {
        bufferPool.release(b);
      }
    },
  );

  test(
    'TC-1125: the success path is untouched — a normal pooled decode still '
    'returns a DngImage that holds its slot until releaseToPool()',
    () async {
      pool = CeyxDecodePool(width: 1, entryPoint: poolErrorDeliveryWorker);
      final image = await pool.decode('ok.arw');
      expect(image.width, 2);
      expect(image.height, 2);
      expect(bufferPool.hasOutstandingCheckouts, isTrue);
      image.releaseToPool();
      expect(bufferPool.hasOutstandingCheckouts, isFalse);
    },
  );
}

// Referenced so the analyzer keeps the ffi import honest when the file is
// edited: the fake worker's pooled replies echo an address the POOL allocated,
// and calloc/Pointer are the types that ownership is expressed in.
// ignore: unused_element
Pointer<Uint8> _unusedAllocationProbe() => calloc<Uint8>(1);
