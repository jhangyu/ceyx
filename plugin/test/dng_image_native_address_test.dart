import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_decoder_service.dart';

/// WP3a (AC3a.6): `DngImage.nativeAddress` is 0 on the legacy
/// TransferableTypedData arm — the only arm [DngDecoderService.decodeOnWorker]
/// drives. Non-zero addresses are produced EXCLUSIVELY by
/// `CeyxDecodePool._materialize`'s H2-A pointer-transfer arm (covered by
/// decode_pool_test.dart), which `decodeOnWorker` never touches.
///
/// flutter test runs with cwd == package root (plugin/).
void main() {
  final dylibPath = File(
    'macos/Libraries/libdng_decoder_native.dylib',
  ).absolute.path;
  final samplePath = File(
    '../image_samples/lossless_dng_sample.dng',
  ).absolute.path;

  setUpAll(() {
    expect(
      File(dylibPath).existsSync(),
      isTrue,
      reason: 'shipped dylib missing at $dylibPath',
    );
    expect(
      File(samplePath).existsSync(),
      isTrue,
      reason: 'sample DNG missing at $samplePath',
    );
  });

  test(
    'decodeOnWorker (the legacy TransferableTypedData arm) returns '
    'nativeAddress == 0',
    () async {
      final service = DngDecoderService(libraryPath: dylibPath);
      final image = await service.decodeOnWorker(samplePath);
      expect(image.nativeAddress, equals(0));
      expect(image.rgbaData.length, equals(image.width * image.height * 4));
    },
  );
}
