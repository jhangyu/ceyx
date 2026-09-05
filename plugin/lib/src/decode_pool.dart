import 'dart:async';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:meta/meta.dart';

import 'dng_decoder_service.dart';

/*
---
file_summary: "常駐 worker isolate pool：dylib 每 worker 只載入一次，取代每次操作 Isolate.run"
modules:
  - name: "Protocol"
    description: "pool <-> worker 之間的訊息常數與 job 型別"
  - name: "CeyxDecodePool"
    description: "admission、coalescing、generation gating、crash respawn"
  - name: "ceyxDecodeWorkerMain"
    description: "正式 worker entry point：一次 initialize()，之後跑 job 迴圈"
---
*/

/// What a pool job asks a worker to do.
///
/// Exactly two types, per the P2 design: `decode` replaces
/// [DngDecoderService.decodeOnWorker]'s per-call `Isolate.run`, `probe`
/// replaces [DngDecoderService.getPreviewJpegOnWorker]'s. Both used to pay a
/// spawn plus a fresh `DynamicLibrary` open per operation; on a pool worker the
/// dylib is opened once per worker for the process lifetime.
enum CeyxPoolJobType {
  /// Extract the embedded preview JPEG (returns `Uint8List?`).
  probe,

  /// Full RAW/DNG decode (returns [DngImage]).
  decode,
}

/// Result of a pool submission.
///
/// `value` is null exactly when [discarded] is true: the job completed but its
/// generation was stale by the time the bytes arrived, so the pool dropped the
/// payload WITHOUT materialising it (the materialise is the expensive half —
/// discarding before it is the point of soft cancellation).
///
/// This is deliberately NOT a fourth `NativeImageResult` variant: it never
/// reaches Halcyon's image-source contract (AD-010/AD-011). Halcyon's
/// [DngFullDecoder]-shaped adapter converts a discard into a throw, which the
/// existing "any throw ⇒ fall back" rule already handles.
class CeyxPoolOutcome<T> {
  const CeyxPoolOutcome.value(T this.value, this.generation)
    : discarded = false;
  const CeyxPoolOutcome.discarded(this.generation)
    : value = null,
      discarded = true;

  final T? value;
  final bool discarded;

  /// The generation the job carried when it was submitted (refreshed by a
  /// later coalesced submit).
  final int generation;
}

/// Thrown when a submitted job's result was dropped by generation gating.
class CeyxPoolDiscardedException implements Exception {
  CeyxPoolDiscardedException(this.jobGeneration, this.poolGeneration);

  final int jobGeneration;
  final int poolGeneration;

  @override
  String toString() =>
      'CeyxPoolDiscardedException(job=$jobGeneration, pool=$poolGeneration)';
}

/// Thrown for a job that was lost because its worker died.
class CeyxPoolWorkerDiedException implements Exception {
  CeyxPoolWorkerDiedException(this.workerIndex, this.detail);

  final int workerIndex;
  final String detail;

  @override
  String toString() =>
      'CeyxPoolWorkerDiedException(worker=$workerIndex): $detail';
}

/// Thrown when every worker failed to load the native library.
class CeyxPoolUnavailableException implements Exception {
  CeyxPoolUnavailableException(this.detail);

  final String detail;

  @override
  String toString() => 'CeyxPoolUnavailableException: $detail';
}

// --- wire protocol -------------------------------------------------------
// Messages are plain Lists so the protocol stays inspectable and so a test
// worker can be written without importing any private symbol.

/// `[kMsgReady, SendPort jobPort]` — worker booted and loaded its dylib.
const String kMsgReady = 'ready';

/// `[kMsgUnavailable, String detail]` — worker could not load the dylib.
const String kMsgUnavailable = 'unavailable';

/// `[kMsgResult, int requestId, Object? payload...]`
const String kMsgResult = 'result';

/// `[kMsgError, int requestId, Object error]`
const String kMsgError = 'error';

/// `[kMsgJob, int requestId, int jobTypeIndex, String path, int? maxDim]`
const String kMsgJob = 'job';

/// `[kMsgShutdown]` — worker should close its port and exit.
const String kMsgShutdown = 'shutdown';

