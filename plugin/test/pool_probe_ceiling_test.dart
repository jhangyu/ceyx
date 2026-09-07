// WP2 review-fix cycle 2: a probeSize answer that cannot describe a real image
// must be treated exactly as a non-positive extent already is — "no pooled
// route" — never turned into an allocation request.
//
// Found by an independent gate, not by design: with the pooled route enabled, a
// worker answering probeSize with a decode-shaped payload puts a native ADDRESS
// where the width belongs, and the pool asked the allocator for 844 TB. It
// degraded correctly, but the failing giant allocation is real latency on the
// decode path, and a probe answer is untrusted input.
import 'dart:isolate';

import 'package:ceyx/src/decode_pool.dart';
import 'package:ceyx/src/native_buffer_pool.dart';
import 'package:ffi/ffi.dart' show calloc;
import 'package:flutter_test/flutter_test.dart';
import 'dart:ffi';

/// Answers probeSize with an extent far above any real sensor, then answers the
/// decode normally. Mirrors what a worker with no probeSize arm does in
/// practice (it reports an address as a width).
void absurdExtentWorker(List<Object?> bootstrap) {
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
      // 0x7F_FFFF_FFFF-ish: the shape a stray native address takes when read
      // as a width. Far above kRawMaxPixelCount (268435456).
      poolPort.send(<Object?>[kMsgResult, requestId, 105553116266496, 2]);
      return;
    }
    final buf = calloc<Uint8>(2 * 2 * 4);
    poolPort.send(<Object?>[kMsgResult, requestId, buf.address, 2, 2, 1.0, 2.0]);
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

void main() {
  test('an extent above the pixel ceiling is no-pooled-route, not a 844 TB '
      'allocation request', () async {
    final logs = <String>[];
    CeyxDecodePool.logger = logs.add;
    CeyxDecodePool.debugNativeFree =
        (address) => calloc.free(Pointer<Uint8>.fromAddress(address));
    CeyxDecodePool.debugDecodeIntoAvailable = true;
    CeyxDecodePool.nativeBufferPool = CeyxNativeBufferPool(maxBuffers: 2);
    addTearDown(() {
      CeyxDecodePool.logger = (_) {};
      CeyxDecodePool.debugNativeFree = null;
      CeyxDecodePool.debugDecodeIntoAvailable = null;
      CeyxDecodePool.nativeBufferPool = CeyxNativeBufferPool.shared;
    });

    final pool = CeyxDecodePool(width: 1, entryPoint: absurdExtentWorker);
    final image = await pool.decode('absurd.dng');

    // The photo still opens — degradation, not a new failure mode.
    expect(image.width, 2);
    // And no allocation was ever attempted for the absurd extent.
    expect(
      logs.where((String l) => l.contains('Could not allocate')),
      isEmpty,
      reason: 'an implausible probe answer must never reach the allocator',
    );
    expect(
      logs.where((String l) => l.contains('POOLED_PREPARE_FAILED')),
      isEmpty,
      reason: 'the ceiling must be enforced at the probe, not by a failed malloc',
    );
    await pool.dispose();
  });
}
