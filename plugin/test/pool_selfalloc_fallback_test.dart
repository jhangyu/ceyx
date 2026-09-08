// WP2 Task 2.2: every in-flight degradation path (P1 probe failure, P2 acquire
// throw, P3 decode-into refused twice, P4 resize reacquire failed) must still
// open the photo AND land on an address the Dart pool owns — served by the
// worker's self-allocating decode plus the pool's adoption on receipt, never by
// the dylib's allocating decode entry.
import 'dart:ffi';
import 'dart:isolate';

import 'package:ceyx/src/decode_pool.dart';
import 'package:ceyx/src/dng_decoder_service.dart';
import 'package:ceyx/src/native_buffer_pool.dart';
import 'package:ffi/ffi.dart' show calloc, malloc;
import 'package:flutter_test/flutter_test.dart';

/// Fake worker mirroring the PRODUCTION worker's decode arm shapes:
///   dstAddress != 0  -> decode-into: 5-element payload (pooled slot).
///   dstAddress == 0  -> the WP2 sink: allocate here, 7-element payload whose
///                       element 5 is appliedOrientation (identity: this route
///                       never orients) and element 6 is the ALLOCATED byte
///                       count, which is the wire signal the pool adopts on.
///
/// Path conventions:
///   `noprobe:*`        -> probeSize answers 0/0 (P1: no pooled route).
///   `refuse2:*`        -> every decode-into is refused (kMsgResize), so the
///                         pool's bounded retry gives up and redispatches with
///                         no slot (P3).
///   `refuse1:*`        -> the FIRST decode-into is refused, later ones succeed
///                         (drives the resize/reacquire path, P4).
///   anything else      -> probeSize answers 2x2.
void selfAllocPoolWorker(List<Object?> bootstrap) {
  final poolPort = bootstrap[0] as SendPort;
  final jobs = ReceivePort();
  var refusals = 0;
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
      final knowable = !path.startsWith('noprobe:');
      poolPort.send(<Object?>[
        kMsgResult,
        requestId,
        knowable ? 2 : 0,
        knowable ? 2 : 0,
      ]);
      return;
    }
    final dstAddress = msg.length > 6 ? msg[6] as int : 0;
    if (dstAddress != 0) {
      final refuseAlways = path.startsWith('refuse2:');
      final refuseOnce = path.startsWith('refuse1:') && refusals == 0;
      if (refuseAlways || refuseOnce) {
        refusals++;
        poolPort.send(<Object?>[kMsgResize, requestId, 2, 2]);
        return;
      }
      poolPort.send(<Object?>[kMsgResult, requestId, dstAddress, 2, 2, 1.0, 2.0]);
      return;
    }
    // The sink: no slot arrived, so the worker sizes and allocates itself.
    const bytes = 2 * 2 * 4;
    final buf = calloc<Uint8>(bytes);
    poolPort.send(<Object?>[
      kMsgResult,
      requestId,
      buf.address,
      2,
      2,
      1.0,
      2.0,
      1,
      bytes,
    ]);
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

/// A pool whose [acquire] throws on the Nth call (1-based), so the decode pool's
/// prepare/reacquire catch arms can be driven without restructuring production.
class ThrowingAcquirePool extends CeyxNativeBufferPool {
  ThrowingAcquirePool({required super.maxBuffers, required this.throwOnCall});

  final int throwOnCall;
  int acquireCalls = 0;

  @override
  Future<CeyxNativeBuffer> acquire(int bytes) {
    acquireCalls++;
    if (acquireCalls == throwOnCall) {
      return Future<CeyxNativeBuffer>.error(
        StateError('forced acquire failure #$acquireCalls'),
      );
    }
    return super.acquire(bytes);
  }
}