/// `[kMsgConfigSlots, int requested]` — the pool asks the worker to configure
/// the PROCESS-global native slot cap. Broadcast to every ready worker on a
/// width change; a worker that boots later receives the value in its bootstrap
/// instead, so there is no window in which a fresh worker runs at the old cap.
const String kMsgConfigSlots = 'config_slots';

/// `[kMsgSlotsAck, int effective, int? rec24, int? rec61, int? rec108]`
///
/// `effective` is the value the native layer actually adopted, or -1 when the
/// loaded dylib predates the configurable cap. The pool logs any difference
/// from the request, because a silently narrowed cap is the exact defect class
/// R4 item 1 exists to remove.
///
/// The three trailing values are this machine's ADVISORY recommended widths
/// for the 24 MP / 61 MP / 108 MP resolution classes (ruling r-6), so the
/// host's settings UI can display them. They ride along on the ack rather than
/// having their own request because the UI isolate deliberately never opens
/// the dylib — only pool workers do — and these figures are constant for the
/// life of the process. Absent (short message) when unsupported.
const String kMsgSlotsAck = 'slots_ack';

const String _kMsgExit = 'exit';

/// Signature of a worker entry point. Must be a top-level or static function.
///
/// The argument is
/// `[SendPort poolPort, String? libraryPath, int workerIndex,
///   int nativeSlotTarget]`.
///
/// R4 item 1 widened this from 3 to 4 elements. Consumers MUST read index 3
/// with a length guard so a worker entry can still be driven by an older host
/// that sends the 3-element form.
typedef CeyxPoolWorkerEntry = void Function(List<Object?> bootstrap);

/// A pool of persistent decode workers.
///
/// One instance is enough for a process; [shared] is the one Halcyon installs
/// behind its `DngFullDecoder` seam.
///
/// Guarantees:
/// * at most [width] jobs execute at once (admission control — the pool paces
///   admissions, never arrivals; the consumer's pacer still owns delivery);
/// * a second submit of the same `(jobType, path, maxDim)` while one is in
///   flight attaches to the in-flight future instead of enqueuing a duplicate;
/// * a result whose job generation is older than [generation] is discarded
///   without materialising its payload;
/// * a worker that dies is respawned (up to [maxRespawns] times per session)
///   and its lost jobs fail with [CeyxPoolWorkerDiedException] — never a hung
///   future.
class CeyxDecodePool {
  CeyxDecodePool({
    int width = 2,
    String? libraryPath,
    CeyxPoolWorkerEntry entryPoint = ceyxDecodeWorkerMain,
    this.maxRespawns = 8,
  }) : _width = width < 1 ? 1 : width,
       _libraryPath = libraryPath,
       _entryPoint = entryPoint;

  /// Process-wide instance used by host apps.
  static final CeyxDecodePool shared = CeyxDecodePool();

  /// Loud-log sink. Host apps replace this to route pool events into their own
  /// perf log; the default prints, because a silently narrowed pool is exactly
  /// the defect class this line exists to prevent.
  static void Function(String line) logger = _defaultLog;

  static void _defaultLog(String line) {
    // ignore: avoid_print
    print(line);
  }

  /// Test-only: how many isolates this pool has spawned, ever. After warmup
  /// this must NOT grow per decode — that is the whole point of the pool.
  @visibleForTesting
  int debugIsolateSpawnCount = 0;

  /// Test-only: how many crash respawns have happened.
  @visibleForTesting
  int debugRespawnCount = 0;

  /// Test-only: how many results were dropped by generation gating.
  @visibleForTesting
  int debugDiscardCount = 0;

  /// Test-only: how many submits attached to an in-flight job.
  @visibleForTesting
  int debugCoalescedCount = 0;

  /// Test-only (R4 item 4 / BLOCKER-1 evidence): how many times `_pump()` was
  /// called while an OUTER `_pump()` call was already on the stack (derived
  /// from the same `_pumping` flag the re-entrancy guard reads, not a second
  /// independent signal). Must stay 0 — the guard's whole job is to make sure
  /// the reentrant body never runs, not merely to bound how deep it goes if
  /// it did.
  @visibleForTesting
  int debugNestedPumpEntries = 0;

  final String? _libraryPath;
  final CeyxPoolWorkerEntry _entryPoint;

  /// Total crash respawns allowed before the pool runs narrower on purpose.
  final int maxRespawns;

  int _width;

