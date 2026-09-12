// S2.4 / AC5 regression: more than eight sequential decodes must complete on
// the pooled route without wedging.
//
// The historical wedge (docs/logs/2026-09-08/task4-headless-bench-io-vs-compute.md)
// stopped at exactly eight decodes: the result buffer came out of the dylib's
// fixed native set and was reclaimable only by a Dart garbage collection that
// native-heavy work never triggers, so the ninth decode waited forever. Both
// ingredients are gone — every buffer is either acquired from a slot or adopted
// on receipt, and every completion path releases explicitly — and this file is
// the executable proof rather than a prose argument.
//
// Twenty four is three times the eight-slot bound, so the pool must recycle at
// least twice: eight would prove nothing, and nine would not distinguish
// "recycles once, then stalls" from "recycles indefinitely".
import 'dart:ffi';
import 'dart:isolate';

import 'package:ceyx/ceyx.dart';
import 'package:ffi/ffi.dart' show calloc;
import 'package:flutter_test/flutter_test.dart';

/// How many decodes each case drives.
const int sequentialDecodeCount = 24;

/// Fake worker for the wedge regression: answers `probeSize` with a fixed 2x2
/// extent and answers `decode` on whichever route the pool selected.
///
/// No dylib is loaded, so this runs everywhere CI runs. The two reply shapes
/// are the production ones: five elements when a destination address arrived
/// (the pool owns that buffer already), and seven when none did, the seventh
/// element being the byte count the worker allocated — which is the wire signal
/// the pool adopts on.
void sequentialDecodeWedgeWorkerEntryPoint(List<Object?> bootstrap) {
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
    if (type == CeyxPoolJobType.probeSize) {
      poolPort.send(<Object?>[kMsgResult, requestId, 2, 2]);
      return;
    }
    const bytes = 2 * 2 * 4;
    final destinationAddress = msg.length > 6 ? msg[6] as int : 0;
    if (destinationAddress != 0) {
      // Pooled route: decode INTO the supplied slot and report that address.
      poolPort.send(
        <Object?>[kMsgResult, requestId, destinationAddress, 2, 2, 1.0, 2.0],
      );
      return;
    }
    // Self-allocating adoption sink: no slot arrived, so the worker sizes and
    // allocates the buffer itself and the pool adopts the address on receipt.
    final buffer = calloc<Uint8>(bytes);
    poolPort.send(
      <Object?>[kMsgResult, requestId, buffer.address, 2, 2, 1.0, 2.0, 1, bytes],
    );
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

void main() {
  late CeyxDecodePool pool;
  late CeyxNativeBufferPool bufferPool;

  setUp(() {
    bufferPool = CeyxNativeBufferPool(maxBuffers: 8);
    CeyxDecodePool.nativeBufferPool = bufferPool;
  });

  tearDown(() async {
    await pool.dispose();
    bufferPool.debugDisposeIdle();
    CeyxDecodePool.nativeBufferPool = CeyxNativeBufferPool.shared;
    CeyxDecodePool.debugDecodeIntoAvailable = null;
  });

  /// Drives [sequentialDecodeCount] decodes one after another, releasing each
  /// frame before starting the next, and returns how many completed.
  ///
  /// Deliberately a sequential `await` loop rather than `Future.wait`: the
  /// failure mode under test is a HANG, and the bounded per-test timeout must
  /// be able to fail the case. A single unbounded await over a collected list
  /// would hang the suite instead of failing the test.
  Future<int> driveSequentialDecodes() async {
    var decodedCount = 0;
    for (var index = 0; index < sequentialDecodeCount; index++) {
      final image = await pool.decode('wedge_regression_$index.dng');
      expect(image.width, equals(2));
      expect(image.height, equals(2));
      image.releaseToPool();
      decodedCount++;
    }
    return decodedCount;
  }

  test(
    'twenty four sequential decodes complete on the pooled route without '
    'exhausting the eight slot buffer pool',
    () async {
      CeyxDecodePool.debugDecodeIntoAvailable = true;
      pool = CeyxDecodePool(
        width: 1,
        entryPoint: sequentialDecodeWedgeWorkerEntryPoint,
      );

      expect(await driveSequentialDecodes(), equals(sequentialDecodeCount));
      // Slots were RECYCLED, not re-allocated: at three times the bound, an
      // allocation count above the bound would mean the pool grew instead of
      // returning buffers.
      expect(bufferPool.debugAllocations, lessThanOrEqualTo(bufferPool.maxBuffers));
      // The direct negation of the wedge's mechanism: no reclaim depended on a
      // garbage collection. On the old route GC was the ONLY reclaim path.
      expect(bufferPool.debugFinalizerReleases, equals(0));
      expect(CeyxNativeBufferPool.debugTotalLiveAddresses, equals(0));
    },
    timeout: const Timeout(Duration(seconds: 30)),
  );

  test(
    'twenty four sequential decodes complete on the self allocating adoption '
    'sink without exhausting the eight slot buffer pool',
    () async {
      // No decode-into entry, so no slot is ever acquired and every decode
      // takes the sink. This is the executable form of the S2.4 proof: the
      // sink occupies no slot, which is the structural reason it cannot wedge.
      CeyxDecodePool.debugDecodeIntoAvailable = false;
      pool = CeyxDecodePool(
        width: 1,
        entryPoint: sequentialDecodeWedgeWorkerEntryPoint,
      );

      expect(await driveSequentialDecodes(), equals(sequentialDecodeCount));
      expect(bufferPool.debugAdoptions, equals(sequentialDecodeCount));
      expect(bufferPool.debugAllocations, equals(0));
      expect(bufferPool.debugFinalizerReleases, equals(0));
      expect(CeyxNativeBufferPool.debugTotalLiveAddresses, equals(0));
    },
    timeout: const Timeout(Duration(seconds: 30)),
  );
}
