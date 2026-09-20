import 'dart:ffi';
import 'dart:isolate';

import 'package:ceyx/ceyx.dart';
import 'package:ffi/ffi.dart';
import 'package:flutter_test/flutter_test.dart';

/// T12.8 — the length the pool ADVERTISES for a decode result must be the
/// requested format's byte count, not an unconditional 4 B/px; and a non-rgba8
/// job that never got a destination buffer must FAIL rather than be served as
/// rgba8.
///
/// Route fidelity matters here. These decodes run the POOLED route — slot
/// pre-acquired on the pool isolate, address handed to the worker, worker
/// writes into that exact address — because that is the only route production
/// yuv420 ever takes. The degraded (no-slot) route is not a second way to get
/// yuv420 pixels; it is the bug TC-1363 pins.
///
/// Why a fake worker rather than a real file: the length defect is purely
/// host-side Dart arithmetic over (width, height, job.format), and a real
/// decode carries those same three values through the same line. This worker
/// therefore substitutes ONLY the native pixel work — it honours the wire
/// contract exactly, decoding into the caller-provided address — and
/// reproduces the geometries T15b observed, so the numbers asserted below are
/// the very numbers T15b saw wrong.
void pooledProbeWorker(List<Object?> bootstrap) {
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
    final path = msg[3] as String;
    // `dims:<w>:<h>`
    final parts = path.split(':');
    final width = int.parse(parts[1]);
    final height = int.parse(parts[2]);

    if (type == CeyxPoolJobType.probeSize) {
      poolPort.send(<Object?>[kMsgResult, requestId, width, height]);
      return;
    }
    // WP10 wire shape: the destination address rides at index 6 when the pool
    // pre-acquired a slot. Honour it — writing somewhere else would make this
    // fake diverge from the route it is supposed to stand in for.
    final dstAddress = msg.length > 6 ? msg[6] as int : 0;
    final address = dstAddress != 0
        ? dstAddress
        : calloc<Uint8>(width * height * 4).address;
    poolPort.send(<Object?>[
      kMsgResult,
      requestId,
      address,
      width,
      height,
      1.0,
      2.0,
    ]);
  });
  poolPort.send(<Object?>[kMsgReady, jobs.sendPort]);
}

void main() {
  setUp(() {
    CeyxDecodePool.debugYuv420Available = true;
    CeyxDecodePool.debugNativeFree =
        (address) => calloc.free(Pointer<Uint8>.fromAddress(address));
  });

  tearDown(() {
    CeyxDecodePool.debugYuv420Available = null;
    CeyxDecodePool.debugDecodeIntoAvailable = null;
    CeyxDecodePool.debugNativeFree = null;
    CeyxDecodePool.nativeBufferPool = CeyxNativeBufferPool.shared;
  });

  /// The POOLED route: the slot is real, sized by the pool itself from the
  /// probed extent and the requested format.
  Future<DngImage> decodePooled(
    int width,
    int height,
    CeyxOutputFormat format,
  ) {
    CeyxDecodePool.debugDecodeIntoAvailable = true;
    CeyxDecodePool.nativeBufferPool = CeyxNativeBufferPool(maxBuffers: 2);
    final pool = CeyxDecodePool(width: 1, entryPoint: pooledProbeWorker);
    addTearDown(pool.dispose);
    return pool.decode('dims:$width:$height', format: format);
  }

  test('TC-1360: a yuv420 decode advertises the yuv420 byte count', () async {
    const w = 4080;
    const h = 3056;
    final image = await decodePooled(w, h, CeyxOutputFormat.yuv420);
    expect(
      image.rgbaData.length,
      ceyxOutputFormatByteCount(CeyxOutputFormat.yuv420, w, h),
      reason:
          'the view must not be longer than the slot the pool allocated; at '
          'w*h*4 a consumer reads 2.67x past the allocation',
    );
  });

  test('TC-1361: rgba8 lengths are unchanged', () async {
    const w = 4080;
    const h = 3056;
    final image = await decodePooled(w, h, CeyxOutputFormat.rgba8);
    expect(image.rgbaData.length, w * h * 4);
    expect(
      image.rgbaData.length,
      ceyxOutputFormatByteCount(CeyxOutputFormat.rgba8, w, h),
    );
  });

  test('TC-1362: T15b geometries advertise T15b expected numbers', () async {
    // ARW 6024x4024 — T15b observed 96,962,304 (= w*h*4).
    final arw = await decodePooled(6024, 4024, CeyxOutputFormat.yuv420);
    expect(arw.rgbaData.length, 36360864);
    // DNG 4080x3056 — T15b observed 49,873,920 (= w*h*4).
    final dng = await decodePooled(4080, 3056, CeyxOutputFormat.yuv420);
    expect(dng.rgbaData.length, 18702720);
  });

  test(
    'TC-1363: a non-rgba8 decode with no destination buffer FAILS rather '
    'than being dispatched as rgba8',
    () async {
      // The pooled route is unavailable, so no slot is acquired and the job
      // would reach the worker WITHOUT the format element on the wire — where
      // the worker reads rgba8 by construction and the self-allocating arm
      // hands back 4 B/px pixels for a 1.5 B/px request. R-J forbids exactly
      // that silent substitution at submit; this is the same rule one step
      // later, at dispatch, which is the first point the missing slot is
      // knowable.
      CeyxDecodePool.debugDecodeIntoAvailable = false;
      final pool = CeyxDecodePool(width: 1, entryPoint: pooledProbeWorker);
      addTearDown(pool.dispose);

      await expectLater(
        pool.decode('dims:64:48', format: CeyxOutputFormat.yuv420),
        throwsA(
          isA<StateError>().having(
            (StateError e) => e.message,
            'message',
            allOf(contains('yuv420'), contains('no destination buffer')),
          ),
        ),
        reason:
            'returning rgba8 pixels for a yuv420 request is a corrupt image '
            'with no error — strictly worse than failing the decode',
      );
    },
  );

  test('TC-1364: an rgba8 decode with no destination buffer still works', () {
    // The guard must be format-scoped, not route-scoped: the degraded route
    // is the normal one for rgba8 on any library without the decode-into
    // pair, and breaking it would take every such build offline.
    CeyxDecodePool.debugDecodeIntoAvailable = false;
    final pool = CeyxDecodePool(width: 1, entryPoint: pooledProbeWorker);
    addTearDown(pool.dispose);

    return expectLater(
      pool.decode('dims:64:48', format: CeyxOutputFormat.rgba8),
      completion(
        isA<DngImage>().having(
          (DngImage i) => i.rgbaData.length,
          'length',
          64 * 48 * 4,
        ),
      ),
    );
  });
}
