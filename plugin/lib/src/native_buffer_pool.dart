import 'dart:async';
import 'dart:collection';
import 'dart:ffi';

import 'package:ffi/ffi.dart' show malloc;
import 'package:meta/meta.dart';

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
  }) : assert(maxBuffers > 0, 'a pool with no slots is not a pool') {
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

  /// Process-wide instance. Sized to sit ABOVE the host's byte budget, so
  /// exhaustion is a backstop rather than the operating point:
  /// [debugWaitsForCapacity] > 0 in production means the two bounds disagree.
  static final CeyxNativeBufferPool shared = CeyxNativeBufferPool(
    maxBuffers: 8,
  );

  /// Test seam: when set, every free this pool would perform routes here
  /// instead of into `malloc.free`, and every allocation is still a real
  /// `malloc` (so addresses are genuine). Mirrors
  /// `CeyxDecodePool.debugNativeFree`.
  @visibleForTesting
  static void Function(int address)? debugFreeHook;

  final List<CeyxNativeBuffer> _idle = <CeyxNativeBuffer>[];
  final Queue<_Waiter> _waiting = Queue<_Waiter>();

  /// Pooled buffers that exist right now (checked out + idle). Never exceeds
  /// [maxBuffers].
  int _live = 0;

  /// Every pooled address this pool currently owns, so a bare address arriving
  /// from elsewhere (the decode pool's free path) can be recognised as
  /// pool-owned instead of being freed behind the pool's back.
  final Map<int, CeyxNativeBuffer> _byAddress = <int, CeyxNativeBuffer>{};

  @visibleForTesting
  int debugCheckedOut = 0;

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
  /// `dng_rgba_output_checked_out_count()` gauge, at the same strength.
  ///
  /// DERIVED from [_byAddress], never maintained as a parallel set: two
  /// structures that must agree eventually disagree, and the disagreement
  /// would be invisible precisely when the gauge matters.
  @visibleForTesting
  Set<int> get debugLiveAddresses => _byAddress.entries
      .where((e) => !e.value.released)
      .map((e) => e.key)
      .toSet();

  /// Live checkouts across EVERY pool on this isolate, including [shared] and
  /// any test-constructed pool. This is the half of the native gauge that
  /// per-buffer identity accounting cannot express: "is anything, anywhere,
  /// still checked out right now?" A test that ends non-zero has found a leak
  /// in exactly the sense the native gauge meant.
  ///
  /// Counts CHECKED-OUT buffers, not allocated ones: an idle pooled buffer is
  /// reuse, not a leak.
  @visibleForTesting
  static int get debugTotalLiveAddresses {
    _instances.removeWhere(
      (WeakReference<CeyxNativeBufferPool> ref) => ref.target == null,
    );
    var total = 0;
    for (final ref in _instances) {
      total += ref.target!.debugLiveAddresses.length;
    }
    return total;
  }

  @visibleForTesting
  int get debugIdleCount => _idle.length;

  @visibleForTesting
  int get debugWaiterCount => _waiting.length;

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
      _byAddress[buffer.address] = buffer;
      return buffer;
    }

    final reused = _takeIdleFitting(bytes);
    if (reused != null) {
      debugCheckedOut++;
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
  /// Used by the decode pool's self-allocating fallback, where the worker
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
    _live++;
    debugAllocations++;
    debugCheckedOut++;
    // ponytail: one malloc per POOL SLOT for the process lifetime, not one per
    // decode. Bucketing beyond "capacity >= bytes" is not justified until a
    // mixed-resolution folder is measured to thrash it.
    final buffer = CeyxNativeBuffer._(malloc<Uint8>(bytes).address, bytes, true);
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
    malloc.free(Pointer<Uint8>.fromAddress(buffer.address));
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

/// One native allocation handed out by [CeyxNativeBufferPool].
class CeyxNativeBuffer {
  CeyxNativeBuffer._(this.address, this.capacity, this.pooled);

  /// Native address of the first byte. Process-global.
  final int address;

  /// Bytes allocated. May exceed the requested size when a larger idle buffer
  /// was reused.
  final int capacity;

  /// False for an oversize buffer served outside the pool: its release frees
  /// rather than returns.
  final bool pooled;

  bool _released = false;

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
