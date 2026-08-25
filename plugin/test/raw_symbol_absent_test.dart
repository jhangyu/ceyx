import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:dng_processor_ffi/src/dng_bindings.dart';
import 'package:dng_processor_ffi/src/dng_decoder_service.dart';
import 'package:dng_processor_ffi/src/raw_error_codes.dart';

/// Spec §4: symbol absent (OFF build or old dylib) must produce a typed
/// RawUnavailableException — not a crash, and not a silent fallback to the
/// DNG parser (which would report a misleading DNG error code for a RAF).
///
/// Fixture: the pre-Phase-17 dylib snapshotted by Task 1 into
/// tmp/old-dylib-raw/, overridable via DNG_OLD_RAW_DYLIB. Skips with an
/// explicit reason when absent, following dng_sized_decode_fallback_test.dart.
void main() {
  final oldDylibPath = File(
    Platform.environment['DNG_OLD_RAW_DYLIB'] ??
        '../tmp/old-dylib-raw/libdng_decoder_native.dylib',
  ).absolute.path;
  final rafPath = File(
    '../image_samples/raw_corpus/fuji_xt3.raf',
  ).absolute.path;
  final dngPath = File(
    '../image_samples/lossless_dng_sample.dng',
  ).absolute.path;

  var oldDylibUsable = false;
  var skipReason = '';

  setUpAll(() {
    if (!File(oldDylibPath).existsSync()) {
      skipReason =
          'reason: no symbol-less dylib snapshot at $oldDylibPath — re-run '
          'Phase 18 Task 1 Step 3, or set DNG_OLD_RAW_DYLIB';
    } else if (DngNativeBindings.fromPath(oldDylibPath).rawDecodeAvailable) {
      skipReason =
          'reason: dylib at $oldDylibPath DOES export raw_decode_and_process';
    } else {
      oldDylibUsable = true;
    }
  });

  test(
    'decode() on a RAF throws RawUnavailableException when the dylib has no '
    'RAW symbols',
    () {
      if (!oldDylibUsable) {
        markTestSkipped(skipReason);
        return;
      }
      final service = DngDecoderService(libraryPath: oldDylibPath);
      expect(service.rawDecodeAvailable, isFalse);
      expect(
        () => service.decode(rafPath),
        throwsA(
          isA<RawUnavailableException>()
              .having((e) => e.filePath, 'filePath', rafPath),
        ),
      );
      // No silent fallback: nothing was handed to the DNG parser.
      expect(
        () => service.decode(rafPath),
        isNot(throwsA(isA<DngDecodeException>())),
      );
    },
  );

  test(
    'the DNG route still works on a dylib without RAW symbols',
    () {
      if (!oldDylibUsable) {
        markTestSkipped(skipReason);
        return;
      }
      final service = DngDecoderService(libraryPath: oldDylibPath);
      final image = service.decode(dngPath);
      expect(image.width, greaterThan(0));
    },
    timeout: const Timeout(Duration(minutes: 3)),
  );
}
