import 'dart:async';
import 'dart:collection';
import 'dart:ffi';

import 'package:ffi/ffi.dart' show malloc;
import 'package:meta/meta.dart';

import 'dng_bindings.dart';

/// A fixed-slot pool of native RGBA buffers.
///
/// Exists so a ~97MB full-res frame stops being reclaimed on the Dart GC's
/// schedule: a buffer returned by [release] is reusable IMMEDIATELY, and the
/// `NativeFinalizer`/`Finalizer` route is demoted to a SAFETY NET for the paths
/// that forget. [debugFinalizerReleases] above zero in a test is therefore a
/// defect signal, not normal operation.
///
/// ## Bound
///
/// [maxBuffers] is the hard cap on POOLED live buffers (checked out + idle) and
/// is fixed at construction: there is no growth path, because a growable
/// "fixed" pool is not a bound. Exhaustion WAITS ([acquire] returns a future
/// that completes when a buffer comes back); it never allocates past the cap.
/// A request larger than [maxBufferBytes] is served OUTSIDE the pool
/// (`pooled: false`), so one oversize frame can neither consume nor permanently
/// enlarge a slot; its [release] frees rather than returns.
///
/// ## Threading / isolates
///
/// This object is NOT shared across isolates. A native address is
/// process-global (isolates share the native heap) but this free list is a Dart
/// object, so it lives on ONE isolate — the main isolate, beside
/// `CeyxDecodePool`, which is where returns land.
class CeyxNativeBufferPool {
  CeyxNativeBufferPool({
    required this.maxBuffers,
    this.maxBufferBytes = _kDefaultMaxBufferBytes,
    int? idleFloor,
  }) : idleFloor = idleFloor ?? maxBuffers,
       assert(maxBuffers > 0, 'a pool with no slots is not a pool') {
    assert(
      this.idleFloor >= 0 && this.idleFloor <= maxBuffers,
      'idleFloor must be within [0, maxBuffers]',
    );
    _instances.add(WeakReference<CeyxNativeBufferPool>(this));
  }

  /// Every pool constructed on this isolate, held WEAKLY so a test pool that
  /// goes out of scope stops contributing. A registry that retained everything
  /// would turn [debugTotalLiveAddresses] into a monotonically rising number
  /// nobody could assert on — a gauge that has silently stopped working.
  static final List<WeakReference<CeyxNativeBufferPool>> _instances =
      <WeakReference<CeyxNativeBufferPool>>[];

  /// 256MB. Comfortably above a 108MP RGBA frame (~432MB is beyond it — such a
  /// frame is served unpooled on purpose rather than permanently inflating a
  /// slot).
  static const int _kDefaultMaxBufferBytes = 256 * 1024 * 1024;

  /// Hard cap on live POOLED buffers (checked out + idle).
  final int maxBuffers;

  /// Requests above this are served outside the pool.
  final int maxBufferBytes;

  /// How many pooled buffers [shrinkToFloor] leaves behind. Defaults to
  /// [maxBuffers], i.e. "never shrink" — every pre-existing construction of
  /// this class keeps its old behaviour, and only the constructions that opt
  /// in (the [shared] pool) can lose buffers to the idle shrink.
  final int idleFloor;

  /// Process-wide instance. Sized to sit ABOVE the host's byte budget, so
  /// exhaustion is a backstop rather than the operating point:
  /// [debugWaitsForCapacity] > 0 in production means the two bounds disagree.
  ///
  /// `idleFloor: 2` is the idle-shrink campaign's user ruling: after 5s of
  /// decode quiescence the 8 slots collapse to 2 (~192MB instead of ~771MB)
  /// and demand regrows them for free through [acquireOrNull]'s
  /// `_live < maxBuffers` branch.
  static final CeyxNativeBufferPool shared = CeyxNativeBufferPool(
    maxBuffers: 8,
    idleFloor: 2,
  );

  /// Test seam: when set, every free this pool would perform routes here
  /// instead of into `malloc.free`/`ceyx_pool_aligned_free`, and every
  /// allocation is still real (so addresses are genuine). Mirrors
  /// `CeyxDecodePool.debugNativeFree`.
  @visibleForTesting
  static void Function(int address)? debugFreeHook;

  /// Test seam for the wall clock the hysteresis timestamps are stamped with.
  /// A test drives it forward explicitly rather than sleeping, so the 5s/1s
  /// windows are asserted as logic, not as elapsed real time.
  @visibleForTesting
  static DateTime Function() debugClock = DateTime.now;

  /// Test seam standing in for the native `ceyx_pool_pressure_relief` symbol
  /// (macOS `malloc_zone_pressure_relief`) until the FFI binding lands. When
  /// null, the binding is consulted; when the binding is also absent (a plain
  /// Dart test process with no dylib) the relief is simply skipped — the
  /// frees still happened, only the eager page return did not.
  @visibleForTesting
  static int Function()? debugPressureReliefOverride;

