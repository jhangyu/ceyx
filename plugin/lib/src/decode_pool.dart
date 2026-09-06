import 'dart:async';
import 'dart:collection';
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart' show calloc;
import 'package:meta/meta.dart';

import 'dng_bindings.dart';
import 'dng_decoder_service.dart';
import 'encode_bindings.dart';
import 'encode_service.dart';
import 'native_buffer_pool.dart';

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

  /// WP3a: encode an RGBA8 frame that already lives in native memory
  /// (returns `Uint8List`, the JPEG bytes). APPENDED — never renumbered, same
  /// rule the error codes carry (`encode_bindings.dart:51`).
  encode,

  /// WP10: metadata-only output-extent probe for one `(path, maxDim)`
  /// (returns `[int width, int height]`, both 0 when unsupported).
  ///
  /// Dispatched by the pool ITSELF, never by a host: it is how the main
  /// isolate learns how large a slot to acquire before a pooled decode. Also
  /// APPENDED — the index is the wire value, so this must stay last.
  probeSize,
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
///
/// WP3a: gains an optional trailing 6th element,
/// `List<Object?> encodeArgs = [rgbaAddress, width, height, quality]`, present
/// only for [CeyxPoolJobType.encode] jobs. Read with a length guard
/// (`message.length > 5 ? message[5] as List<Object?> : null`) mirroring the
/// R4 bootstrap-widening rule (`:150-153` below), so an older host driving a
/// newer worker (or vice versa) still works.
/// WP10: gains two more optional trailing elements, `dstAddress` and
/// `dstCapacity`, present only when the MAIN isolate pre-acquired a slot for
/// this job. They sit AFTER `encodeArgs` so the existing `message.length > 5`
/// read is unaffected, and their ABSENCE is the degraded route — a worker that
/// sees no address and a pool that took no slot agree by construction.
const String kMsgJob = 'job';

