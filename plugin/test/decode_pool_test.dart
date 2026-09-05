import 'dart:async';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ceyx/ceyx.dart';
import 'package:flutter_test/flutter_test.dart';

/// Fake worker entry point: same wire protocol as [ceyxDecodeWorkerMain] but
/// loads no dylib, so the pool's scheduling contract is testable on any host.
///
/// Path conventions:
///   `crash:*`     -> throws, killing the worker (errorsAreFatal).
///   `slow:<ms>:*` -> answers after <ms> milliseconds.
///   `nolib`       -> the worker reports `unavailable` at boot and exits.
///
/// R4 item 1: the bootstrap is now 4 elements
/// (`[SendPort, libraryPath, index, nativeSlotTarget]`) and the pool may send
/// [kMsgConfigSlots]. This fake echoes the requested value back as the
/// effective one, standing in for a dylib that honours the request exactly —
/// which is what ruling r-6 requires of the real one.
void fakePoolWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final libraryPath = bootstrap[1] as String?;
  final nativeSlotTarget = bootstrap.length > 3 ? bootstrap[3] as int : 0;
  if (libraryPath == 'nolib') {
    poolPort.send(<Object?>[kMsgUnavailable, 'fake: no dylib']);
    return;
  }
  if (nativeSlotTarget > 0) {
    poolPort.send(<Object?>[kMsgSlotsAck, nativeSlotTarget]);
  }
  final jobs = ReceivePort();
  jobs.listen((Object? message) {
    final msg = message as List<Object?>;
    if (msg[0] == kMsgShutdown) {
      jobs.close();
      return;
    }
    // MUST precede the requestId read below. A config message carries an int
    // at index 1 but nothing at index 2, so falling through would throw a
    // RangeError inside the worker isolate and kill it (errorsAreFatal) — the
    // pool would then report a respawn instead of a slot ack.
    if (msg[0] == kMsgConfigSlots) {
      poolPort.send(<Object?>[kMsgSlotsAck, msg[1] as int]);
      return;
    }
    final requestId = msg[1] as int;
    final type = CeyxPoolJobType.values[msg[2] as int];
    final path = msg[3] as String;
    if (path.startsWith('crash:')) {
      throw StateError('fake worker crash for $path');
    }
    var delayMs = 0;
    if (path.startsWith('slow:')) {
      delayMs = int.parse(path.split(':')[1]);
    }
    void answer() {
      if (type == CeyxPoolJobType.probe) {
        poolPort.send(<Object?>[
          kMsgResult,
          requestId,
          TransferableTypedData.fromList([Uint8List.fromList([1, 2, 3])]),
        ]);
      } else {
        poolPort.send(<Object?>[
          kMsgResult,
          requestId,
          TransferableTypedData.fromList([Uint8List(2 * 2 * 4)]),
          2,
          2,
          1.0,
          2.0,
        ]);
      }
    }

    if (delayMs == 0) {
      answer();
    } else {
      Timer(Duration(milliseconds: delayMs), answer);
    }
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

/// R4 item 1: stands in for a dylib that predates the configurable slot cap,
/// where `DngDecoderService.configureNativeSlots` returns -1. Proves the pool
/// logs the divergence and keeps the worker alive.
void unsupportedSlotsWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final nativeSlotTarget = bootstrap.length > 3 ? bootstrap[3] as int : 0;
  if (nativeSlotTarget > 0) {
    poolPort.send(<Object?>[kMsgSlotsAck, -1]);
  }
  final jobs = ReceivePort();
  jobs.listen((Object? message) {
    final msg = message as List<Object?>;
    if (msg[0] == kMsgShutdown) {
      jobs.close();
      return;
    }
    if (msg[0] == kMsgConfigSlots) {
      poolPort.send(<Object?>[kMsgSlotsAck, -1]);
      return;
    }
    final requestId = msg[1] as int;
    final type = CeyxPoolJobType.values[msg[2] as int];
    if (type == CeyxPoolJobType.probe) {
      poolPort.send(<Object?>[
        kMsgResult,
        requestId,
        TransferableTypedData.fromList([Uint8List.fromList([1, 2, 3])]),
      ]);
    } else {
      poolPort.send(<Object?>[
        kMsgResult,
        requestId,
        TransferableTypedData.fromList([Uint8List(2 * 2 * 4)]),
        2,
        2,
        1.0,
        2.0,
      ]);
    }
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

/// Boots immediately for workers 0 and 1 but stalls worker 2's boot ack by
/// [_staggeredBootDelay] — models a worker spawned right before a width
/// change that is still booting when the change happens, so it carries the
/// OLD value in its bootstrap (R4 round-2 should-fix, decode_pool.dart
/// kMsgSlotsAck / kMsgReady).
const _staggeredBootDelay = Duration(milliseconds: 150);

void staggeredBootWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final index = bootstrap[2] as int;
  final nativeSlotTarget = bootstrap.length > 3 ? bootstrap[3] as int : 0;

  void boot() {
    if (nativeSlotTarget > 0) {
      poolPort.send(<Object?>[kMsgSlotsAck, nativeSlotTarget]);
    }
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
      void answer() {
        if (type == CeyxPoolJobType.probe) {
          poolPort.send(<Object?>[
            kMsgResult,
            requestId,
            TransferableTypedData.fromList([Uint8List.fromList([1, 2, 3])]),
          ]);
        } else {
          poolPort.send(<Object?>[
            kMsgResult,
            requestId,
            TransferableTypedData.fromList([Uint8List(2 * 2 * 4)]),
            2,
            2,
            1.0,
            2.0,
          ]);
        }
      }

      // Honour the same `slow:<ms>:*` convention as [fakePoolWorker] so a
      // dispatched job keeps this worker busy instead of freeing up
      // instantly and stealing a job meant to prove worker 2's late boot.
      var delayMs = 0;
      if (path.startsWith('slow:')) {
        delayMs = int.parse(path.split(':')[1]);
      }
      if (delayMs == 0) {
        answer();
      } else {
        Timer(Duration(milliseconds: delayMs), answer);
      }
    });
    poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
  }

  if (index >= 2) {
    Timer(_staggeredBootDelay, boot);
  } else {
    boot();
  }
}

void main() {
  late CeyxDecodePool pool;
  final logLines = <String>[];

  setUp(() {
    logLines.clear();
    CeyxDecodePool.logger = logLines.add;
  });

  tearDown(() async {
    await pool.dispose();
    CeyxDecodePool.logger = (_) {};
  });

  test(
    'TC-928: workers are REUSED — spawn count stops growing after warmup',
    () async {
      pool = CeyxDecodePool(width: 2, entryPoint: fakePoolWorker);
      // Warmup: two sequential decodes bring both slots up.
      await pool.decode('a.dng');
      await pool.decode('b.dng');
      final afterWarmup = pool.debugIsolateSpawnCount;
      expect(afterWarmup, lessThanOrEqualTo(2));

      for (var i = 0; i < 20; i++) {
        final image = await pool.decode('file_$i.dng');
        expect(image.width, 2);
        expect(image.rgbaData.length, 2 * 2 * 4);
      }

      // The whole point of the pool: no spawn per decode.
      expect(pool.debugIsolateSpawnCount, equals(afterWarmup));
      expect(pool.debugRespawnCount, isZero);
    },
  );

  test('TC-929: concurrency never exceeds the pool width', () async {
    pool = CeyxDecodePool(width: 3, entryPoint: fakePoolWorker);
    var maxObserved = 0;
    final ticker = Timer.periodic(const Duration(milliseconds: 2), (_) {
      if (pool.inFlightCount > maxObserved) maxObserved = pool.inFlightCount;
    });
    await Future.wait([
      for (var i = 0; i < 12; i++) pool.decode('slow:20:item_$i'),
    ]);
    ticker.cancel();
    expect(maxObserved, greaterThan(0));
    expect(maxObserved, lessThanOrEqualTo(3));
    expect(pool.debugIsolateSpawnCount, lessThanOrEqualTo(3));
  });

  test('TC-930: a second submit for the same key coalesces', () async {
    pool = CeyxDecodePool(width: 2, entryPoint: fakePoolWorker);
    final a = pool.submit(CeyxPoolJobType.decode, 'slow:40:same.dng');
    final b = pool.submit(CeyxPoolJobType.decode, 'slow:40:same.dng');
    expect(pool.debugCoalescedCount, equals(1));
    final results = await Future.wait([a, b]);
    expect(identical(results[0], results[1]), isTrue);
    // A different (path) is NOT coalesced.
    await pool.decode('other.dng');
    expect(pool.debugCoalescedCount, equals(1));
  });

  test(
    'TC-931: a result whose generation is stale is discarded, not delivered',
    () async {
      pool = CeyxDecodePool(width: 2, entryPoint: fakePoolWorker);
      final gen = pool.generation;
      final pending = pool.submit(
        CeyxPoolJobType.decode,
        'slow:60:stale.dng',
        generation: gen,
      );
      pool.bumpGeneration();
      final outcome = await pending;
      expect(outcome.discarded, isTrue);
      expect(outcome.value, isNull);
      expect(pool.debugDiscardCount, equals(1));

      // A job submitted at the CURRENT generation still delivers.
      final fresh = await pool.submit(CeyxPoolJobType.decode, 'fresh.dng');
      expect(fresh.discarded, isFalse);
      expect(fresh.value, isA<DngImage>());
      expect(pool.debugDiscardCount, equals(1));
    },
  );

  test('TC-932: decode() surfaces a stale result as a throw', () async {
    pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
    final pending = pool.decode('slow:60:stale2.dng');
    pool.bumpGeneration();
    await expectLater(pending, throwsA(isA<CeyxPoolDiscardedException>()));
  });

  test(
    'TC-933: a dead worker is respawned loudly and its job fails cleanly',
    () async {
      pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
      await pool.decode('warm.dng');
      final spawnsBefore = pool.debugIsolateSpawnCount;

      await expectLater(
        pool.decode('crash:boom.dng'),
        throwsA(isA<CeyxPoolWorkerDiedException>()),
      );
      expect(pool.debugRespawnCount, equals(1));
      expect(pool.debugIsolateSpawnCount, equals(spawnsBefore + 1));
      expect(logLines.where((l) => l.contains('|DIED|')), isNotEmpty);
      expect(logLines.where((l) => l.contains('|RESPAWN|')), isNotEmpty);

      // The pool still works afterwards.
      final image = await pool.decode('after_crash.dng');
      expect(image.width, equals(2));
    },
  );

  test('TC-934: respawn cap makes the pool fail loudly, never hang', () async {
    pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker, maxRespawns: 1);
    await expectLater(
      pool.decode('crash:one.dng'),
      throwsA(isA<CeyxPoolWorkerDiedException>()),
    );
    await expectLater(
      pool.decode('crash:two.dng'),
      throwsA(isA<CeyxPoolWorkerDiedException>()),
    );
    expect(pool.debugRespawnCount, equals(1));
    expect(
      logLines.where((l) => l.contains('RESPAWN_CAP_REACHED')),
      isNotEmpty,
    );
    // Past the cap, queued work fails cleanly instead of hanging forever.
    await expectLater(
      pool.decode('later.dng'),
      throwsA(isA<CeyxPoolUnavailableException>()),
    );
  });

  test(
    'TC-942: past the respawn cap with a SURVIVING busy worker, a queued job '
    'fails cleanly instead of recursing (BLOCKER-1)',
    () async {
      // Round-3 review repro. TC-934 missed this because width 1 leaves NO
      // live worker, which takes the strand branch; the crash needed a
      // survivor, so that `_failIfNoCapacity` pumped, `_pump` found no idle
      // worker, `_maybeSpawn` saw the cap, and the three re-entered each other
      // until the isolate threw StackOverflowError.
      pool = CeyxDecodePool(width: 2, entryPoint: fakePoolWorker, maxRespawns: 1);
      await Future.wait([pool.decode('a.dng'), pool.decode('b.dng')]);
      expect(pool.workerCount, equals(2));

      await expectLater(
        pool.decode('crash:1.dng'),
        throwsA(isA<CeyxPoolWorkerDiedException>()),
      );
      await expectLater(
        pool.decode('crash:2.dng'),
        throwsA(isA<CeyxPoolWorkerDiedException>()),
      );
      expect(pool.debugRespawnCount, equals(1));
      expect(
        pool.workerCount,
        equals(1),
        reason: 'the survivor must still be there; a capped pool runs '
            'narrower, it does not die',
      );

      // Occupy the survivor, then queue one more job behind it.
      final busy = pool.decode('slow:300:busy.dng');
      await Future<void>.delayed(const Duration(milliseconds: 40));
      expect(pool.inFlightCount, equals(1));

      // Must not recurse, must not hang: a clean failure.
      await expectLater(
        pool.decode('queued.dng'),
        throwsA(isA<CeyxPoolUnavailableException>()),
      );

      // ... and the survivor's in-flight work still completes normally.
      final image = await busy;
      expect(image.width, equals(2));
    },
  );

  test(
    'TC-943: an unavailable dylib is latched, not retried once per submit',
    () async {
      // Same defect shape as BLOCKER-1 one level up: without the latch, every
      // submit spawned another worker that failed the identical load.
      pool = CeyxDecodePool(
        width: 2,
        libraryPath: 'nolib',
        entryPoint: fakePoolWorker,
      );
      for (var i = 0; i < 5; i++) {
        await expectLater(
          pool.decode('f$i.dng'),
          throwsA(isA<CeyxPoolUnavailableException>()),
        );
      }
      expect(
        pool.debugIsolateSpawnCount,
        equals(1),
        reason: 'the pool re-attempted a load it had already been told fails',
      );
    },
  );

  test(
    'TC-935: a worker that cannot load the dylib reports unavailable once',
    () async {
      pool = CeyxDecodePool(
        width: 2,
        libraryPath: 'nolib',
        entryPoint: fakePoolWorker,
      );
      await expectLater(
        pool.decode('anything.dng'),
        throwsA(isA<CeyxPoolUnavailableException>()),
      );
      expect(logLines.where((l) => l.contains('UNAVAILABLE')), isNotEmpty);
      // It did not retry the failing load per job.
      expect(pool.debugIsolateSpawnCount, equals(1));
    },
  );

  test('TC-936: probe jobs share the same workers as decode jobs', () async {
    pool = CeyxDecodePool(width: 2, entryPoint: fakePoolWorker);
    await pool.decode('x.dng');
    final spawns = pool.debugIsolateSpawnCount;
    final bytes = await pool.probePreviewJpeg('x.dng');
    expect(bytes, equals(Uint8List.fromList([1, 2, 3])));
    expect(pool.debugIsolateSpawnCount, equals(spawns));
  });

  test('TC-937: narrowing retires surplus workers, widening spawns', () async {
    pool = CeyxDecodePool(width: 3, entryPoint: fakePoolWorker);
    await Future.wait([
      for (var i = 0; i < 6; i++) pool.decode('slow:15:w_$i'),
    ]);
    expect(pool.workerCount, equals(3));
    pool.width = 1;
    expect(pool.workerCount, equals(1));
    final image = await pool.decode('after_narrow.dng');
    expect(image.height, equals(2));
    pool.width = 2;
    await Future.wait([
      pool.decode('slow:15:g1'),
      pool.decode('slow:15:g2'),
    ]);
    expect(pool.workerCount, equals(2));
  });

  // --- R4 item 1: three-layer parallelism sync ----------------------------
  //
  // These assert the DART half of AC-1a: that a width change reaches every
  // live worker as a native-slot configuration, and that a worker spawned
  // afterwards starts already configured. The NATIVE half (the pool really
  // running at N != 4) is proven by test_slot_config / test_concurrent_decode.
  //
  // Observation happens on the POOL side (logger capture + public getters),
  // never via a top-level list written inside the worker: the worker runs in a
  // different isolate, so such a list is a DIFFERENT list on the pool side and
  // would read empty — a silently always-green assertion.

  test('TC-955: setting width pushes the same value to the native slot target',
      () async {
    pool = CeyxDecodePool(width: 2, entryPoint: fakePoolWorker);
    pool.width = 5;
    expect(pool.nativeSlotTarget, equals(5));
    expect(
      pool.nativeSlotTarget,
      equals(pool.width),
      reason: 'width and native slot target are one setting, not two',
    );
  });

  test('TC-956: a width change broadcasts a slot ack from every live worker',
      () async {
    pool = CeyxDecodePool(width: 2, entryPoint: fakePoolWorker);
    // Two workers become live by serving two concurrent jobs.
    await Future.wait([
      pool.decode('slow:15:a'),
      pool.decode('slow:15:b'),
    ]);
    expect(pool.workerCount, equals(2));

    logLines.clear();
    pool.width = 5;
    // Let the acks land.
    await pool.decode('c.dng');

    expect(pool.lastNativeSlotEffective, equals(5));
    expect(
      logLines.where((l) => l.contains('|slots|requested=5|effective=5')).length,
      equals(2),
      reason: 'one ack per live worker, all carrying the configured value',
    );
  });

  test('TC-957: a worker spawned after a width change is configured at boot',
      () async {
    pool = CeyxDecodePool(width: 1, entryPoint: fakePoolWorker);
    await pool.decode('a.dng');
    pool.width = 6;
    logLines.clear();
    // Widening spawns lazily: these submits are what create the new workers,
    // which must configure themselves from the bootstrap, not from a
    // broadcast they were never present to receive.
    await Future.wait([
      pool.decode('slow:15:b'),
      pool.decode('slow:15:c'),
    ]);
    expect(pool.lastNativeSlotEffective, equals(6));
    expect(
      logLines.any((l) => l.contains('|slots|requested=6|effective=6')),
      isTrue,
    );
  });

  test('TC-958: a width ABOVE the recommendation propagates unclamped (r-6)',
      () async {
    // Ruling r-6: the user setting wins end-to-end. 8 is the host slider
    // maximum and is deliberately higher than a small machine's recommended
    // width; the pool must carry it through untouched, with no clamping of
    // any kind on the propagation path.
    pool = CeyxDecodePool(width: 2, entryPoint: fakePoolWorker);
    await pool.decode('a.dng');
    logLines.clear();
    pool.width = 8;
    await pool.decode('b.dng');

    expect(pool.width, equals(8));
    expect(pool.nativeSlotTarget, equals(8));
    expect(pool.lastNativeSlotEffective, equals(8));
    expect(
      logLines.any((l) => l.contains('SLOT_TARGET_NOT_HONOURED')),
      isFalse,
      reason: 'nothing on the Dart path may narrow the user request',
    );
  });

  test('TC-959: an unsupported library is logged loudly, not silently', () async {
    // The pinned dylib predates the configurable cap, so -1 is the EXPECTED
    // answer until the pin bump. It must be visible rather than absorbed.
    pool = CeyxDecodePool(width: 2, entryPoint: unsupportedSlotsWorker);
    await pool.decode('a.dng');
    pool.width = 4;
    await pool.decode('b.dng');

    expect(pool.lastNativeSlotEffective, equals(-1));
    expect(
      logLines.any((l) => l.contains('SLOT_CONFIG_UNSUPPORTED')),
      isTrue,
    );
    // The worker must survive the ack; an unhandled message would have killed
    // it through the `default:` arm of the pool's message switch.
    expect(pool.workerCount, greaterThan(0));
  });

  test(
    'TC-960: a worker that lands with a stale bootstrap slot value is '
    'corrected to the CURRENT target instead of overwriting the '
    'process-global pool with a stale one',
    () async {
      pool = CeyxDecodePool(width: 1, entryPoint: staggeredBootWorker);
      // Target=3 with no workers yet: no broadcast, just records the value
      // every subsequent spawn's bootstrap will carry.
      pool.width = 3;

      // Spawning is serialised (one boot in flight at a time — see
      // `_maybeSpawn`), so submitting three concurrent jobs spawns worker 0,
      // then (once it is ready) worker 1, then (once THAT is ready) worker 2.
      // Worker 2's boot is artificially slow, so it is still booting — and
      // therefore still carrying bootstrap target 3 — when the width change
      // below fires.
      final futures = [
        pool.decode('slow:400:a'),
        pool.decode('slow:400:b'),
        pool.decode('slow:400:c'),
      ];

      // Long enough for workers 0 and 1 to spawn AND become ready (their boot
      // is immediate), short enough to land well inside worker 2's 150ms
      // artificial boot delay.
      await Future<void>.delayed(const Duration(milliseconds: 60));
      expect(
        pool.workerCount,
        equals(3),
        reason: 'all three must have spawned even though worker 2 is still '
            'booting',
      );

      logLines.clear();
      // Broadcasts only to READY workers (0 and 1). Worker 2 is not ready
      // yet, so it misses this entirely and will land with its stale
      // bootstrap value of 3.
      pool.width = 5;

      await Future.wait(futures);

      final worker2Slots = logLines
          .where((l) => l.contains('worker=2') && l.contains('|slots|'))
          .toList();
      expect(
        worker2Slots.any((l) => l.contains('effective=3')),
        isTrue,
        reason: 'worker 2 must land carrying the stale bootstrap value first',
      );
      expect(
        worker2Slots.any((l) => l.contains('effective=5')),
        isTrue,
        reason: 'the pool must re-send the CURRENT target once worker 2\'s '
            'stale ack reveals the divergence',
      );
      expect(
        pool.lastNativeSlotEffective,
        equals(5),
        reason: 'the process-global pool must end up at the current target, '
            'never left on the stale value a late-booting worker applied',
      );
    },
  );
}