  // --- R4 (gpu-copy-elimination campaign): page-aligned pooled allocations --
  //
  // The C2 zero-copy wrap only engages when the caller's destination pointer
  // AND capacity are BOTH multiples of `_kAlignmentBytes` (the alignment
  // probe in `ceyxDecodeIntoPrepare`, native/src/ffi/ceyx_decode_into_ffi.cpp,
  // and the constant the C1 arena asserts on its own allocations,
  // `kRawDeviceArenaAlignmentBytes`). `package:ffi`'s `malloc` has no aligned
  // form, so POOLED allocations go through the native `ceyx_pool_aligned_alloc`
  // / `ceyx_pool_aligned_free` pair (native/include/ceyx_decode_into.h) when
  // the loaded dylib exports it, rounding the requested size UP to a
  // `_kAlignmentBytes` multiple so the capacity half of the contract holds
  // too. A dylib predating this pair falls back to ordinary `malloc` — the
  // decode is still correct, it just never satisfies the alignment probe.
  //
  // ONLY pooled allocations take this path. Unpooled/oversize buffers
  // (`acquireOrNull`'s `bytes > maxBufferBytes` branch) and adopted foreign
  // pointers (`adoptUnpooled`) are UNCHANGED: an unaligned adopted pointer is
  // legal input to the probe (it just answers false), and there is no
  // "aligned adopt" concept to introduce.

  /// Apple Silicon page size; matches `kRawDeviceArenaAlignmentBytes`
  /// (native/include/raw_persistent_device_arena.h) and the alignment probe
  /// constant — one physical number, checked by three call sites.
  static const int _kAlignmentBytes = 16384;

  static int _roundUpToAlignment(int bytes) {
    final remainder = bytes % _kAlignmentBytes;
    return remainder == 0 ? bytes : bytes + (_kAlignmentBytes - remainder);
  }

  /// Test-only override for the whole native-bindings resolution below: a
  /// unit test that wants to exercise the aligned path against a FRESH build
  /// (rather than whatever dylib the default search order finds) sets this
  /// before constructing a pool. Mirrors the pattern
  /// `wp10_activation_proof_test.dart` uses for `DngDecoderService`.
  @visibleForTesting
  static DngNativeBindings? debugNativeBindingsOverride;

  static DngNativeBindings? _nativeBindings;
  static bool _nativeBindingsAttempted = false;

  /// Resolves (once) the dylib's aligned allocator pair, if it exports one.
  /// A failure to load the library at all (no dylib on this host, or a plain
  /// Dart unit-test process with no native library present) is NOT an error
  /// here — it just means every pooled allocation takes the `malloc`
  /// fallback, exactly like a dylib that predates the pair.
  static DngNativeBindings? _resolveNativeBindings() {
    final override = debugNativeBindingsOverride;
    if (override != null) return override;
    if (_nativeBindingsAttempted) return _nativeBindings;
    _nativeBindingsAttempted = true;
    try {
      _nativeBindings = DngNativeBindings.load();
    } catch (_) {
      _nativeBindings = null;
    }
    return _nativeBindings;
  }

  /// Test-only: forces the next [_resolveNativeBindings] call to re-attempt
  /// resolution rather than reuse a cached (possibly null) result. Needed
  /// because [_nativeBindingsAttempted] is a process-wide latch and a test
  /// that sets [debugNativeBindingsOverride] to null wants a clean slate for
  /// the NEXT test rather than inheriting whatever the first resolution
  /// attempt in this process found.
  @visibleForTesting
  static void debugResetNativeBindingsCache() {
    _nativeBindings = null;
    _nativeBindingsAttempted = false;
  }

  /// Allocates [bytes] (already rounded to [_kAlignmentBytes]) through the
  /// native aligned allocator when available, else falls back to `malloc`.
  /// Returns the raw address AND whether the aligned allocator produced it,
  /// so the matching free routes to the right allocator family
  /// ([_freeAlignedOrMalloc]) even if native-bindings resolution later
  /// changes (it does not today, but the flag makes that safe regardless).
  static (int address, bool wasAligned) _allocateAlignedAddress(int bytes) {
    final bindings = _resolveNativeBindings();
    final alloc = bindings?.ceyxPoolAlignedAlloc;
    if (alloc != null) {
      final ptr = alloc(bytes);
      if (ptr != nullptr) return (ptr.address, true);
      // Aligned allocation failed (OOM or a bad size) — fall through to the
      // ordinary allocator rather than failing the whole pool.
    }
    return (malloc<Uint8>(bytes).address, false);
  }

  /// Frees an address obtained from [_allocateAlignedAddress], routing to
  /// whichever allocator actually produced it (mismatched allocator/free is
  /// the exact failure mode this pair is designed to avoid).
  static void _freeAlignedOrMalloc(int address, {required bool wasAligned}) {
    if (wasAligned) {
      final free = _resolveNativeBindings()?.ceyxPoolAlignedFree;
      if (free != null) {
        free(Pointer<Uint8>.fromAddress(address));
        return;
      }
      // This memory came from ceyx_pool_aligned_alloc (which is _aligned_malloc
      // on Windows). Falling back to malloc.free here would be undefined
      // behaviour on Windows and corrupt the CRT heap — never mix allocator
      // families for aligned memory. Failing to resolve the paired free
      // symbol means the native bindings are in an inconsistent state, so
      // fail loudly rather than silently freeing with the wrong allocator.
      throw StateError(
        'ceyx_pool_aligned_free is unavailable but address $address was '
        'allocated via ceyx_pool_aligned_alloc; refusing to free it with '
        'malloc.free (undefined behaviour on Windows _aligned_malloc memory).',
      );
    }
    malloc.free(Pointer<Uint8>.fromAddress(address));
  }