/// `[kMsgResize, int requestId, int width, int height]` — WP10.
///
/// The worker's decode-into was refused because the pre-acquired slot was too
/// small (the native `kDngErrDstTooSmall` / `kRawErrDstTooSmall` return), and
/// these are the dimensions the NATIVE layer reported. The pool releases the
/// undersized slot, re-acquires `width*height*4`, and re-dispatches the same
/// request id EXACTLY once; a second refusal for one job fails the job rather
/// than looping, because a self-healing retry that can loop is not
/// self-healing.
const String kMsgResize = 'resize';

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
///
/// ## Decode-payload ownership (H2-A)
///
/// A [CeyxPoolJobType.decode] result crosses the port as a bare native ADDRESS
/// plus dimensions ([DngDecoderService.decodeForPointerTransfer]); the ~97MB
/// RGBA buffer is never copied into the Dart heap. Between the worker's send
/// and the pool's handling of that message, NO Dart object owns the buffer.
/// The pool then takes ownership on exactly one of two mutually exclusive
/// branches of [_completeJob]:
/// * fresh generation → [_materialize] wraps it with
///   `asTypedList(len, finalizer: dng_free_rgba_buffer)`; the buffer's lifetime
///   becomes the typed list's lifetime and the engine frees it on GC;
/// * stale generation (soft cancel) → the address is freed EXPLICITLY and no
///   finalizer is ever attached.
/// Because the discard arm never materialises and the materialise arm never
/// frees, a double free is impossible by construction rather than by
/// bookkeeping.
///
/// BOUNDED NATIVE LEAKS (documented on purpose; there is deliberately no
/// reaper — a reaper would need a registry of in-flight addresses, which is
/// the bookkeeping the two-branch design exists to avoid):
/// 1. `kMsgError`: payload is null, nothing crossed — the worker's own
///    `finally` already freed via `dng_free_result`. No leak.
/// 2. Worker death with a natively-completed but never-processed result: ONE
///    buffer, on the PROCESS-global native heap (isolates share it), leaks per
///    worker-death event. Worker deaths are already logged loudly via
///    [logger].
/// 3. [dispose] / shutdown with completed messages still queued: same class,
///    same bound (one buffer per queued completed decode).
///
/// RSS ATTRIBUTION NOTE (risk R-1): for identity-orientation photos the host
/// aliases this native-backed buffer into its payload objects, so bytes that
/// used to show up as Dart heap now show up as native allocation. That is
/// correct behaviour, not a leak — read future memory captures accordingly.
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

  /// H2 discriminator gate: when this returns true, the pool times the
  /// main-isolate payload-landing step — `TransferableTypedData.materialize()`
  /// for a probe, the zero-copy native wrap for a decode — and emits a
  /// `pool.materialize|dur_us=..|bytes=..|type=..` line through [logger].
  /// A callback (not a bool) so hosts can bind it to their own perf-logging
  /// switch (e.g. Halcyon's `PerfLog.enabled`), which may flip after pool
  /// construction. Defaults to off: one function call of overhead per job,
  /// no Stopwatch, no string built.
  static bool Function() materializeTimingEnabled = _timingOff;

  static bool _timingOff() => false;

  /// Test-only seam for native ownership handling. When set, EVERY free the
  /// pool would perform routes here instead of into the dylib, and
  /// [_materialize] attaches no `NativeFinalizer` — so unit tests can drive
  /// the decode path with a fake (e.g. `calloc`-backed) address and no native
  /// library present.
  @visibleForTesting
  static void Function(int address)? debugNativeFree;

  /// WP6: the native buffer pool a decode payload's RGBA buffer came from,
  /// when it came from one.
  ///
  /// The free list lives on THIS isolate (the pool's own), because that is
  /// where returns land; a native address is process-global but a Dart pool
  /// object is not. Null means "no pooled allocation on this route", which is
  /// the state whenever the worker's buffer is allocated by the dylib's own
  /// decode entry (it owns the allocation and `dng_free_rgba_buffer` owns the
  /// free) — the pool then owns nothing and every code path below degrades to
  /// the pre-WP6 behaviour, unchanged.
  static CeyxNativeBufferPool? nativeBufferPool;

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

  // --- WP10: pooled decode-into wiring ------------------------------------

  /// `'$path|$maxDim' -> required bytes`. Ints only, so a long session cannot
  /// grow this into a memory problem of its own; LRU-evicted at
  /// [_kSizeCacheLimit] entries.
  ///
  /// A value of [_kNoPooledRoute] means "this key has no pooled route" (the
  /// probe was unsupported or failed), which is cached exactly like a size so
  /// a permanently-unprobeable path costs one probe, not one per decode.
  final LinkedHashMap<String, int> _sizeCache = LinkedHashMap<String, int>();
  static const int _kSizeCacheLimit = 512;
  static const int _kNoPooledRoute = -1;

  /// Dispatches per path, including retries. Keyed by path rather than request
  /// id because a retry deliberately REUSES its request id (that is what makes
  /// "exactly once" checkable at all).
  final Map<String, int> _dispatchCounts = <String, int>{};

  /// Test-only: how many `probeSize` jobs this pool has dispatched. The size
  /// cache's whole purpose is to keep this at one per `(path, maxDim)`.
  @visibleForTesting
  int debugProbeSizeCount = 0;

  /// Test-only override for "the dylib exports the decode-into pair".
  /// Null means "ask the library". Exists because a plugin unit test has no
  /// dylib to ask, and because forcing it FALSE is the only way to test the
  /// degraded route that every currently pinned Halcyon build is actually in.
  ///
  /// ONE override, matching the one real flag: the entries are format-agnostic
  /// and live in an always-compiled TU, so there is no build in which they are
  /// available for one format and absent for another. A format this build
  /// cannot decode is signalled by the PROBE returning no extent, which this
  /// pool already caches as "no pooled route".
  @visibleForTesting
  static bool? debugDecodeIntoAvailable;

  @visibleForTesting
  void debugSeedSizeCache(String path, int? maxDim, int bytes) =>
      _sizeCache['$path|$maxDim'] = bytes;

  @visibleForTesting
  int? debugSizeCacheFor(String path, int? maxDim) =>
      _sizeCache['$path|$maxDim'];

  @visibleForTesting
  int debugDispatchCountFor(String path) => _dispatchCounts[path] ?? 0;

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
    if (type == CeyxPoolJobType.decode && _pooledRouteEnabled) {
      // WP10: the slot is acquired HERE, on the pool's own isolate, because
      // that is where CeyxNativeBufferPool's free list lives. Preparation is
      // asynchronous (a probe may have to run first), so the job is registered
      // in `_byKey` above BEFORE the await — a concurrent submit for the same
      // key must coalesce onto it rather than start a second probe.
      unawaited(_prepareAndEnqueue(job));
    } else {
      _queue.add(job);
      _pump();
    }
    return job.completer.future;
  }

  /// Whether a decode may take the pooled decode-into route: a pool must exist
  /// to take a slot from, and the dylib must export the entry pair.
  ///
  /// Deliberately NOT per-path. The entries are format-agnostic, so there is
  /// nothing to ask about a path here; a format this build cannot decode is
  /// discovered by the PROBE (which answers no extent) and cached as "no
  /// pooled route", so the slot is never acquired in the first place.
  bool get _pooledRouteEnabled {
    if (nativeBufferPool == null) return false;
    return debugDecodeIntoAvailable ?? _libraryDecodeInto();
  }

  /// The availability flag read off the loaded library, resolved once.
  ///
  /// A failure to open the library is NOT rethrown here (unlike
  /// [_freeBindings], where a missing free symbol would mean a permanent
  /// leak): "cannot ask" and "not present" both mean the pooled route stays
  /// unreachable, which is exactly today's behaviour.
  bool _libraryDecodeInto() {
    final cached = _decodeIntoCache;
    if (cached != null) return cached;
    bool resolved;
    try {
      resolved = _freeBindings.decodeIntoBufferAvailable;
    } catch (e) {
      logger('pool|DECODE_INTO_UNAVAILABLE|$e');
      resolved = false;
    }
    _decodeIntoCache = resolved;
    return resolved;
  }

  bool? _decodeIntoCache;

  /// WP10 preparation: learn the output extent (probing once per
  /// `(path, maxDim)`), acquire a slot of that size, then enqueue the job.
  ///
  /// Any failure degrades to the ordinary allocating route rather than failing
  /// the decode: this whole path is an optimisation, and a photo that will not
  /// probe must still open.
  Future<void> _prepareAndEnqueue(_PoolJob job) async {
    try {
      var bytes = _sizeCache[_sizeKey(job.path, job.maxDim)];
      if (bytes == null) {
        bytes = await _probeSizeFor(job.path, job.maxDim);
        if (_disposed || job.completer.isCompleted) return;
      }
      if (bytes != null && bytes > 0) {
        job.slot = await nativeBufferPool?.acquire(bytes);
        if (_disposed || job.completer.isCompleted) {
          // Raced with dispose/cancel while awaiting: return the slot rather
          // than stranding it checked out forever.
          _releaseSlot(job);
          return;
        }
      }
    } catch (e) {
      // Probe or acquire blew up. Drop the slot idea and take the old route.
      logger('pool|POOLED_PREPARE_FAILED|${job.path}|$e');
      _releaseSlot(job);
    }
    if (_disposed || job.completer.isCompleted) {
      _releaseSlot(job);
      return;
    }
    _queue.add(job);
    _pump();
  }

  /// Dispatches ONE `probeSize` job and caches its answer. Returns null when
  /// the extent is not knowable, which is itself cached (as [_kNoPooledRoute])
  /// so an unprobeable path costs one probe for the session, not one per
  /// decode.
  Future<int?> _probeSizeFor(String path, int? maxDim) async {
    final key = _sizeKey(path, maxDim);
    debugProbeSizeCount++;
    final probe = _PoolJob(
      key: (CeyxPoolJobType.probeSize, path, maxDim),
      type: CeyxPoolJobType.probeSize,
      path: path,
      maxDim: maxDim,
      generation: _generation,
    );
    _byKey[probe.key] = probe;
    _queue.add(probe);
    _pump();

    final CeyxPoolOutcome<Object?> outcome;
    try {
      outcome = await probe.completer.future;
    } catch (e) {
      logger('pool|PROBE_SIZE_FAILED|$path|$e');
      _putSizeCache(key, _kNoPooledRoute);
      return null;
    }
    final size = outcome.value;
    if (size is! List<Object?> || size.length < 2) {
      _putSizeCache(key, _kNoPooledRoute);
      return null;
    }
    final width = size[0] as int;
    final height = size[1] as int;
    if (width <= 0 || height <= 0) {
      // The dylib predates the entry for this route, or the probe failed.
      _putSizeCache(key, _kNoPooledRoute);
      return null;
    }
    final bytes = width * height * 4;
    _putSizeCache(key, bytes);
    return bytes;
  }

  static String _sizeKey(String path, int? maxDim) => '$path|$maxDim';

  void _putSizeCache(String key, int bytes) {
    _sizeCache.remove(key);
    _sizeCache[key] = bytes;
    while (_sizeCache.length > _kSizeCacheLimit) {
      // LinkedHashMap iterates in insertion order, and the re-insert above
      // makes "first" the least recently written — LRU without a second
      // structure to keep in sync.
      _sizeCache.remove(_sizeCache.keys.first);
    }
  }

  /// Returns [job]'s slot to the buffer pool, if it still holds one.
  void _releaseSlot(_PoolJob job) {
    final slot = job.slot;
    if (slot == null) return;
    job.slot = null;
    nativeBufferPool?.release(slot);
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

  /// WP3a: encodes an RGBA8 frame that already lives in native memory at
  /// [rgbaAddress] as a baseline JPEG, dispatched to an ALREADY-RUNNING pool
  /// worker (no per-call isolate spawn, no `malloc`+copy of the pixels — the
  /// worker calls the jpeg binding on `Pointer.fromAddress(rgbaAddress)`
  /// directly).
  ///
  /// Unlike [submit], encode jobs are NOT subject to generation-based soft
  /// cancellation: [CeyxPoolJobType.decode]'s generation gate exists to drop
  /// superseded navigation results, which has no meaning for an encode a
  /// caller is actively awaiting. An encode job therefore always delivers its
  /// value or its error, never a [CeyxPoolOutcome.discarded]. It is also never
  /// coalesced with another submission (each call encodes a distinct buffer).
  ///
  /// Throws [CeyxEncodeUnavailableException] when the loaded dylib lacks the
  /// encode symbols, or a `CeyxEncodeException`/`ArgumentError` mirroring the
  /// worker's own validation — identical to [CeyxEncodeService]'s existing
  /// throw contract, so a caller's existing fallback policy applies unchanged.
  Future<Uint8List> submitEncode({
    required int rgbaAddress,
    required int width,
    required int height,
    required int quality,
  }) {
    if (_disposed) {
      return Future.error(
        CeyxPoolUnavailableException('pool disposed'),
        StackTrace.current,
      );
    }
    final job = _PoolJob(
      key: (CeyxPoolJobType.encode, 'encode:${_nextEncodeKey++}', null),
      type: CeyxPoolJobType.encode,
      path: '',
      maxDim: null,
      generation: _generation,
      encodeArgs: <Object?>[rgbaAddress, width, height, quality],
    );
    _byKey[job.key] = job;
    _queue.add(job);
    _pump();
    return job.completer.future.then(
      (outcome) => outcome.value! as Uint8List,
    );
  }

  // Synthetic per-call discriminator so every encode submission gets its own
  // `_byKey` entry — encode jobs must never coalesce (each call targets a
  // distinct native buffer, unlike a decode's `(type, path, maxDim)` key).
  int _nextEncodeKey = 0;

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
      // WP10: a job dying with a slot still checked out would strand that slot
      // for the pool's lifetime. Its payload never landed, so nothing is
      // reading the memory and returning it here is safe.
      _releaseSlot(job);
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
    // WP10: a resize retry REUSES its request id on purpose. The retry is the
    // same job, and reusing the id is what makes "dispatched exactly twice"
    // observable instead of being two unrelated ids that have to be correlated
    // after the fact.
    final id = job.awaitingRetry ? job.requestId! : _nextRequestId++;
    job.awaitingRetry = false;
    job.requestId = id;
    worker.currentJob = job;
    _byRequestId[id] = job;
    _dispatchCounts[job.path] = (_dispatchCounts[job.path] ?? 0) + 1;
    final encodeArgs = job.encodeArgs;
    final slot = job.slot;
    worker.sendPort!.send(<Object?>[
      kMsgJob,
      id,
      job.type.index,
      job.path,
      job.maxDim,
      // WP10: additive, and present ONLY when a slot was pre-acquired on this
      // isolate. Absent entirely on the degraded route, so a worker that
      // ignores them and a pool that never took a slot agree by construction.
      //
      // The two fields sit at FIXED indices 6 and 7, which means index 5 must
      // be occupied even for a decode job that has no encodeArgs — otherwise a
      // collection-if would collapse the list and slide the address into the
      // encodeArgs slot, where the encode reader would find an int. A pooled
      // decode therefore sends an explicit null placeholder.
      if (slot != null) ...<Object?>[encodeArgs, slot.address, slot.capacity]
      // No slot: byte-for-byte the pre-WP10 shape (length 5, or 6 for encode).
      else if (encodeArgs != null)
        encodeArgs,
    ]);
  }

  /// WP10: the worker refused the pre-acquired slot as too small and reported
  /// the extent the native layer actually needs.
  void _onResize(_PoolWorker worker, int requestId, int width, int height) {
    final job = _byRequestId[requestId];
    if (job == null) return;
    final bytes = width * height * 4;

    if (job.pooledAbandoned) {
      // A refusal on the UNPOOLED fallback, where this job was handed no
      // buffer at all. Nothing was refused because nothing was given, so the
      // worker is broken rather than the file being awkward. Fail: an
      // unbounded resize loop hangs a decode forever, which is worse than
      // either honest outcome.
      _byRequestId.remove(requestId);
      _byKey.remove(job.key);
      worker.currentJob = null;
      _releaseSlot(job);
      job.completeError(
        StateError(
          'decode-into refused for ${job.path} after the pool had already '
          'withdrawn the buffer: the worker asked for $bytes bytes for a job '
          'carrying no dstAddress',
        ),
      );
      _pump();
      return;
    }

    if (job.resized) {
      // SECOND refusal (erratum E-WP10-1072). This is NOT necessarily a lying
      // probe: a -301 has two sources, and the RAW post-unpack capacity
      // backstop can legitimately refuse a retry that was sized from the FIRST
      // refusal's extent, for formats whose geometry shifts during unpack
      // (R11.1, X3F/Foveon).
      //
      // Failing here would mean a photo that opened fine BEFORE WP10 stops
      // opening because of WP10. So withdraw the buffer and dispatch once more
      // on the ordinary allocating route: the photo still opens, degraded.
      //
      // The POOLED retry is still bounded at exactly one, which is the
      // property AC12.3 exists to protect. This cannot loop: with no
      // dstAddress in the message the worker takes the allocating route and
      // has nothing to refuse — and if it refuses anyway, the guard above
      // fails the job.
      logger(
        'pool|DECODE_INTO_REFUSED_TWICE|${job.path}|supplied='
        '${job.slot?.capacity}|requested=$bytes|falling back to the '
        'allocating route',
      );
      job.pooledAbandoned = true;
      _byRequestId.remove(requestId);
      worker.currentJob = null;
      _releaseSlot(job);
      job.awaitingRetry = true;
      _queue.add(job);
      _pump();
      return;
    }
    job.resized = true;
    _putSizeCache(_sizeKey(job.path, job.maxDim), bytes);
    _byRequestId.remove(requestId);
    worker.currentJob = null;
    _releaseSlot(job);
    unawaited(_reacquireAndRedispatch(job, bytes));
    _pump();
  }

  /// Re-acquires a slot at the extent native reported and re-queues the SAME
  /// job for one more dispatch.
  Future<void> _reacquireAndRedispatch(_PoolJob job, int bytes) async {
    try {
      job.slot = await nativeBufferPool?.acquire(bytes);
    } catch (e) {
      logger('pool|RESIZE_REACQUIRE_FAILED|${job.path}|$e');
      // Fall through with no slot: the retry takes the ordinary allocating
      // route, which still produces a correct image.
      job.slot = null;
    }
    if (_disposed || job.completer.isCompleted) {
      _releaseSlot(job);
      return;
    }
    job.awaitingRetry = true;
    _queue.add(job);
    _pump();
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
      // WP10. MUST stay ahead of `default:`, which calls _onWorkerLost — an
      // unhandled resize would kill the worker that just correctly refused an
      // undersized buffer.
      case kMsgResize:
        _onResize(worker, raw[1] as int, raw[2] as int, raw[3] as int);
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
        // WP10: the decode failed, so nothing wrapped the slot and nothing is
        // reading it. Returning it here is what keeps a run of failing files
        // from draining the fixed slot set. (On SUCCESS the slot is
        // deliberately NOT released: ownership passes to the DngImage, whose
        // releaseToPool() returns it at end of consumption.)
        _releaseSlot(job);
        job.completeError(error);
      } else if (job.type != CeyxPoolJobType.encode &&
          job.type != CeyxPoolJobType.probeSize &&
          job.generation < _generation) {
        // Soft cancellation: drop the payload WITHOUT materialising it.
        //
        // H2-A: a decode payload carries a raw native address whose ownership
        // was shipped over the port, so "dropping" it here would leak ~97MB
        // per superseded decode. This arm frees it EXACTLY ONCE and never
        // reaches _materialize, which is the only place a finalizer is ever
        // attached — hence no double free is possible.
        debugDiscardCount++;
        if (job.type == CeyxPoolJobType.decode &&
            payload != null &&
            payload.isNotEmpty) {
          _freeNativeRgba(payload[0] as int);
        }
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
        return transfer == null ? null : _materializeBytes(transfer, type);
      case CeyxPoolJobType.decode:
        // H2-A: payload[0] is a native address, not a TransferableTypedData.
        final width = payload[1] as int;
        final height = payload[2] as int;
        final address = payload[0] as int;
        return DngImage(
          rgbaData: _wrapNativeRgba(address, width * height * 4, type),
          width: width,
          height: height,
          decodeMs: payload[3] as double,
          processMs: payload[4] as double,
          nativeAddress: address,
          // WP6 explicit release entry. Null when no pool owns this address,
          // so `releaseToPool()` stays a safe no-op on every non-pooled route
          // rather than pretending to reclaim something.
          onReleaseToPool: _releaseToPoolCallbackFor(address),
        );
      case CeyxPoolJobType.encode:
        // WP3a: the worker returns the encoded JPEG as TransferableTypedData
        // (a ~2MB copy, kept: the result must become Dart-owned).
        return _materializeBytes(payload[0] as TransferableTypedData, type);
      case CeyxPoolJobType.probeSize:
        // WP10: two ints, no payload to own. Handed back as the raw list so
        // the caller (_probeSizeFor, on this isolate) reads width/height
        // without a type that exists only to carry them one frame.
        return payload;
    }
  }

  /// The materialize step, optionally timed (H2 discriminator). Instrumentation
  /// only — the returned bytes are identical either way.
  Uint8List _materializeBytes(
    TransferableTypedData transfer,
    CeyxPoolJobType type,
  ) {
    if (!materializeTimingEnabled()) {
      return transfer.materialize().asUint8List();
    }
    final sw = Stopwatch()..start();
    final bytes = transfer.materialize().asUint8List();
    sw.stop();
    logger(
      'pool.materialize|dur_us=${sw.elapsedMicroseconds}'
      '|bytes=${bytes.length}|type=${type.name}',
    );
    return bytes;
  }

  /// Zero-copy view over the worker's native RGBA buffer, taking ownership via
  /// a `NativeFinalizer` bound to `dng_free_rgba_buffer`. Attached exactly
  /// once, by the only code path that ever touches this address.
  ///
  /// The `pool.materialize` event is retained here (FROZEN format) so the perf
  /// recapture can still attribute payload landings; under pointer transfer it
  /// measures the wrap, which is sub-µs by construction — that near-zero value
  /// IS the evidence the copy is gone.
  Uint8List _wrapNativeRgba(int address, int length, CeyxPoolJobType type) {
    if (!materializeTimingEnabled()) {
      return _viewNativeRgba(address, length);
    }
    final sw = Stopwatch()..start();
    final bytes = _viewNativeRgba(address, length);
    sw.stop();
    logger(
      'pool.materialize|dur_us=${sw.elapsedMicroseconds}'
      '|bytes=${bytes.length}|type=${type.name}',
    );
    return bytes;
  }

  /// WP6: the explicit end-of-consumption entry for [address], or null when no
  /// pool owns it.
  void Function()? _releaseToPoolCallbackFor(int address) {
    final buffers = nativeBufferPool;
    if (buffers == null || !buffers.ownsAddress(address)) return null;
    return () => buffers.tryReleaseByAddress(address);
  }

  /// WP6 safety net. A pooled buffer is meant to come back through
  /// `DngImage.releaseToPool()`; this Dart [Finalizer] catches the paths that
  /// forget, and its reclaim is counted separately
  /// (`CeyxNativeBufferPool.debugFinalizerReleases`) precisely so a test can
  /// assert it never fired. It is attached ONLY for pool-owned addresses — a
  /// dylib-owned buffer keeps its existing `NativeFinalizer`, which frees, and
  /// no address is ever owned by both.
  static final Finalizer<int> _poolSafetyNet = Finalizer<int>((address) {
    CeyxDecodePool.nativeBufferPool?.releaseByAddressFromFinalizer(address);
  });

  Uint8List _viewNativeRgba(int address, int length) {
    final ptr = Pointer<Uint8>.fromAddress(address);
    final buffers = nativeBufferPool;
    if (buffers != null && buffers.ownsAddress(address)) {
      // Pool-owned: NO NativeFinalizer (freeing it would take the buffer away
      // from the pool). The safety net returns it instead.
      final bytes = ptr.asTypedList(length);
      _poolSafetyNet.attach(bytes, address, detach: bytes);
      return bytes;
    }
    if (debugNativeFree != null) {
      // Test seam: no dylib is loaded, so no finalizer may be attached. The
      // test owns the fake allocation and frees it through the seam.
      return ptr.asTypedList(length);
    }
    return ptr.asTypedList(length, finalizer: _nativeFreePtr);
  }

  /// Explicit free for the ONE arm that never materialises (soft cancel).
  void _freeNativeRgba(int address) {
    // WP6: a pool-owned buffer is RETURNED, not freed — "release" on the
    // discard arm keeps its meaning (this address is done) while the memory
    // stays in the fixed slot set. Still exactly one reclaim per address: the
    // discard arm never materialises, so no safety net was ever attached.
    if (nativeBufferPool?.tryReleaseByAddress(address) ?? false) return;
    final seam = debugNativeFree;
    if (seam != null) {
      seam(address);
      return;
    }
    if (address == 0) return;
    _freeBindings.dngFreeRgbaBuffer(Pointer<Void>.fromAddress(address));
  }

  Pointer<NativeFinalizerFunction> get _nativeFreePtr =>
      _freeBindings.dngFreeRgbaBufferPtr.cast();

  /// Symbol-table access to `dng_free_rgba_buffer` on the POOL's isolate (the
  /// host's UI isolate in production).
  ///
  /// This opens the native library on the main isolate — previously only
  /// workers did — but it never calls a decode entry point: the constructor
  /// performs `dlsym`-class lookups only. `dlopen` is refcounted, so the OS
  /// loader hands back the image the worker isolates already mapped rather
  /// than loading a second copy.
  ///
  /// The path is resolved EXACTLY as the workers resolve it: with the pool's
  /// [_libraryPath] when the host supplied one (the same value that travels in
  /// `bootstrap[1]`), and otherwise through the identical
  /// [DngNativeBindings.load] candidate search the worker runs — which is the
  /// production case, since hosts construct [shared] without a path.
  ///
  /// A failure to resolve is rethrown, not swallowed: silently skipping the
  /// finalizer would turn every decode into a permanent 97MB native leak.
  DngNativeBindings get _freeBindings {
    final cached = _freeBindingsCache;
    if (cached != null) return cached;
    try {
      final bindings = _libraryPath == null
          ? DngNativeBindings.load()
          : DngNativeBindings.fromPath(_libraryPath);
      _freeBindingsCache = bindings;
      return bindings;
    } catch (e) {
      logger('pool|NATIVE_FREE_UNAVAILABLE|$e');
      rethrow;
    }
  }

  DngNativeBindings? _freeBindingsCache;

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
      // WP10: same reasoning as in dispose() — the payload never landed, so
      // the slot is unreferenced and must go back rather than leak.
      _releaseSlot(lost);
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
      // WP10: a queued job may already hold a pre-acquired slot.
      _releaseSlot(job);
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
    this.encodeArgs,
  });

  final _JobKey key;
  final CeyxPoolJobType type;
  final String path;
  final int? maxDim;
  int generation;
  // WP3a: [rgbaAddress, width, height, quality] for CeyxPoolJobType.encode
  // jobs; null for every other job type.
  final List<Object?>? encodeArgs;
  int? requestId;

  // WP10: the slot pre-acquired on the pool's isolate for this job, or null on
  // the degraded route. Whoever clears this field owns returning it.
  CeyxNativeBuffer? slot;

  /// True once this job has been refused with [kMsgResize] one time. A second
  /// refusal fails the job instead of re-dispatching, which is the bound on
  /// the retry.
  bool resized = false;

  /// Set between a resize and the retry dispatch, so [_dispatch] reuses the
  /// existing request id rather than minting a new one.
  bool awaitingRetry = false;

  /// True once the pool has given up on the pooled route for THIS job and
  /// re-dispatched it with no slot (erratum E-WP10-1072). A further refusal
  /// after this point is a broken worker, not an awkward file, and fails the
  /// job — which is what makes the resize path terminate.
  bool pooledAbandoned = false;

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
/// Probe results cross the boundary as [TransferableTypedData] (small preview
/// JPEGs — the copy is negligible and the wire type is unchanged).
///
/// H2-A: DECODE results do not. They cross as a bare native address plus
/// dimensions ([DngDecoderService.decodeForPointerTransfer]); the worker
/// neither copies nor frees the RGBA buffer, and the pool takes ownership on
/// arrival. See the ownership section on [CeyxDecodePool] for the two arms and
/// the bounded-leak inventory. `DngDecoderService.decodeOnWorker` (the
/// `Isolate.run` A/B control) still uses the copy path unchanged.
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

  // WP3a: resolved once, at worker boot, exactly like `service` above — not
  // per encode call. `dlopen` is refcounted by the OS loader, so this hands
  // back the SAME image `service`'s bindings already mapped rather than
  // loading a second copy; it merely gives this worker its own
  // `DynamicLibrary` handle to look the encode symbols up from.
  final DynamicLibrary lib = libraryPath == null
      ? DngNativeBindings.load().library
      : DngNativeBindings.fromPath(libraryPath).library;

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
    // WP3a: optional trailing element, present only for encode jobs. Length
    // guard mirrors the R4 bootstrap-widening rule above.
    //
    // WP10 made this position NULLABLE as well as optional: a pooled decode
    // occupies index 5 with an explicit null so its dstAddress/dstCapacity keep
    // fixed indices 6/7 (see _dispatch). Hence the `?` — an unconditional cast
    // would throw on every pooled decode.
    final encodeArgs = message.length > 5
        ? message[5] as List<Object?>?
        : null;
    try {
      switch (type) {
        case CeyxPoolJobType.probe:
          final bytes = service.getPreviewJpeg(path);
          poolPort.send(<Object?>[
            kMsgResult,
            requestId,
            bytes == null ? null : TransferableTypedData.fromList([bytes]),
          ]);
        case CeyxPoolJobType.probeSize:
          // WP10: metadata only. Zero/zero means "not knowable here" (the
          // dylib predates this route's entry, or the probe failed) and the
          // pool caches that as "no pooled route" rather than re-asking.
          final size = service.probeOutputSize(path, maxDim: maxDim);
          poolPort.send(<Object?>[
            kMsgResult,
            requestId,
            size?.width ?? 0,
            size?.height ?? 0,
          ]);
        case CeyxPoolJobType.decode:
          // WP10: the address arrives in the message ONLY when the main
          // isolate pre-acquired a slot AND the dylib exports the entry for
          // this file's route. Both absent => byte-for-byte the pre-WP10 path.
          final dstAddress = message.length > 6 ? message[6] as int : 0;
          final dstCapacity = message.length > 7 ? message[7] as int : 0;
          if (dstAddress != 0 && service.decodeIntoBufferAvailable) {
            try {
              final image = service.decodeIntoPointer(
                path,
                dstAddress,
                dstCapacity,
                maxDim: maxDim,
              );
              poolPort.send(<Object?>[kMsgResult, requestId, ...image]);
            } on DngBufferTooSmallException catch (e) {
              // The prediction was stale. Report the extent native gave us so
              // the pool re-acquires exactly, rather than guessing again.
              poolPort.send(<Object?>[kMsgResize, requestId, e.width, e.height]);
            }
          } else {
            final image = service.decodeForPointerTransfer(path, maxDim: maxDim);
            poolPort.send(<Object?>[kMsgResult, requestId, ...image]);
          }
        case CeyxPoolJobType.encode:
          final result = _encodeOnPoolWorker(lib, encodeArgs!);
          poolPort.send(<Object?>[kMsgResult, requestId, result]);
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

/// WP3a: the encode job's worker-side implementation. `args` is
/// `[rgbaAddress, width, height, quality]`. Called on an ALREADY-RUNNING pool
/// worker (`lib` was resolved once at boot, see [ceyxDecodeWorkerMain]).
///
/// Deliberately mirrors `CeyxEncodeService._encodeOnWorker`
/// (`encode_service.dart:193-252`) EXCEPT for what is ABSENT: no `malloc` of
/// the input and no `setAll` copy, because the pixels already live in native
/// memory at `args[0]` — that copy (~97MB per encode) is the whole point of
/// this work package.
TransferableTypedData _encodeOnPoolWorker(
  DynamicLibrary lib,
  List<Object?> args,
) {
  final bindings = CeyxEncodeBindings.fromLibrary(lib);
  if (!bindings.available) {
    throw CeyxEncodeUnavailableException();
  }

  final rgbaPtr = Pointer<Uint8>.fromAddress(args[0] as int);
  final width = args[1] as int;
  final height = args[2] as int;
  final quality = args[3] as int;

  final outPtr = calloc<Pointer<Uint8>>();
  final outLenPtr = calloc<Size>();
  try {
    final result = bindings.jpeg(
      rgbaPtr,
      width,
      height,
      quality,
      outPtr,
      outLenPtr,
    );

    if (result != CeyxEncodeErrorCode.success) {
      // Contract: *out is NULL and *out_len is 0 on failure, so nothing to
      // free here.
      throw CeyxEncodeException(result, bindings.errorName(result));
    }

    final buffer = outPtr.value;
    final len = outLenPtr.value;
    if (buffer == nullptr || len == 0) {
      throw CeyxEncodeException(
        CeyxEncodeErrorCode.encodeFailed,
        bindings.errorName(CeyxEncodeErrorCode.encodeFailed),
      );
    }

    try {
      final encoded = Uint8List.fromList(buffer.asTypedList(len));
      return TransferableTypedData.fromList([encoded]);
    } finally {
      bindings.free(buffer);
    }
  } finally {
    calloc.free(outPtr);
    calloc.free(outLenPtr);
  }
}