  // R4 item 1: the value most recently pushed to the PROCESS-global native
  // slot cap, and the last effective value a worker reported back. Seeded to 0
  // ("nothing pushed yet") rather than to a width, so the first `width`
  // assignment always broadcasts even when it assigns the current default.
  int _nativeSlotTarget = 0;
  int? _lastNativeSlotEffective;

  // ADVISORY recommended widths for the 24/61/108 MP classes (ruling r-6),
  // reported by a worker on its slot ack. Null until a worker with a
  // supporting dylib has answered. Display only — never a clamp.
  List<int>? _nativeRecommendations;

  int _generation = 0;
  int _nextRequestId = 1;
  bool _disposed = false;
  bool _respawnCapped = false;
  bool _libraryUnavailable = false;
  bool _pumping = false;

  final List<_PoolWorker> _workers = [];
  final List<_PoolJob> _queue = [];
  final Map<_JobKey, _PoolJob> _byKey = {};
  final Map<int, _PoolJob> _byRequestId = {};

  /// Live worker slots (ready or still booting).
  int get workerCount => _workers.length;

  /// Jobs handed to a worker and not yet answered.
  int get inFlightCount => _byRequestId.length;

  /// Jobs admitted-but-not-yet-dispatched.
  int get queuedCount => _queue.length;

  /// The current generation. Results from older generations are discarded.
  int get generation => _generation;

  /// How many jobs may execute at once.
  int get width => _width;

  /// Widening spawns lazily on the next dispatch. NARROWING never pre-empts:
  /// no native decode is cancellable, so surplus workers are asked to shut
  /// down and leave after their current job — the same rule `DecodeLane.width`
  /// already follows.
  ///
  /// R4 item 1: the SAME number is pushed to the native slot cap. The two are
  /// assigned together and are deliberately NOT exposed as independent knobs,
  /// so the host's setting cannot reach one layer and miss the other — which
  /// is precisely the defect this item fixes (N Dart workers contending for a
  /// hardcoded 4 native slots).
  set width(int value) {
    _width = value < 1 ? 1 : value;
    _setNativeSlotTarget(_width);
    _retireSurplus();
    _pump();
  }

  /// The value most recently pushed to the native slot cap. Always equal to
  /// [width] once a width has been assigned; exposed for assertions and logs.
  int get nativeSlotTarget => _nativeSlotTarget;

  /// The effective cap the native layer reported, or null before the first ack
  /// (no worker has loaded the library yet). `-1` means the loaded dylib
  /// predates the configurable cap. A positive value below [nativeSlotTarget]
  /// means the dylib bounded the request, which is logged loudly.
  int? get lastNativeSlotEffective => _lastNativeSlotEffective;

  /// This machine's ADVISORY recommended decode widths for the 24 MP / 61 MP /
  /// 108 MP resolution classes, in that order. Null until a worker running a
  /// dylib that supports the query has reported them.
  ///
  /// Ruling r-6: for DISPLAY ONLY. No code path may clamp the user's setting
  /// against these values.
  List<int>? get nativeRecommendations =>
      _nativeRecommendations == null
          ? null
          : List<int>.unmodifiable(_nativeRecommendations!);

  void _setNativeSlotTarget(int value) {
    if (value == _nativeSlotTarget) return;
    _nativeSlotTarget = value;
    for (final w in _workers) {
      if (w.ready && !w.dead) {
        w.sendPort?.send(<Object?>[kMsgConfigSlots, _nativeSlotTarget]);
      }
    }
  }

  /// Bumps the generation; every job submitted before this call is discarded
  /// when it lands. Cheap, synchronous, and the ONLY cancellation there is —
  /// hard native cancel is deliberately out of scope.
  int bumpGeneration() {
    _generation++;
    return _generation;
  }

  /// Submits [type] for [path]. See the class doc for the guarantees.
  ///
  /// [generation] defaults to the pool's current generation (i.e. "do not
  /// cancel me"), which is what callers that do their own post-hoc window
  /// re-check want.
  Future<CeyxPoolOutcome<Object?>> submit(
    CeyxPoolJobType type,
    String path, {
    int? maxDim,
    int? generation,
  }) {
    if (_disposed) {
      return Future.error(
        CeyxPoolUnavailableException('pool disposed'),
        StackTrace.current,
      );
    }
    final gen = generation ?? _generation;
    final key = (type, path, maxDim);
    final existing = _byKey[key];
    if (existing != null) {
      debugCoalescedCount++;
      // A newer submit refreshes the generation so the in-flight work is not
      // discarded on arrival just because an older requester lost interest.
      if (gen > existing.generation) existing.generation = gen;
      return existing.completer.future;
    }
    final job = _PoolJob(
      key: key,
      type: type,
      path: path,
      maxDim: maxDim,
      generation: gen,
    );
    _byKey[key] = job;
    _queue.add(job);
    _pump();
    return job.completer.future;
  }