  final List<CeyxNativeBuffer> _idle = <CeyxNativeBuffer>[];
  final Queue<_Waiter> _waiting = Queue<_Waiter>();

  /// Pooled buffers that exist right now (checked out + idle). Never exceeds
  /// [maxBuffers].
  int _live = 0;

  /// Every pooled address this pool currently owns, so a bare address arriving
  /// from elsewhere (the decode pool's free path) can be recognised as
  /// pool-owned instead of being freed behind the pool's back.
  final Map<int, CeyxNativeBuffer> _byAddress = <int, CeyxNativeBuffer>{};

  int _checkedOut = 0;

  /// Buffers currently checked out. Assignment is funnelled through this
  /// setter so [onCheckoutChange] cannot be forgotten at one of the eight
  /// sites that move it — a quiescence observer that misses one return is a
  /// pool that never shrinks (or, worse, shrinks while borrowed).
  @visibleForTesting
  int get debugCheckedOut => _checkedOut;

  @visibleForTesting
  set debugCheckedOut(int value) {
    if (value == _checkedOut) return;
    _checkedOut = value;
    onCheckoutChange?.call();
  }

  /// Round-1 review F1: how many times an explicit release disarmed a safety
  /// net. Asserted by the decode-side suites so "the release detaches" is a
  /// mechanical fact rather than a claim — GC cannot be forced, but the detach
  /// call can be counted.
  @visibleForTesting
  static int debugSafetyNetDetaches = 0;

  /// Called by the two attach sites when they detach on explicit release.
  static void noteSafetyNetDetach() => debugSafetyNetDetaches++;

  @visibleForTesting
  int debugExplicitReleases = 0;

  /// MUST stay 0 in tests. Counts ONLY this pool's own safety-net reclaim
  /// (`decode_pool.dart`'s attach site) — never any unrelated finalizer.
  @visibleForTesting
  int debugFinalizerReleases = 0;

  @visibleForTesting
  int debugWaitsForCapacity = 0;

  /// Bounded by [maxBuffers] for pooled buffers; unpooled oversize allocations
  /// are counted separately by [debugUnpooledAllocations].
  @visibleForTesting
  int debugAllocations = 0;

  @visibleForTesting
  int debugUnpooledAllocations = 0;

  /// Test-visible count of foreign addresses taken over by [adoptUnpooled].
  /// Separate from [debugUnpooledAllocations] on purpose: that one means "this
  /// photo was larger than a slot", this one means "a degradation path fired".
  @visibleForTesting
  int debugAdoptions = 0;

  /// Addresses this pool currently considers checked out — pooled, unpooled
  /// oversize and adopted alike. The Dart-side replacement for the native
  /// native pool's checked-out-count gauge, at the same strength.
  ///
  /// DERIVED from [_byAddress], never maintained as a parallel set: two
  /// structures that must agree eventually disagree, and the disagreement
  /// would be invisible precisely when the gauge matters.
  @visibleForTesting
  Set<int> get debugLiveAddresses => _byAddress.entries
      .where((e) => !e.value.released)
      .map((e) => e.key)
      .toSet();

  /// The summed CAPACITY of everything this pool currently has checked out.
  ///
  /// Exists for the host's memory-attribution ledger (S3.0), which has to state
  /// the pool's contribution in BYTES and cannot reach the per-buffer sizes
  /// from outside this class: [debugLiveAddresses] gives addresses only, and
  /// multiplying a count by a nominal frame size would report a guess as a
  /// measurement. Read-only and DERIVED from [_byAddress], for the same reason
  /// that getter is.
  ///
  /// Capacity, not decoded extent: a slot is charged at the size it occupies,
  /// which is what a memory ledger is asking about.
  @visibleForTesting
  int get debugLiveBufferByteTotal => _byAddress.values
      .where((buffer) => !buffer.released)
      .fold<int>(0, (total, buffer) => total + buffer.capacity);

  /// Live checkouts across EVERY pool on this isolate, including [shared] and
  /// any test-constructed pool. This is the half of the native gauge that
  /// per-buffer identity accounting cannot express: "is anything, anywhere,
  /// still checked out right now?" A test that ends non-zero has found a leak
  /// in exactly the sense the native gauge meant.
  ///
  /// Counts CHECKED-OUT buffers, not allocated ones: an idle pooled buffer is
  /// reuse, not a leak.
  ///
  /// Not test-only: `app/bin/benchmark_raw_zero_copy.dart` reads this as its
  /// production pool-leak gauge (the replacement for the deleted native
  /// `poolCheckedOut` counter), so it is intentionally NOT
  /// `@visibleForTesting` even though every other `debug*` member on this
  /// class is. Debug/diagnostics API, not a test seam.
  static int get debugTotalLiveAddresses {
    var total = 0;
    // Each target is read ONCE into a local. A previous revision pruned dead
    // references first and then dereferenced with `!`, which is a
    // time-of-check/time-of-use race: the collector can take a target between
    // the prune and the read, and the `!` then throws. It survived a local run
    // and failed an independent one — GC timing is not part of the contract,
    // so the code must not depend on it.
    _instances.removeWhere((WeakReference<CeyxNativeBufferPool> ref) {
      final pool = ref.target;
      if (pool == null) return true; // dead: prune as hygiene, not correctness
      total += pool.debugLiveAddresses.length;
      return false;
    });
    return total;
  }

