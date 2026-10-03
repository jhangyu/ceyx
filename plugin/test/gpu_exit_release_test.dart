import 'dart:async';

import 'package:ceyx/ceyx.dart';
import 'package:ceyx/src/gpu_shutdown.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

/// M1 T-G (IC9): the app-close hook releases the native GPU context exactly
/// once, at decode quiescence, and nothing may decode or encode afterwards.
/// A fake export stands in for `ceyx_native_release_gpu`; no dylib is needed.
void main() {
  final binding = TestWidgetsFlutterBinding.ensureInitialized();

  late List<String> logs;
  late int releases;

  setUp(() {
    CeyxGpuShutdown.debugReset();
    logs = <String>[];
    releases = 0;
    CeyxGpuShutdown.logger = logs.add;
    CeyxGpuShutdown.debugReleaseOverride = () => releases++;
  });

  tearDown(() {
    CeyxGpuShutdown.debugReset();
    CeyxNativeBufferPool.debugArenaIdleShrinkOverride = null;
    CeyxNativeBufferPool.debugPressureReliefOverride = null;
  });

  /// Delivers the framework's exit request exactly as an embedder does.
  Future<String> requestAppExit() async {
    final reply = Completer<ByteData?>();
    binding.defaultBinaryMessenger.handlePlatformMessage(
      'flutter/platform',
      const JSONMethodCodec().encodeMethodCall(
        const MethodCall('System.requestAppExit', <String, dynamic>{
          'type': 'cancelable',
        }),
      ),
      reply.complete,
    );
    final data = await reply.future;
    final decoded =
        const JSONMethodCodec().decodeEnvelope(data!) as Map<Object?, Object?>;
    return decoded['response']! as String;
  }

  test('TC-1434 an app exit request releases the GPU exactly once, loudly, '
      'and always lets the app exit', () async {
    final pool = CeyxDecodePool(width: 1);
    addTearDown(pool.dispose);

    expect(await requestAppExit(), 'exit');
    expect(releases, 1);
    expect(logs, <String>['[CeyxExit] release=done']);

    expect(await requestAppExit(), 'exit');
    expect(releases, 1, reason: 'terminal: a second request must not re-call');
  });

  test('TC-1435 a pool that never drains skips the release, loudly, and '
      'still exits', () async {
    CeyxGpuShutdown.register((Duration bound) async => false);

    await CeyxGpuShutdown.prepareForExit();

    expect(releases, 0);
    expect(logs, <String>['[CeyxExit] release=skipped reason=not-quiescent']);
  });

  test('TC-1436 an in-flight checkout is waited for, then the release runs '
      'once', () async {
    final pool = CeyxDecodePool(width: 1);
    addTearDown(pool.dispose);
    final held = await CeyxDecodePool.nativeBufferPool.acquire(4096);
    var releasedBeforeDrain = -1;

    final exit = CeyxGpuShutdown.prepareForExit();
    await Future<void>.delayed(const Duration(milliseconds: 100));
    releasedBeforeDrain = releases;
    CeyxDecodePool.nativeBufferPool.release(held);
    await exit;

    expect(releasedBeforeDrain, 0, reason: 'must not release under a checkout');
    expect(releases, 1);
    expect(logs, <String>['[CeyxExit] release=done']);
  });

  test('TC-1437 after the exit request every decode and encode entry point '
      'refuses work', () async {
    final pool = CeyxDecodePool(width: 1);
    addTearDown(pool.dispose);
    await CeyxGpuShutdown.prepareForExit();

    await expectLater(
      pool.submit(CeyxPoolJobType.decode, 'x.raf'),
      throwsA(isA<CeyxPoolUnavailableException>()),
    );
    await expectLater(
      pool.submitEncode(rgbaAddress: 1, width: 1, height: 1, quality: 90),
      throwsA(isA<CeyxPoolUnavailableException>()),
    );
    await expectLater(
      DngDecoderService().decodeOnWorker('x.raf'),
      throwsA(isA<CeyxShutdownException>()),
    );
    await expectLater(
      DngDecoderService().getPreviewJpegOnWorker('x.raf'),
      throwsA(isA<CeyxShutdownException>()),
    );
    expect(
      () => DngDecoderService().decode('x.raf'),
      throwsA(isA<CeyxShutdownException>()),
    );
    await expectLater(
      HeifDecoderService().decodeOnWorker('x.heic'),
      throwsA(isA<CeyxShutdownException>()),
    );
    await expectLater(
      CeyxStillDecoderService().decodeOnWorker('x.jpg'),
      throwsA(isA<CeyxShutdownException>()),
    );
    await expectLater(
      CeyxEncodeService().encodeJpegNative(
        Uint8List(4),
        width: 1,
        height: 1,
        quality: 90,
      ),
      throwsA(isA<CeyxShutdownException>()),
    );
  });

  test('TC-1438 no native funnel call follows the release', () async {
    var funnelCalls = 0;
    CeyxNativeBufferPool.debugPressureReliefOverride = () => 0;
    CeyxNativeBufferPool.debugArenaIdleShrinkOverride = (int floor) {
      funnelCalls++;
      return 0;
    };
    final buffers = CeyxNativeBufferPool(maxBuffers: 4, idleFloor: 2);
    addTearDown(buffers.debugDisposeIdle);

    expect(buffers.shrinkToFloor(), 0);
    expect(funnelCalls, 1, reason: 'precondition: funnel runs before release');

    await CeyxGpuShutdown.prepareForExit();
    expect(releases, 1);
    expect(buffers.shrinkToFloor(), 0);
    expect(funnelCalls, 1, reason: 'the export is the last ceyx call');
  });
}
