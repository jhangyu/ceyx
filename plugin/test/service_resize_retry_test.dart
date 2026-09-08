// Round-1 review F2 (BLOCKER, contract R-A): the synchronous service arms size
// from the probe and had no resize retry, so a probe/decode extent
// disagreement threw DngBufferTooSmallException straight out of the public
// decode() — a NEW hard failure for a photo that opens today. The pooled route
// has always treated that disagreement as routine (kMsgResize).
//
// Driven end to end against the real dylib: the probe is forced to under-report
// via a narrow test seam, so the NATIVE layer issues the genuine refusal with
// its own extent, exactly as it would for a real probe/decode disagreement.
import 'dart:io';

import 'package:ceyx/src/dng_decoder_service.dart';
import 'package:ceyx/src/native_buffer_pool.dart';
import 'package:flutter_test/flutter_test.dart';

const String _kLibraryPath = 'macos/Libraries/libdng_decoder_native.dylib';
const String _kSample = '../image_samples/lossless_dng_sample.dng';

String? get _skipReason {
  if (!File(_kLibraryPath).existsSync()) {
    return 'no built dylib at $_kLibraryPath';
  }
  if (!File(_kSample).existsSync()) return 'no sample at $_kSample';
  return null;
}

void main() {
  tearDown(() {
    DngDecoderService.debugProbeOutputSizeOverride = null;
    CeyxNativeBufferPool.shared.debugDisposeIdle();
  });

  test('decode() survives a probe that under-reports the extent', () {
    final service = DngDecoderService(libraryPath: _kLibraryPath)..initialize();
    final truth = service.probeOutputSize(_kSample);
    expect(truth, isNotNull);

    // The probe now lies low. The native decode will refuse the undersized
    // buffer and report its real extent — the same shape as a genuine
    // probe/decode disagreement.
    DngDecoderService.debugProbeOutputSizeOverride = (width: 16, height: 16);

    final image = service.decode(_kSample);

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
  }, skip: _skipReason);

  test('decodeForTransfer() survives the same disagreement', () {
    final service = DngDecoderService(libraryPath: _kLibraryPath)..initialize();
    final truth = service.probeOutputSize(_kSample);
    DngDecoderService.debugProbeOutputSizeOverride = (width: 16, height: 16);

    // decodeForTransfer returns the wire list, not a DngImage:
    // [rgba, width, height, decodeMs, processMs].
    final wire = service.decodeForTransfer(_kSample);

    expect(wire[1], truth!.width);
    expect(CeyxNativeBufferPool.shared.debugCheckedOut, 0,
        reason: 'the transferable route copies out and returns both buffers');
  }, skip: _skipReason);
}
