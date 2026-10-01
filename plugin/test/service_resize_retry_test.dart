// Round-1 review F2 (BLOCKER, contract R-A): the synchronous service arms size
// from the probe and had no resize retry, so a probe/decode extent
// disagreement threw DngBufferTooSmallException straight out of the public
// decode() — a NEW hard failure for a photo that opens today. The pooled route
// has always treated that disagreement as routine (kMsgResize).
//
// Driven end to end against the real dylib: the probe is forced to under-report
// via a narrow test seam, so the NATIVE layer issues the genuine refusal with
// its own extent, exactly as it would for a real probe/decode disagreement.

import 'package:ceyx/src/dng_decoder_service.dart';
import 'package:ceyx/src/native_buffer_pool.dart';
import 'package:flutter_test/flutter_test.dart';
import 'support/native_fixtures.dart';


void main() {
  tearDown(() {
    DngDecoderService.debugProbeOutputSizeOverride = null;
    CeyxNativeBufferPool.shared.debugDisposeIdle();
  });

  test('decode() survives a probe that under-reports the extent', () {
    final service = DngDecoderService(libraryPath: shippedDylibPath)..initialize();
    final truth = service.probeOutputSize(losslessDngSamplePath);
    expect(truth, isNotNull);

    // The probe now lies low. The native decode will refuse the undersized
    // buffer and report its real extent — the same shape as a genuine
    // probe/decode disagreement.
    DngDecoderService.debugProbeOutputSizeOverride = (width: 16, height: 16);

    final image = service.decode(losslessDngSamplePath);

    expect(image.width, truth!.width,
        reason: 'the retry must land on the extent native actually reported');
    expect(image.height, truth.height);
    expect(
      CeyxNativeBufferPool.shared.ownsAddress(image.nativeAddress),
      isTrue,
      reason: 'the retry buffer must come from the pool, like the first one',
    );
    image.releaseToPool();
    expect(CeyxNativeBufferPool.shared.debugCheckedOut, 0,
        reason: 'the abandoned undersized buffer must not stay checked out');
  }, skip: missingFixtureReason(dylib: shippedDylibPath, sample: losslessDngSamplePath));

  test('decodeForTransfer() survives the same disagreement', () {
    final service = DngDecoderService(libraryPath: shippedDylibPath)..initialize();
    final truth = service.probeOutputSize(losslessDngSamplePath);
    DngDecoderService.debugProbeOutputSizeOverride = (width: 16, height: 16);

    // decodeForTransfer returns the wire list, not a DngImage:
    // [rgba, width, height, decodeMs, processMs].
    final wire = service.decodeForTransfer(losslessDngSamplePath);

    expect(wire[1], truth!.width);
    expect(CeyxNativeBufferPool.shared.debugCheckedOut, 0,
        reason: 'the transferable route copies out and returns both buffers');
  }, skip: missingFixtureReason(dylib: shippedDylibPath, sample: losslessDngSamplePath));
}