void main() {
  tearDown(() {
    // WP2 Task 2.4: the standing proof that no address reached the wrap site
    // unowned by the pool — which is what makes deleting the dylib-free tail
    // safe rather than merely plausible.
    expect(CeyxDecodePool.debugUnownedWraps, 0);
    // WP2 Task 2.5: nothing may still be checked out anywhere on this isolate
    // when a test ends — the Dart-side replacement for the native
    // checked-out-count leak assertion.
    expect(CeyxNativeBufferPool.debugTotalLiveAddresses, 0);
  });

  late CeyxDecodePool pool;
  final freed = <int>[];

  setUp(() {
    freed.clear();
    CeyxNativeBufferPool.debugFreeHook = freed.add;
    CeyxDecodePool.debugDecodeIntoAvailable = true;
  });

  tearDown(() async {
    await pool.dispose();
    CeyxDecodePool.nativeBufferPool?.debugDisposeIdle();
    CeyxDecodePool.nativeBufferPool = CeyxNativeBufferPool.shared;
    CeyxNativeBufferPool.debugFreeHook = null;
    CeyxDecodePool.debugDecodeIntoAvailable = null;
  });

  /// The single assertion every degradation path shares: the photo opened, and
  /// the address it opened onto is owned and reclaimable by the Dart pool.
  void expectServedByAdoption(DngImage image) {
    final buffers = CeyxDecodePool.nativeBufferPool!;
    expect(image.width, 2);
    expect(image.height, 2);
    expect(
      buffers.ownsAddress(image.nativeAddress),
      isTrue,
      reason: 'a degradation path must still yield a pool-owned address',
    );
    expect(buffers.debugAdoptions, 1);
    expect(buffers.debugFinalizerReleases, 0);

    // Round-1 review F1: the decode pool's explicit release must disarm the
    // safety net too, not only the service's.
    final detachesBefore = CeyxNativeBufferPool.debugSafetyNetDetaches;
    image.releaseToPool();
    expect(CeyxNativeBufferPool.debugSafetyNetDetaches, detachesBefore + 1,
        reason: 'releaseToPool must disarm the safety net for this buffer');
    expect(freed, <int>[image.nativeAddress]);
    expect(buffers.ownsAddress(image.nativeAddress), isFalse);
    malloc.free(Pointer<Uint8>.fromAddress(image.nativeAddress));
  }

  test('P1 probe failure is served by adoption', () async {
    CeyxDecodePool.nativeBufferPool = CeyxNativeBufferPool(maxBuffers: 2);
    pool = CeyxDecodePool(width: 1, entryPoint: selfAllocPoolWorker);
    final image = await pool.decode('noprobe:a.dng');
    expectServedByAdoption(image);
  });

  test('P2 acquire throw is served by adoption', () async {
    CeyxDecodePool.nativeBufferPool = ThrowingAcquirePool(
      maxBuffers: 2,
      throwOnCall: 1,
    );
    pool = CeyxDecodePool(width: 1, entryPoint: selfAllocPoolWorker);
    final image = await pool.decode('b.dng');
    expectServedByAdoption(image);
  });

  test('P3 decode-into refused twice is served by adoption', () async {
    CeyxDecodePool.nativeBufferPool = CeyxNativeBufferPool(maxBuffers: 2);
    pool = CeyxDecodePool(width: 1, entryPoint: selfAllocPoolWorker);
    final image = await pool.decode('refuse2:c.dng');
    expectServedByAdoption(image);
  });

  test('P4 resize reacquire failure is served by adoption', () async {
    // The first acquire succeeds (so a slot is dispatched and refused once),
    // the RE-acquire throws — the path that leaves job.slot null.
    CeyxDecodePool.nativeBufferPool = ThrowingAcquirePool(
      maxBuffers: 2,
      throwOnCall: 2,
    );
    pool = CeyxDecodePool(width: 1, entryPoint: selfAllocPoolWorker);
    final image = await pool.decode('refuse1:d.dng');
    expectServedByAdoption(image);
  });

  group('the worker-side sink itself', () {
    test('frees its own allocation when the decode throws', () {
      final freedByWorker = <int>[];
      var allocated = 0;
      expect(
        () => ceyxDecodeSelfAllocated(
          path: 'x.dng',
          probe: () => (width: 3, height: 4),
          decodeInto: (address, bytes) => throw StateError('decode blew up'),
          allocate: (bytes) {
            allocated = calloc<Uint8>(bytes).address;
            return allocated;
          },
          free: freedByWorker.add,
        ),
        throwsA(isA<StateError>()),
      );
      expect(
        freedByWorker,
        <int>[allocated],
        reason: 'no address may escape an error path',
      );
      calloc.free(Pointer<Uint8>.fromAddress(allocated));
    });

    test('sizes the buffer from the probe and appends the byte count', () {
      int? seenBytes;
      final wire = ceyxDecodeSelfAllocated(
        path: 'x.dng',
        probe: () => (width: 3, height: 4),
        decodeInto: (address, bytes) {
          seenBytes = bytes;
          return <Object?>[address, 3, 4, 1.0, 2.0];
        },
        allocate: (bytes) => 0xBEEF,
        free: (_) => fail('nothing to free on the success path'),
      );
      expect(seenBytes, 3 * 4 * 4);
      expect(wire, <Object?>[0xBEEF, 3, 4, 1.0, 2.0, 1, 3 * 4 * 4]);
    });

    test('an unprobeable file is a decode failure, not a silent zero', () {
      expect(
        () => ceyxDecodeSelfAllocated(
          path: 'x.dng',
          probe: () => null,
          decodeInto: (_, _) => fail('must not decode without an extent'),
          allocate: (_) => fail('must not allocate without an extent'),
          free: (_) => fail('nothing was allocated'),
        ),
        throwsA(isA<DngDecodeException>()),
      );
    });
  });
}