  /// Convenience wrapper: full decode, throwing on discard so the result type
  /// matches [DngDecoderService.decodeOnWorker] exactly.
  Future<DngImage> decode(String path, {int? maxDim, int? generation}) async {
    final outcome = await submit(
      CeyxPoolJobType.decode,
      path,
      maxDim: maxDim,
      generation: generation,
    );
    if (outcome.discarded) {
      throw CeyxPoolDiscardedException(outcome.generation, _generation);
    }
    return outcome.value! as DngImage;
  }

  /// Convenience wrapper: embedded preview JPEG, or null when there is none.
  /// A discarded probe also returns null (there is nothing to show anyway).
  Future<Uint8List?> probePreviewJpeg(
    String path, {
    int? generation,
  }) async {
    final outcome = await submit(
      CeyxPoolJobType.probe,
      path,
      generation: generation,
    );
    if (outcome.discarded) return null;
    return outcome.value as Uint8List?;
  }

  /// Stops every worker. In-flight jobs fail rather than hang.
  Future<void> dispose() async {
    _disposed = true;
    for (final worker in List<_PoolWorker>.from(_workers)) {
      _shutdown(worker);
    }
    _workers.clear();
    final lost = List<_PoolJob>.from(_byRequestId.values)..addAll(_queue);
    _queue.clear();
    _byRequestId.clear();
    for (final job in lost) {
      _byKey.remove(job.key);
      job.completeError(CeyxPoolUnavailableException('pool disposed'));
    }
  }

  // --- internals ---------------------------------------------------------

  void _pump() {
    if (_disposed) return;
    // RE-ENTRANCY GUARD (BLOCKER-1, round-3 review). `_pump` is entered from
    // several event paths and calls helpers that may want to pump again; a
    // cycle among them recursed until the UI isolate threw StackOverflowError.
    // The cycle itself is broken below (`_failQueuedForNoCapacity` no longer
    // calls back into `_pump`), but this guard makes the whole FAMILY of such
    // cycles impossible rather than just the one instance that was found:
    // the outer loop is still running and will observe whatever the inner
    // call changed.
    //
    // `reentered` reads the SAME `_pumping` flag the guard itself uses (one
    // piece of state, two readers) so the test-only counter below can never
    // disagree with what the guard actually saw.
    final reentered = _pumping;
    if (reentered) debugNestedPumpEntries++;
    if (reentered) return;
    _pumping = true;
    try {
      while (_queue.isNotEmpty) {
        final worker = _idleReadyWorker();
        if (worker == null) {
          _maybeSpawn();
          return;
        }
        _dispatch(worker, _queue.removeAt(0));
      }
    } finally {
      _pumping = false;
    }
  }

  _PoolWorker? _idleReadyWorker() {
    for (final w in _workers) {
      if (w.ready && !w.retiring && w.currentJob == null) return w;
    }
    return null;
  }

  void _maybeSpawn() {
    // Reached only with a non-empty queue and no idle worker. Each early
    // return below therefore means "this queue cannot be served by growing",
    // and must either leave the queue to a busy worker's completion or fail
    // it -- never ask `_pump` to try again, which is what recursed.
    if (_respawnCapped) {
      _failQueuedForNoCapacity('respawn cap reached');
      return;
    }
    if (_libraryUnavailable) {
      // A worker already reported that the dylib will not load. Spawning
      // another would repeat the failed load once per submit forever (the
      // same defect shape as BLOCKER-1, one level up).
      _failQueuedForNoCapacity('native library unavailable');
      return;
    }
    final live = _workers.where((w) => !w.retiring && !w.dead).length;
    if (live >= _width) return;
    if (_workers.any((w) => !w.ready && !w.dead)) {
      // A worker is already booting; it will pick the queue up when ready.
      return;
    }
    _spawn(_workers.length);
  }