  /// Shrink batches that actually freed something. A batch that found nothing
  /// above the floor, or that refused because the pool was busy, does NOT
  /// count — otherwise "the shrink fired" could not be distinguished from
  /// "the shrink ran and did nothing".
  @visibleForTesting
  int debugShrinkEvents = 0;

  @visibleForTesting
  int debugBuffersFreedByShrink = 0;

  /// Times the pressure-relief call actually reached a function (override or
  /// binding). MUST be at most one per shrink batch.
  @visibleForTesting
  int debugPressureReliefCalls = 0;

  /// Bytes the native relief reported reclaiming on its last call, or
  /// [kCeyxPressureReliefUnsupported] (-1) on a platform/build without it.
  /// Null before the first call that reached a function. A real 0 ("nothing
  /// cached") stays distinguishable from -1 on purpose.
  @visibleForTesting
  int? debugLastPressureReliefResult;

  /// Shrink batches that freed memory but found no relief symbol to call.
  /// Non-zero on a release build means the dylib predates the symbol.
  @visibleForTesting
  int debugPressureReliefSkips = 0;

  /// Shrink batches that refused because the pool was not idle
  /// (`_waiting` non-empty or something still checked out).
  @visibleForTesting
  int debugShrinkRefusals = 0;

  /// True while ANY buffer of this pool is checked out. The quiescence
  /// predicate's buffer-side half: a borrowed buffer may still be written by a
  /// worker isolate, so "no decode running" is not enough on its own.
  ///
  /// DERIVED from [_byAddress] (the same source as [debugLiveAddresses]),
  /// deliberately NOT from the [debugCheckedOut] counter: two structures that
  /// must agree eventually disagree, and this one gates freeing memory
  /// somebody may be reading.
  ///
  /// RULING (agreed with the quiescence owner): adopted and oversize-unpooled
  /// buffers COUNT as outstanding — no `pooled` filter here. Both register in
  /// [_byAddress], both are live RGBA a `DngImage` is still reading, and an
  /// in-flight degradation path is not "the app has stopped decoding".
  /// [shrinkToFloor] would never free them anyway (it walks `_idle` only), but
  /// quiescence is a broader signal than the shrink precondition, and the only
  /// cost of being strict is that a shrink waits.
  bool get hasOutstandingCheckouts =>
      _byAddress.values.any((CeyxNativeBuffer b) => !b.released);

  /// True while an acquirer is blocked waiting for capacity.
  bool get hasWaiters => _waiting.isNotEmpty;

  /// Notified whenever [debugCheckedOut] changes, so a quiescence observer
  /// sees returns it cannot otherwise observe (a `DngImage.releaseToPool()`
  /// never goes through the decode pool). Set by
  /// `CeyxDecodePool`'s quiescence watch; null when nobody is watching.
  void Function()? onCheckoutChange;

  /// Notified when [shrinkToFloor] COMPLETES a batch that actually freed
  /// buffers, with the number of buffers freed.
  ///
  /// Fires exactly once per such batch, as the last thing the shrink does:
  /// after every buffer has been freed and after the native pressure relief
  /// ran. It never fires on the refusal path (waiters or outstanding
  /// checkouts), nor on an already-at-the-floor call that freed nothing — so
  /// a listener may treat every call as "pages just came back".
  ///
  /// Unlike [onCheckoutChange], no ceyx code ever assigns this: the host
  /// application is its single writer (Halcyon couples its Windows
  /// working-set trim to it). Called plainly, so the callback MUST NOT throw.
  void Function(int freedBuffers)? onShrink;

  DateTime? _lastGrowAt;
  DateTime? _lastShrinkAt;

  /// When this pool last allocated a pooled buffer. The hysteresis rule
  /// "after a grow, no shrink until a fresh quiet window" is enforced against
  /// this by [CeyxPoolShrinkPolicy]; the pool only records the fact, it does
  /// not hold policy (mirrors the mechanism/policy split `MemoryPressureTarget`
  /// uses on the Halcyon side).
  DateTime? get lastGrowAt => _lastGrowAt;

  /// When [shrinkToFloor] last freed something.
  DateTime? get lastShrinkAt => _lastShrinkAt;

  /// The size [warmUpFor] last pre-committed, or null when the warm state has
  /// been invalidated (including by a shrink). Test-visible so "the shrink
  /// invalidated the warm" is a mechanical assertion.
  @visibleForTesting
  int? get debugWarmedBytes => _warmedBytes;

  @visibleForTesting
  int get debugIdleCount => _idle.length;

