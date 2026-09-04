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

const String _kMsgExit = 'exit';

/// Signature of a worker entry point. Must be a top-level or static function.
///
/// The argument is `[SendPort poolPort, String? libraryPath, int workerIndex]`.
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

  final String? _libraryPath;
  final CeyxPoolWorkerEntry _entryPoint;

  /// Total crash respawns allowed before the pool runs narrower on purpose.
  final int maxRespawns;

  int _width;
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
  set width(int value) {
    _width = value < 1 ? 1 : value;
    _retireSurplus();
    _pump();
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
    if (_pumping) return;
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
          <Object?>[worker.responses.sendPort, _libraryPath, index],
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

  final jobs = ReceivePort('ceyx-decode-worker');
  jobs.listen((Object? message) {
    if (message is! List || message.isEmpty) return;
    if (message[0] == kMsgShutdown) {
      jobs.close();
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
