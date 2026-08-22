import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:dng_processor_ffi/src/dng_bindings.dart';
import 'package:dng_processor_ffi/src/dng_decoder_service.dart';

/// Covers the guarded lookup skeleton for the upcoming
/// `dng_decode_and_process_sized` native symbol (2026-08-23 handover,
/// docs/logs/2026-08-23/targetwidth-sized-decode-handover.md §5.1;
/// contract AC4, docs/logs/2026-08-23/targetwidth-sized-decode-contract.md).
///
/// The dylib shipped as of 2026-08-23 does NOT export the sized symbol
/// (verified via `nm -gU ... | grep dng_decode_and_process_sized` -> 0
/// matches), so this suite exercises exactly the fallback path AC4
/// requires: constructing bindings against that dylib must not throw, and
/// `decodeOnWorker` with a non-null `maxDim` must still succeed and return
/// full-resolution output identical to the no-maxDim path.
///
/// flutter test runs with cwd == package root (dng_processor_ffi/), so all
/// paths below are resolved relative to Directory.current.
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
    'DngNativeBindings.fromPath constructs without throwing against a '
    'dylib lacking dng_decode_and_process_sized',
    () {
      final bindings = DngNativeBindings.fromPath(dylibPath);

      expect(bindings.sizedDecodeAvailable, isFalse);
      expect(bindings.dngDecodeAndProcessSized, isNull);
      // Existing full-resolution entry remains fully functional — proves
      // the guarded lookup did not disturb any other binding.
      expect(bindings.dngDecodeAndProcess, isNotNull);
    },
  );

  test(
    'decodeOnWorker(maxDim: 200) falls back to full resolution when the '
    'sized symbol is absent',
    () async {
      final sizedService = DngDecoderService(libraryPath: dylibPath);
      final baseline = await sizedService.decodeOnWorker(samplePath);
      final sized = await sizedService.decodeOnWorker(
        samplePath,
        maxDim: 200,
      );

      // Fallback engaged: AC4 requires the maxDim request to be silently
      // ignored, producing output identical in shape to the plain decode —
      // not a 200px-bounded image.
      expect(sized.width, greaterThan(200));
      expect(sized.height, greaterThan(200));
      expect(sized.width, equals(baseline.width));
      expect(sized.height, equals(baseline.height));
    },
    timeout: const Timeout(Duration(minutes: 2)),
  );

  test(
    'decodeOnWorker succeeds after the service has already been '
    'initialize()d (regression: Isolate.run closure must not capture '
    '`this`)',
    () async {
      // Calling initialize() first populates _bindings with a live
      // DynamicLibrary/NativeFinalizer BEFORE decodeOnWorker runs. If
      // decodeOnWorker's Isolate.run closure references an instance field
      // (e.g. `_libraryPath`) directly instead of a hoisted local, Dart
      // captures the whole `this` object graph — including the
      // now-initialized native handles — which Isolate.run cannot send,
      // throwing ArgumentError. A service that is never explicitly
      // initialize()d before decodeOnWorker would NOT reproduce this, since
      // `_bindings` stays a `late final` unset field until first use.
      final service = DngDecoderService(libraryPath: dylibPath);
      service.initialize();

      final image = await service.decodeOnWorker(samplePath);

      expect(image.width, greaterThan(0));
      expect(image.height, greaterThan(0));
    },
    timeout: const Timeout(Duration(minutes: 2)),
  );
}