  void _spawn(int index) {
    final worker = _PoolWorker(index);
    _workers.add(worker);
    worker.responses = ReceivePort('ceyx-pool-$index');
    worker.responses.listen((msg) => _onWorkerMessage(worker, msg));
    debugIsolateSpawnCount++;
    Isolate.spawn(
          _entryPoint,
          // R4 item 1: the native slot target travels in the bootstrap so a
          // worker spawned AFTER a width change applies it before serving any
          // job, rather than waiting for the next broadcast.
          <Object?>[
            worker.responses.sendPort,
            _libraryPath,
            index,
            _nativeSlotTarget,
          ],
          onExit: worker.responses.sendPort,
          onError: worker.responses.sendPort,
          errorsAreFatal: true,
          debugName: 'ceyx-decode-$index',
        )
        .then((iso) {
          worker.isolate = iso;
          if (worker.dead) iso.kill(priority: Isolate.immediate);
        })
        .catchError((Object e) {
          _onWorkerLost(worker, 'spawn failed: $e');
        });
  }

  void _dispatch(_PoolWorker worker, _PoolJob job) {
    final id = _nextRequestId++;
    job.requestId = id;
    worker.currentJob = job;
    _byRequestId[id] = job;
    worker.sendPort!.send(<Object?>[
      kMsgJob,
      id,
      job.type.index,
      job.path,
      job.maxDim,
    ]);
  }

  void _onWorkerMessage(_PoolWorker worker, Object? raw) {
    if (raw == null) {
      // Isolate.spawn's onExit sends null.
      _onWorkerLost(worker, 'worker isolate exited');
      return;
    }
    if (raw is! List || raw.isEmpty) {
      // onError sends [errorString, stackString].
      _onWorkerLost(worker, 'worker error: $raw');
      return;
    }
    final tag = raw[0];
    if (tag is! String) {
      _onWorkerLost(worker, 'worker error: $raw');
      return;
    }
    switch (tag) {
      case kMsgReady:
        worker.sendPort = raw[1] as SendPort;
        worker.ready = true;
        logger(
          'pool|worker=${worker.index}|ready|workers=$workerCount'
          '|spawned=$debugIsolateSpawnCount',
        );
        if (worker.retiring) {
          _shutdown(worker);
        } else {
          // R4 round-2 should-fix: this worker's slot ack (sent before this
          // very kMsgReady, per the wire protocol) may have arrived with a
          // stale bootstrap value that couldn't be corrected yet because
          // sendPort was still null. Correct it now that the port exists.
          if (worker.pendingSlotResync) {
            worker.pendingSlotResync = false;
            if (_nativeSlotTarget > 0) {
              worker.lastSlotResendTarget = _nativeSlotTarget;
              worker.sendPort!.send(<Object?>[
                kMsgConfigSlots,
                _nativeSlotTarget,
              ]);
            }
          }
          _pump();
        }
        return;
      case kMsgUnavailable:
        worker.dead = true;
        worker.ready = false;
        final detail = '${raw.length > 1 ? raw[1] : 'unknown'}';
        logger('pool|worker=${worker.index}|UNAVAILABLE|$detail');
        // The dylib does not load in THIS process; another worker would fail
        // the same way. Latch it so `_maybeSpawn` stops trying (ceyx's
        // `_unavailableCache` rule, applied to workers instead of calls).
        _libraryUnavailable = true;
        _removeWorker(worker);
        if (_workers.any((w) => !w.dead)) {
          _pump();
        } else {
          _failQueuedForNoCapacity(detail);
        }
        return;
      case kMsgResult:
        _completeJob(worker, raw[1] as int, raw.sublist(2), null);
        return;
      case kMsgError:
        _completeJob(worker, raw[1] as int, null, raw.length > 2 ? raw[2] : null);
        return;
      // R4 item 1. This case MUST stay ahead of `default:` — that arm calls
      // _onWorkerLost, so an unhandled ack would kill the very worker that
      // just successfully configured the native cap. It also deliberately does
      // NOT require worker.ready: the bootstrap ack is sent before kMsgReady.
      case kMsgSlotsAck:
        final effective = raw.length > 1 ? raw[1] as int : -1;
        _lastNativeSlotEffective = effective;
        if (raw.length > 4) {
          _nativeRecommendations = <int>[
            raw[2] as int,
            raw[3] as int,
            raw[4] as int,
          ];
        }
        logger(
          'pool|worker=${worker.index}|slots|requested=$_nativeSlotTarget'
          '|effective=$effective',
        );
        if (effective < 0) {
          logger(
            'pool|SLOT_CONFIG_UNSUPPORTED|library predates the configurable '
            'native slot cap',
          );
        } else if (effective != _nativeSlotTarget) {
          // Under ruling r-6 nothing should narrow the user's request, so this
          // line means the dylib disagreed and we want it loud, not absorbed.
          logger(
            'pool|SLOT_TARGET_NOT_HONOURED|requested=$_nativeSlotTarget'
            '|effective=$effective',
          );
          // R4 round-2 (parked should-fix): a worker spawned just before a
          // width change carries the OLD value in its bootstrap and applies it
          // when it finally lands, which can be after `_setNativeSlotTarget`'s
          // broadcast already moved every READY worker on to the new value —
          // the broadcast in `_setNativeSlotTarget` only reaches ready
          // workers, so a still-booting one is invisible to it and would
          // otherwise leave the PROCESS-global native pool stuck on its stale
          // bootstrap value. The ack is the moment the pool learns what a
          // worker actually applied, so converge here instead.
          if (worker.sendPort != null) {
            if (worker.lastSlotResendTarget != _nativeSlotTarget) {
              // Ask this worker to try the CURRENT target. Guarded so a dylib
              // that genuinely cannot honour the value (or predates the
              // feature, though that case returns -1 above) is asked at most
              // once per target instead of retried forever.
              worker.lastSlotResendTarget = _nativeSlotTarget;
              worker.sendPort!.send(<Object?>[
                kMsgConfigSlots,
                _nativeSlotTarget,
              ]);
            }
          } else {
            // Still booting: this ack arrived ahead of kMsgReady by design
            // (see ceyxDecodeWorkerMain) so there is no send port yet. Defer
            // the correction to the ready handler below.
            worker.pendingSlotResync = true;
          }
        } else {
          // Matches the current target: forget any earlier mismatch so a
          // future divergence from THIS same target is not suppressed by the
          // dedup guard above.
          worker.lastSlotResendTarget = null;
        }
        return;
      case _kMsgExit:
        _onWorkerLost(worker, 'worker signalled exit');
        return;
      default:
        _onWorkerLost(worker, 'unknown worker message: $tag');
        return;
    }
  }

