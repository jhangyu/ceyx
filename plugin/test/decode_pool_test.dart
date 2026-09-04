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
void fakePoolWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final libraryPath = bootstrap[1] as String?;
  if (libraryPath == 'nolib') {
    poolPort.send(<Object?>[kMsgUnavailable, 'fake: no dylib']);
    return;
  }
  final jobs = ReceivePort();
  jobs.listen((Object? message) {
    final msg = message as List<Object?>;
    if (msg[0] == kMsgShutdown) {
      jobs.close();
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
}