  /// Live POOLED buffers (checked out + idle). Test-visible so a shrink's
  /// effect on the bound is assertable without reaching into privates.
  @visibleForTesting
  int get debugLiveBuffers => _live;

  @visibleForTesting
  int get debugWaiterCount => _waiting.length;

  /// The size last pre-committed by [warmUpFor], so a repeat call at the
  /// same size is a no-op. `null` before the first successful warm.
  int? _warmedBytes;

  /// Pre-commits the pages of one pooled buffer at [bytes], so the first real
  /// decode does not pay the ~96MB first-touch page-fault. Replaces the
  /// native warmup's step-2 pool touch (`warmPipelinePoolsForSize`), deleted
  /// with the native pools in WP5. Idempotent per size: a second call at the
  /// same size is a no-op, because the pages are already committed.
  ///
  /// Fills the WHOLE buffer rather than touching one byte per page: page size
  /// differs across the two platforms this pool runs on (Apple Silicon uses
  /// 16KiB pages; Android ARM64 commonly uses 4KiB, but is not guaranteed
  /// to), so a single stride assumption would silently under-commit on
  /// whichever platform guessed wrong while still reporting success. A full
  /// fill is stride-independent and correct on both.
  ///
  /// Never blocks a real decode: if [acquire] would have to wait (pool at
  /// cap, nothing idle), the warm is skipped outright rather than queued
  /// behind a real request — warmup is an optimisation, and an optimisation
  /// that delays the thing it optimises has inverted its own purpose.
  Future<void> warmUpFor(int bytes) async {
    if (bytes <= 0 || bytes == _warmedBytes) return;
    final buffer = acquireOrNull(bytes);
    if (buffer == null) return;
    final ptr = Pointer<Uint8>.fromAddress(buffer.address);
    ptr.asTypedList(buffer.capacity).fillRange(0, buffer.capacity, 0);
    release(buffer);
    _warmedBytes = bytes;
  }

  /// Checks out a buffer of at least [bytes]. Completes immediately when a slot
  /// is free; otherwise waits for a return — never allocates past [maxBuffers].
  Future<CeyxNativeBuffer> acquire(int bytes) {
    final immediate = acquireOrNull(bytes);
    if (immediate != null) return Future<CeyxNativeBuffer>.value(immediate);

    debugWaitsForCapacity++;
    final waiter = _Waiter(bytes);
    _waiting.add(waiter);
    return waiter.completer.future;
  }

  /// The synchronous half of [acquire]: everything servable WITHOUT waiting.
  /// Null means "at the cap with nothing idle to re-size" — the one case that
  /// needs a future.
  ///
  /// WP2: exists because `DngDecoderService.decode` is synchronous by public
  /// contract (R-C keeps every public signature) and a synchronous caller
  /// cannot await a slot. Its null answer is served there by malloc +
  /// [adoptUnpooled], so the bound still counts slots and the address is still
  /// pool-owned.
  CeyxNativeBuffer? acquireOrNull(int bytes) {
    assert(bytes > 0);
    if (bytes > maxBufferBytes) {
      // Outside the pool entirely: takes no slot, is freed on release.
      debugUnpooledAllocations++;
      debugCheckedOut++;
      final buffer = CeyxNativeBuffer._(
        malloc<Uint8>(bytes).address,
        bytes,
        false,
      );
      // WP2: an unpooled buffer is still THIS pool's to reclaim. Without this
      // registration `ownsAddress` answered false, `tryReleaseByAddress`
      // refused it, and the decode pool handed a malloc'd address to the
      // dylib's own free. `_returnToFreeList` already routes `!pooled` to
      // `_disposeBuffer`, and `_disposeBuffer` removes the entry, so this is
      // the only line that was missing.
      buffer._checkoutGeneration++;
      _byAddress[buffer.address] = buffer;
      return buffer;
    }

    final reused = _takeIdleFitting(bytes);
    if (reused != null) {
      debugCheckedOut++;
      reused._checkoutGeneration++;
      return reused;
    }

    if (_live < maxBuffers) {
      return _allocatePooled(bytes);
    }

    // At the cap with nothing that fits. An idle-but-too-small buffer can be
    // re-sized in place of a NEW slot (free one, allocate one) — the live count
    // is unchanged, so the bound holds.
    if (_idle.isNotEmpty) {
      final victim = _idle.removeAt(0);
      _disposeBuffer(victim);
      _live--;
      _byAddress.remove(victim.address);
      return _allocatePooled(bytes);
    }

    return null;
  }

  /// Takes ownership of an address this pool did NOT allocate, as an UNPOOLED
  /// buffer: it occupies no slot, counts against no bound, and its release
  /// frees rather than returns.
  ///
  /// Used by the decode pool's self-allocating adoption sink, where the worker
  /// isolate had to allocate (it cannot reach this Dart object) and the pool
  /// must still be the single owner of every live RGBA address.
  CeyxNativeBuffer adoptUnpooled(int address, int bytes) {
    assert(address != 0, 'cannot adopt the null address');
    assert(bytes > 0);
    assert(
      !_byAddress.containsKey(address),
      'address 0x${address.toRadixString(16)} is already owned by this pool',
    );
    debugAdoptions++;
    debugCheckedOut++;
    final buffer = CeyxNativeBuffer._(address, bytes, false);
    buffer._checkoutGeneration++;
    _byAddress[address] = buffer;
    return buffer;
  }

