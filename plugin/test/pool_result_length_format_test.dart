import 'dart:ffi';
import 'dart:isolate';

import 'package:ceyx/ceyx.dart';
import 'package:ffi/ffi.dart';
import 'package:flutter_test/flutter_test.dart';

/// T12.8 — the length the pool ADVERTISES for a decode result must be the
/// requested format's byte count, not an unconditional 4 B/px.
///
/// Why a fake worker and not a real file: the defect is purely host-side Dart
/// arithmetic in `CeyxDecodePool._materialize`, which reads `width`/`height`
/// off the worker payload and the requested format off the job. A real-file
/// decode would carry the same two integers through the same line. The fake
/// worker therefore substitutes ONLY the native decode (which the T12.7
/// evidence already covers) and reproduces the exact geometry T15b observed,
/// so the two numbers asserted below are the very numbers T15b saw wrong.
///
/// The worker allocates the CORRECT (1.5 B/px) slot, which is what production
/// does — so an over-long advertised view is a read past a real allocation.
void lengthProbeWorker(List<Object?> bootstrap) {
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
    final path = msg[3] as String;
    // `dims:<w>:<h>:<allocBytes>`
    final parts = path.split(':');
    final width = int.parse(parts[1]);
    final height = int.parse(parts[2]);
    final allocBytes = int.parse(parts[3]);
    final buf = calloc<Uint8>(allocBytes);
    poolPort.send(<Object?>[
      kMsgResult,
      requestId,
      buf.address,
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
    CeyxDecodePool.debugDecodeIntoAvailable = false;
    CeyxDecodePool.debugNativeFree =
        (address) => calloc.free(Pointer<Uint8>.fromAddress(address));
  });

  tearDown(() {
    CeyxDecodePool.debugYuv420Available = null;
    CeyxDecodePool.debugDecodeIntoAvailable = null;
  });

  Future<DngImage> decodeAt(
    int width,
    int height,
    CeyxOutputFormat format,
  ) async {
    final bytes = ceyxOutputFormatByteCount(format, width, height);
    final pool = CeyxDecodePool(width: 1, entryPoint: lengthProbeWorker);
    addTearDown(pool.dispose);
    return pool.decode('dims:$width:$height:$bytes', format: format);
  }

  test('TC-1360: a yuv420 decode advertises the yuv420 byte count', () async {
    const w = 4080;
    const h = 3056;
    final image = await decodeAt(w, h, CeyxOutputFormat.yuv420);
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
    final image = await decodeAt(w, h, CeyxOutputFormat.rgba8);
    expect(image.rgbaData.length, w * h * 4);
    expect(
      image.rgbaData.length,
      ceyxOutputFormatByteCount(CeyxOutputFormat.rgba8, w, h),
    );
  });

  test('TC-1362: T15b geometries advertise T15b expected numbers', () async {
    // ARW 6024x4024 — T15b observed 96,962,304 (= w*h*4).
    final arw = await decodeAt(6024, 4024, CeyxOutputFormat.yuv420);
    expect(arw.rgbaData.length, 36360864);
    // DNG 4080x3056 — T15b observed 49,873,920 (= w*h*4).
    final dng = await decodeAt(4080, 3056, CeyxOutputFormat.yuv420);
    expect(dng.rgbaData.length, 18702720);
  });
}
