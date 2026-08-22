import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:dng_processor_ffi/src/dng_bindings.dart';
import 'package:dng_processor_ffi/src/dng_decoder_service.dart';

/// Covers the guarded lookup skeleton for the upcoming
/// `dng_decode_and_process_sized` native symbol (2026-08-23 handover,
/// docs/logs/2026-08-23/targetwidth-sized-decode-handover.md §5.1).
///
/// The dylib shipped as of 2026-08-23 (dng_processor_ffi/macos/Libraries/
/// libdng_decoder_native.dylib) does NOT export the sized symbol, so this
/// suite exercises exactly the fallback path AC4 requires: constructing
/// bindings against that dylib must not throw, and `decodeOnWorker` with a
/// non-null `maxDim` must still succeed and return full-resolution
/// dimensions because the sized entry point is unavailable.
void main() {
  final shippedDylib = File(
    'macos/Libraries/libdng_decoder_native.dylib',
  ).absolute.path;
  final samplePath = File(
    '../image_samples/lossless_dng_sample.dng',
  ).absolute.path;

  setUpAll(() {
    expect(
      File(shippedDylib).existsSync(),
      isTrue,
      reason: 'shipped dylib missing at $shippedDylib',
    );
    expect(
      File(samplePath).existsSync(),
      isTrue,
      reason: 'sample DNG missing at $samplePath',
    );
  });

  group('guarded dng_decode_and_process_sized lookup', () {
    test(
      'constructor does not throw against a dylib lacking the sized symbol',
      () {
        final bindings = DngNativeBindings.loadForTesting(shippedDylib);

        // AC1: constructor must complete even though the symbol is absent.
        expect(bindings.sizedDecodeAvailable, isFalse);
        expect(bindings.dngDecodeAndProcessSized, isNull);

        // Existing full-resolution entry must remain fully functional —
        // proves the guarded lookup did not disturb any other binding.
        expect(bindings.dngDecodeAndProcess, isNotNull);
      },
    );
  });

  group('decodeOnWorker(maxDim:) fallback', () {
    test(
      'maxDim:200 still decodes to full resolution when sized symbol is absent',
      () async {
        // DNG_NATIVE_BUILD_DIR steers DngNativeBindings.load() (used inside
        // the worker isolate spawned by decodeOnWorker) at the same shipped
        // dylib verified above, so this exercises the real load() path (not
        // loadForTesting) end-to-end.
        expect(
          Platform.environment['DNG_NATIVE_BUILD_DIR'],
          File('macos/Libraries').absolute.path,
          reason:
              'run this suite with DNG_NATIVE_BUILD_DIR='
              '"\$(pwd)/macos/Libraries" so decodeOnWorker\'s worker isolate '
              'resolves the shipped dylib deterministically',
        );

        final service = DngDecoderService()..initialize();
        final image = await service.decodeOnWorker(
          samplePath,
          maxDim: 200,
        );

        // Fallback engaged: AC4 requires the decode to still succeed and
        // return the FULL-resolution image, not a 200px-bounded one, since
        // the sized entry point does not exist in this dylib.
        expect(image.width, greaterThan(200));
        expect(image.height, greaterThan(200));
      },
      timeout: const Timeout(Duration(minutes: 2)),
    );
  });
}