  /// Returns [buffer] for reuse. Idempotent: a second return is a no-op, not a
  /// double free — the safety net may fire on a buffer that was already
  /// released explicitly.
  void release(CeyxNativeBuffer buffer) {
    if (buffer.released) return;
    buffer._released = true;
    debugExplicitReleases++;
    debugCheckedOut--;
    _returnToFreeList(buffer);
  }

  /// Safety-net reclaim, called from the finalizer attach site in
  /// `decode_pool.dart` when a pooled buffer was garbage-collected without an
  /// explicit [release]. Counted separately so a test can assert it never
  /// happened. Returns true if [address] was pool-owned.
  /// Safety-net reclaim keyed on the BUFFER and the checkout it was armed for.
  bool releaseFromFinalizer(CeyxNativeBuffer buffer, int generation) {
    if (buffer.released) return false;
    // Round-1 review F1: the buffer may have been released explicitly and then
    // handed out AGAIN before this safety net was collected. The pool reuses
    // instances, so identity says "same buffer" in both checkouts and cannot
    // tell them apart; the generation can. Acting here would reclaim a buffer
    // its new owner is still reading.
    if (buffer.checkoutGeneration != generation) return false;
    if (!identical(_byAddress[buffer.address], buffer)) return false;
    debugFinalizerReleases++;
    debugCheckedOut--;
    buffer._released = true;
    _returnToFreeList(buffer);
    return true;
  }

  /// Looks up the live buffer for [address], or null when this pool does not
  /// own it. Lets an attach site capture the buffer identity behind an address.
  CeyxNativeBuffer? bufferFor(int address) => _byAddress[address];

  bool releaseByAddressFromFinalizer(int address) {
    final buffer = _byAddress[address];
    if (buffer == null || buffer.released) return false;
    buffer._released = true;
    debugFinalizerReleases++;
    debugCheckedOut--;
    _returnToFreeList(buffer);
    return true;
  }

  /// Explicit release given only an address (the decode pool's free path holds
  /// addresses, not [CeyxNativeBuffer]s). Returns false when the address is not
  /// pool-owned, in which case the caller keeps its existing free behaviour.
  bool tryReleaseByAddress(int address) {
    final buffer = _byAddress[address];
    if (buffer == null) return false;
    release(buffer);
    return true;
  }

  /// True when [address] is a live pooled buffer of this pool.
  bool ownsAddress(int address) => _byAddress.containsKey(address);

  void _returnToFreeList(CeyxNativeBuffer buffer) {
    if (!buffer.pooled) {
      _disposeBuffer(buffer);
      return;
    }
    // Hand straight to a waiter when one fits, so a returned buffer never sits
    // idle while a caller waits for capacity.
    for (final waiter in _waiting.toList()) {
      if (buffer.capacity >= waiter.bytes) {
        _waiting.remove(waiter);
        buffer._released = false;
        debugCheckedOut++;
        buffer._checkoutGeneration++;
        waiter.completer.complete(buffer);
        return;
      }
    }
    _idle.add(buffer);
    _serviceWaitersByResizing();
  }

  /// A waiter whose request no idle buffer can satisfy is served by re-sizing
  /// an idle slot (free + allocate). Without this the waiter would hang while
  /// capacity sat idle in the wrong size — a hang is explicitly not an
  /// acceptable degradation mode.
  void _serviceWaitersByResizing() {
    while (_waiting.isNotEmpty && _idle.isNotEmpty) {
      final waiter = _waiting.first;
      final fitting = _takeIdleFitting(waiter.bytes);
      if (fitting != null) {
        _waiting.removeFirst();
        debugCheckedOut++;
        fitting._checkoutGeneration++;
        waiter.completer.complete(fitting);
        continue;
      }
      final victim = _idle.removeAt(0);
      _disposeBuffer(victim);
      _live--;
      _byAddress.remove(victim.address);
      _waiting.removeFirst();
      waiter.completer.complete(_allocatePooled(waiter.bytes));
    }
  }

  CeyxNativeBuffer? _takeIdleFitting(int bytes) {
    for (var i = 0; i < _idle.length; i++) {
      if (_idle[i].capacity >= bytes) {
        final buffer = _idle.removeAt(i);
        buffer._released = false;
        return buffer;
      }
    }
    return null;
  }