  void _completeJob(
    _PoolWorker worker,
    int requestId,
    List<Object?>? payload,
    Object? error,
  ) {
    final job = _byRequestId.remove(requestId);
    worker.currentJob = null;
    if (job != null) {
      _byKey.remove(job.key);
      if (error != null) {
        job.completeError(error);
      } else if (job.generation < _generation) {
        // Soft cancellation: drop the payload WITHOUT materialising it.
        debugDiscardCount++;
        job.complete(CeyxPoolOutcome<Object?>.discarded(job.generation));
      } else {
        job.complete(
          CeyxPoolOutcome<Object?>.value(
            _materialize(job.type, payload!),
            job.generation,
          ),
        );
      }
    }
    if (worker.retiring) {
      _shutdown(worker);
      return;
    }
    _pump();
  }

  Object? _materialize(CeyxPoolJobType type, List<Object?> payload) {
    switch (type) {
      case CeyxPoolJobType.probe:
        final transfer = payload[0] as TransferableTypedData?;
        return transfer?.materialize().asUint8List();
      case CeyxPoolJobType.decode:
        final transfer = payload[0] as TransferableTypedData;
        return DngImage(
          rgbaData: transfer.materialize().asUint8List(),
          width: payload[1] as int,
          height: payload[2] as int,
          decodeMs: payload[3] as double,
          processMs: payload[4] as double,
        );
    }
  }