  CeyxNativeBuffer _allocatePooled(int bytes) {
    // Hysteresis input: any pooled allocation counts as a grow, including the
    // resize-a-victim path (free one, allocate one). That path leaves `_live`
    // unchanged, so calling it a grow is conservative — it delays a shrink,
    // it never permits an early one.
    _lastGrowAt = debugClock();
    _live++;
    debugAllocations++;
    debugCheckedOut++;
    // ponytail: one allocation per POOL SLOT for the process lifetime, not one
    // per decode. Bucketing beyond "capacity >= bytes" is not justified until
    // a mixed-resolution folder is measured to thrash it.
    //
    // R4: rounded UP to the page-alignment contract (both halves — pointer
    // AND capacity) so the C2 zero-copy wrap's alignment probe can engage on
    // this buffer. See the block comment above [_kAlignmentBytes].
    final rounded = _roundUpToAlignment(bytes);
    final (address, wasAligned) = _allocateAlignedAddress(rounded);
    final buffer = CeyxNativeBuffer._(
      address,
      rounded,
      true,
      alignedAllocated: wasAligned,
    );
    buffer._checkoutGeneration++;
    _byAddress[buffer.address] = buffer;
    return buffer;
  }

  void _disposeBuffer(CeyxNativeBuffer buffer) {
    _byAddress.remove(buffer.address);
    final hook = debugFreeHook;
    if (hook != null) {
      hook(buffer.address);
      return;
    }
    _freeAlignedOrMalloc(buffer.address, wasAligned: buffer.alignedAllocated);
  }

  /// Releases idle pooled buffers until only [idleFloor] live pooled buffers
  /// remain, and returns how many were freed.
  ///
  /// The production sibling of [debugDisposeIdle]: same traversal, same
  /// refusal to touch anything checked out, but it stops at the floor and it
  /// is safe to call on a live pool.
  ///
  /// REFUSES (frees nothing, returns 0) whenever the pool is not fully idle —
  /// a queued waiter means someone is blocked on capacity this would destroy,
  /// and a non-zero checkout means a decode may still be WRITING into a buffer
  /// (workers run on other isolates and cannot see this free list, so
  /// "it is on `_idle`" is the only safe predicate, and it is only safe when
  /// nothing at all is outstanding).
  ///
  /// On a batch that freed something: invalidates the [warmUpFor] memo (the
  /// warmed pages are gone with the buffer, so the next warm must really
  /// re-commit) and calls the native pressure relief EXACTLY ONCE — plain
  /// `free()` alone leaves an unpredictable reusable residue instead of
  /// returning the pages (see the campaign's free-probe verdict).
  int shrinkToFloor() {
    // Reads the PRODUCTION predicates, not the `@visibleForTesting` counters:
    // `hasOutstandingCheckouts` is derived from `_byAddress`, which is the
    // authoritative ownership record, and the same two getters are what
    // `CeyxDecodePool.isQuiescent` consults — one definition of "busy", not
    // two that can drift.
    if (hasWaiters || hasOutstandingCheckouts) {
      debugShrinkRefusals++;
      return 0;
    }
    var freed = 0;
    while (_live > idleFloor && _idle.isNotEmpty) {
      final victim = _idle.removeAt(0);
      // Routed through _disposeBuffer so `_byAddress` stays the single owner
      // and the finalizer generation guard's premise holds if this address is
      // handed back by a later allocation.
      _disposeBuffer(victim);
      _live--;
      freed++;
    }
    if (freed == 0) return 0;
    _warmedBytes = null;
    _lastShrinkAt = debugClock();
    debugShrinkEvents++;
    debugBuffersFreedByShrink += freed;
    _pressureRelief();
    // LAST statement by design: a listener must observe a completed shrink,
    // pages already returned. Reached only past the `freed == 0` early exit
    // and never on the refusal path above.
    onShrink?.call(freed);
    return freed;
  }

  /// Deliberately does NOT fire [onCheckoutChange]: a shrink runs only when
  /// nothing is checked out and frees only idle buffers, so the checked-out
  /// set it reports is provably unchanged across the call. Firing anyway would
  /// publish a quiescence "transition" that did not happen.
  void _pressureRelief() {
    final fn =
        debugPressureReliefOverride ??
        _resolveNativeBindings()?.ceyxPoolPressureRelief;
    if (fn == null) {
      // Silent in release BY DESIGN (an older dylib simply lacks the symbol;
      // the frees still happened, only the eager page return did not), but a
      // debug build says so — otherwise a shrink that returns far less RSS
      // than expected looks identical to one that worked.
      debugPressureReliefSkips++;
      assert(() {
        // ignore: avoid_print
        print(
          'CeyxNativeBufferPool: ceyx_pool_pressure_relief unavailable; '
          'shrink freed memory but did not request an eager page return.',
        );
        return true;
      }());
      return;
    }
    debugLastPressureReliefResult = fn();
    debugPressureReliefCalls++;
  }

  /// Test-only: frees every idle buffer so a unit test leaves no native
  /// allocation behind. Checked-out buffers are NOT touched — freeing one would
  /// be the very use-after-free this pool exists to prevent.
  @visibleForTesting
  void debugDisposeIdle() {
    for (final buffer in _idle.toList()) {
      _disposeBuffer(buffer);
      _live--;
    }
    _idle.clear();
  }
}

/// Creates the timer a [CeyxPoolShrinkPolicy] waits on. Injectable so a test
/// drives the quiet window explicitly instead of sleeping through it.
typedef PoolShrinkTimerFactory =
    Timer Function(Duration duration, void Function() callback);

/// Turns "the decoder has been quiet for a while" into
/// [CeyxNativeBufferPool.shrinkToFloor].
///
/// Policy lives HERE, not in the pool: the pool knows how to free a buffer and
/// when that is unsafe; it does not know what "idle" means. The quiescence
/// signal is pushed in by whoever owns the decode queue (`CeyxDecodePool`),
/// because a Halcyon-side queue-depth proxy cannot see a worker isolate that is
/// still writing into a borrowed buffer — a false "quiescent" there is the
/// frame-corruption path.
class CeyxPoolShrinkPolicy {
  CeyxPoolShrinkPolicy(
    this.pool, {
    this.quietWindow = kPoolShrinkQuietWindow,
    this.growLockout = kPoolShrinkGrowLockout,
    DateTime Function()? clock,
    PoolShrinkTimerFactory? timerFactory,
    bool Function()? isQuiescentNow,
  }) : _clock = clock ?? (() => CeyxNativeBufferPool.debugClock()),
       _timerFactory = timerFactory ?? Timer.new,
       _isQuiescentNow = isQuiescentNow;

  final CeyxNativeBufferPool pool;

  /// How long quiescence must hold CONTINUOUSLY before a shrink fires.
  final Duration quietWindow;

  /// Minimum distance from the last grow before a pool that has ALREADY
  /// shrunk once may shrink again. Guards the pathological oscillation where
  /// demand regrows the pool and a stale quiet window immediately tears it
  /// back down.
  final Duration growLockout;

  final DateTime Function() _clock;
  final PoolShrinkTimerFactory _timerFactory;
  final bool Function()? _isQuiescentNow;

  bool _quiescent = false;
  Timer? _timer;

  @visibleForTesting
  bool get debugArmed => _timer != null;

  /// Level-triggered sink for the decode-side quiescence signal. Repeated
  /// identical values are ignored, so the producer may re-assert freely; a
  /// transition to false cancels any pending shrink outright (the window must
  /// be CONTINUOUS, not cumulative).
  void onQuiescenceChanged(bool quiescent) {
    if (quiescent == _quiescent) return;
    _quiescent = quiescent;
    if (quiescent) {
      _arm(quietWindow);
    } else {
      _cancel();
    }
  }

  void _arm(Duration duration) {
    _timer?.cancel();
    _timer = _timerFactory(duration, _onWindowElapsed);
  }

  void _cancel() {
    _timer?.cancel();
    _timer = null;
  }

  void _onWindowElapsed() {
    _timer = null;
    if (!_quiescent) return;
    // Re-confirm synchronously at fire time: a missed "false" edge would
    // otherwise let a shrink run against a live decode.
    if (_isQuiescentNow?.call() == false) return;

    final grewAt = pool.lastGrowAt;
    if (grewAt != null) {
      final sinceGrow = _clock().difference(grewAt);
      // After a grow, the quiet window restarts from the grow.
      if (sinceGrow < quietWindow) {
        _arm(quietWindow - sinceGrow);
        return;
      }
      // After a shrink, no re-shrink within `growLockout` of a grow.
      if (pool.lastShrinkAt != null && sinceGrow < growLockout) {
        _arm(growLockout - sinceGrow);
        return;
      }
    }
    pool.shrinkToFloor();
  }

  void dispose() => _cancel();
}

/// Contract constant: 5s of continuous decode quiescence before a shrink.
const Duration kPoolShrinkQuietWindow = Duration(seconds: 5);

/// Contract constant: once shrunk, no re-shrink within 1s of a grow.
const Duration kPoolShrinkGrowLockout = Duration(seconds: 1);

/// One native allocation handed out by [CeyxNativeBufferPool].
class CeyxNativeBuffer {
  CeyxNativeBuffer._(
    this.address,
    this.capacity,
    this.pooled, {
    this.alignedAllocated = false,
  });

  /// Native address of the first byte. Process-global.
  final int address;

  /// Bytes allocated. May exceed the requested size when a larger idle buffer
  /// was reused.
  final int capacity;

  /// False for an oversize buffer served outside the pool: its release frees
  /// rather than returns.
  final bool pooled;

  /// R4: true when this buffer came from `ceyx_pool_aligned_alloc` rather
  /// than `malloc` — determines which allocator [CeyxNativeBufferPool]
  /// routes the matching free through. Always false for unpooled/adopted
  /// buffers, which never take the aligned path.
  final bool alignedAllocated;

  bool _released = false;

  int _checkoutGeneration = 0;

  /// Increments on every checkout. The pool REUSES buffer instances
  /// (`_takeIdleFitting` hands the same object back), so object identity cannot
  /// distinguish one checkout from the next — this can. A safety net captured
  /// during checkout N must not act during checkout N+1.
  int get checkoutGeneration => _checkoutGeneration;

  /// True while this buffer is on the free list (or has been freed). Reading
  /// the memory of a released buffer is a use-after-free.
  bool get released => _released;

  @override
  String toString() =>
      'CeyxNativeBuffer(0x${address.toRadixString(16)}, '
      'capacity: $capacity, pooled: $pooled, released: $_released)';
}

class _Waiter {
  _Waiter(this.bytes);

  final int bytes;
  final Completer<CeyxNativeBuffer> completer = Completer<CeyxNativeBuffer>();
}