  void _onWorkerLost(_PoolWorker worker, String detail) {
    if (worker.dead) return;
    worker.dead = true;
    worker.ready = false;
    final lost = worker.currentJob;
    worker.currentJob = null;
    _removeWorker(worker);

    logger(
      'pool|worker=${worker.index}|DIED|$detail'
      '|respawns=$debugRespawnCount/$maxRespawns',
    );

    if (lost != null) {
      _byRequestId.remove(lost.requestId);
      _byKey.remove(lost.key);
      lost.completeError(CeyxPoolWorkerDiedException(worker.index, detail));
    }

    if (_disposed || worker.retiring) return;

    if (debugRespawnCount >= maxRespawns) {
      // Past the cap the pool runs narrower ON PURPOSE and never silently
      // resurrects a slot through the lazy-spawn path.
      _respawnCapped = true;
      logger(
        'pool|RESPAWN_CAP_REACHED|running narrower|workers=$workerCount',
      );
      // A surviving worker may be idle and able to take the queue right now;
      // only a pool with NO live worker left has to fail it. `_pump` is safe
      // here (this is a port event, not a `_pump` re-entry) and is guarded
      // anyway.
      if (_workers.any((w) => !w.dead)) {
        _pump();
      } else {
        _failQueuedForNoCapacity('respawn cap reached, no worker left');
      }
      return;
    }
    debugRespawnCount++;
    logger('pool|worker=${worker.index}|RESPAWN|#$debugRespawnCount');
    _spawn(worker.index);
    _pump();
  }

  /// Fails every QUEUED job because the pool cannot grow to serve it.
  ///
  /// MUST NOT call [_pump]. It is called from inside `_pump` (via
  /// [_maybeSpawn]) precisely when the queue cannot be dispatched, so pumping
  /// again re-enters the same decision — that cycle was BLOCKER-1's
  /// StackOverflowError (round-3 review). Draining is the job of the events
  /// that actually change capacity: a worker becoming ready, or a job
  /// completing.
  ///
  /// Jobs already IN FLIGHT are untouched, and a surviving busy worker keeps
  /// serving them — a narrowed pool is not a dead pool.
  void _failQueuedForNoCapacity(String detail) {
    final stranded = List<_PoolJob>.from(_queue);
    _queue.clear();
    for (final job in stranded) {
      _byKey.remove(job.key);
      job.completeError(CeyxPoolUnavailableException(detail));
    }
  }

  void _removeWorker(_PoolWorker worker) {
    _workers.remove(worker);
    worker.responses.close();
  }

  void _retireSurplus() {
    var live = _workers.where((w) => !w.retiring && !w.dead).length;
    if (live <= _width) return;
    // Snapshot first: _shutdown mutates _workers.
    final victims = <_PoolWorker>[];
    for (final w in _workers.reversed) {
      if (live <= _width) break;
      if (w.retiring || w.dead) continue;
      w.retiring = true;
      live--;
      victims.add(w);
    }
    for (final w in victims) {
      if (w.ready && w.currentJob == null) _shutdown(w);
    }
  }

  void _shutdown(_PoolWorker worker) {
    if (worker.dead) return;
    worker.dead = true;
    try {
      worker.sendPort?.send(const <Object?>[kMsgShutdown]);
    } catch (_) {
      // Worker already gone; the kill below is the backstop.
    }
    worker.isolate?.kill(priority: Isolate.beforeNextEvent);
    _removeWorker(worker);
  }
}

typedef _JobKey = (CeyxPoolJobType, String, int?);

class _PoolJob {
  _PoolJob({
    required this.key,
    required this.type,
    required this.path,
    required this.maxDim,
    required this.generation,
  });

  final _JobKey key;
  final CeyxPoolJobType type;
  final String path;
  final int? maxDim;
  int generation;
  int? requestId;
  final Completer<CeyxPoolOutcome<Object?>> completer = Completer();

  void complete(CeyxPoolOutcome<Object?> outcome) {
    if (!completer.isCompleted) completer.complete(outcome);
  }

  void completeError(Object error) {
    if (!completer.isCompleted) completer.completeError(error);
  }
}

class _PoolWorker {
  _PoolWorker(this.index);

  final int index;
  late final ReceivePort responses;
  Isolate? isolate;
  SendPort? sendPort;
  bool ready = false;
  bool retiring = false;
  bool dead = false;
  _PoolJob? currentJob;

  // R4 round-2: slot-resync bookkeeping (see kMsgSlotsAck / kMsgReady in
  // CeyxDecodePool._onWorkerMessage). `pendingSlotResync` covers an ack that
  // arrived before sendPort existed; `lastSlotResendTarget` dedups a resend so
  // a dylib that genuinely cannot honour a target is asked once, not forever.
  bool pendingSlotResync = false;
  int? lastSlotResendTarget;
}

/// Production worker entry point.
///
/// Loads the native library ONCE, then serves jobs off its [ReceivePort] until
/// told to shut down. This is the whole reason the pool exists: the previous
/// `Isolate.run`-per-operation path re-opened the dylib on every decode.
///
/// Results cross the boundary as [TransferableTypedData], exactly as
/// [DngDecoderService.decodeOnWorker] already did — the copy-semantics
/// statement in `encode_service.dart` still holds (the bytes are copied into
/// Dart-owned memory inside the worker before transfer, and materialise
/// zero-copy on the pool side).
void ceyxDecodeWorkerMain(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final libraryPath = bootstrap[1] as String?;
  // R4 item 1. Length-guarded so a 3-element bootstrap from an older host
  // still works; 0 means "nothing configured yet, leave the native default".
  final nativeSlotTarget = bootstrap.length > 3 ? bootstrap[3] as int : 0;

  final DngDecoderService service;
  try {
    service = DngDecoderService(libraryPath: libraryPath)..initialize();
  } catch (e) {
    // Report once and leave; the pool degrades to the remaining workers
    // instead of retrying the same load per job (mirrors ceyx's existing
    // `_unavailableCache` behaviour in encode_service.dart).
    poolPort.send(<Object?>[kMsgUnavailable, '$e']);
    return;
  }

  // Configure the PROCESS-global native slot cap from the host's setting
  // BEFORE serving any job, so this worker never runs a decode at a stale cap.
  // The ack is sent before kMsgReady; Dart guarantees per-port message order,
  // and the pool's kMsgSlotsAck case does not require worker.ready, so it is
  // not dropped.
  // Builds the ack, appending this machine's advisory recommendations when the
  // dylib supports the query. Kept in one place so the bootstrap ack and the
  // live-reconfigure ack cannot drift into different shapes.
  List<Object?> slotsAck(int effective) {
    final r24 = service.nativeRecommendedSlots(
      pixels: service.nativeRecommendationClassPixels(0),
    );
    final r61 = service.nativeRecommendedSlots(
      pixels: service.nativeRecommendationClassPixels(1),
    );
    final r108 = service.nativeRecommendedSlots(
      pixels: service.nativeRecommendationClassPixels(2),
    );
    if (r24 < 0 || r61 < 0 || r108 < 0) {
      return <Object?>[kMsgSlotsAck, effective];
    }
    return <Object?>[kMsgSlotsAck, effective, r24, r61, r108];
  }

  if (nativeSlotTarget > 0) {
    poolPort.send(slotsAck(service.configureNativeSlots(nativeSlotTarget)));
  }

  final jobs = ReceivePort('ceyx-decode-worker');
  jobs.listen((Object? message) {
    if (message is! List || message.isEmpty) return;
    if (message[0] == kMsgShutdown) {
      jobs.close();
      return;
    }
    // R4 item 1: live width change. Applies to the PROCESS-global native pool,
    // so whichever worker handles it configures the cap for all of them.
    if (message[0] == kMsgConfigSlots) {
      poolPort.send(slotsAck(service.configureNativeSlots(message[1] as int)));
      return;
    }
    if (message[0] != kMsgJob) return;
    final requestId = message[1] as int;
    final type = CeyxPoolJobType.values[message[2] as int];
    final path = message[3] as String;
    final maxDim = message[4] as int?;
    try {
      switch (type) {
        case CeyxPoolJobType.probe:
          final bytes = service.getPreviewJpeg(path);
          poolPort.send(<Object?>[
            kMsgResult,
            requestId,
            bytes == null ? null : TransferableTypedData.fromList([bytes]),
          ]);
        case CeyxPoolJobType.decode:
          final image = service.decodeForTransfer(path, maxDim: maxDim);
          poolPort.send(<Object?>[kMsgResult, requestId, ...image]);
      }
    } catch (e) {
      Object payload = e;
      try {
        poolPort.send(<Object?>[kMsgError, requestId, payload]);
      } catch (_) {
        // Not every thrown object is sendable; degrade to a typed exception
        // rather than losing the job (a lost job is a hung future).
        payload = DngDecodeException(DngErrorCode.unknownException, '$e');
        poolPort.send(<Object?>[kMsgError, requestId, payload]);
      }
    }
  });

  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}
