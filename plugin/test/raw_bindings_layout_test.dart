import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_bindings.dart';

/// Guards the guarded lookup of the RAW symbols.
///
/// flutter test runs with cwd == package root (plugin/), so all
/// paths below are resolved relative to Directory.current.
void main() {
  final dylibPath = File(
    'macos/Libraries/libdng_decoder_native.dylib',
  ).absolute.path;
  final oldDylibPath = File(
    Platform.environment['DNG_OLD_RAW_DYLIB'] ??
        '../tmp/old-dylib-raw/libdng_decoder_native.dylib',
  ).absolute.path;
  final rafPath = File(
    '../image_samples/raw_corpus/fuji_xt3.raf',
  ).absolute.path;

  var oldDylibUsable = false;
  var oldDylibSkipReason = '';

  setUpAll(() {
    expect(File(dylibPath).existsSync(), isTrue,
        reason: 'bundled dylib missing at $dylibPath');
    expect(File(rafPath).existsSync(), isTrue,
        reason: 'RAF sample missing at $rafPath');

    if (!File(oldDylibPath).existsSync()) {
      oldDylibSkipReason =
          'reason: no symbol-less dylib snapshot at $oldDylibPath — set '
          'DNG_OLD_RAW_DYLIB or re-run Phase 18 Task 1 Step 3 to populate '
          'tmp/old-dylib-raw/';
    } else if (DngNativeBindings.fromPath(oldDylibPath).rawDecodeAvailable) {
      oldDylibSkipReason =
          'reason: dylib at $oldDylibPath DOES export raw_decode_and_process '
          '— it is not the pre-Phase-17 binary this contract needs';
    } else {
      oldDylibUsable = true;
    }
  });

  test(
    'a RAF decode through the native RAW entry frees its pool buffer',
    () {
      final bindings = DngNativeBindings.fromPath(dylibPath);
      expect(bindings.rawDecodeAvailable, isTrue);
      expect(bindings.poolStatsAvailable, isTrue);

      // Drive the native entry directly — this suite must stand alone,
      // without depending on the service-layer routing added later.
      final pathPtr = rafPath.toNativeUtf8();
      try {
        final resultPtr = bindings.rawDecodeAndProcess!(pathPtr.cast(), 0);
        expect(resultPtr, isNot(ffi.nullptr));
        try {
          expect(resultPtr.ref.errorCode, 0);
          expect(resultPtr.ref.width, greaterThan(0));
          expect(resultPtr.ref.height, greaterThan(0));
        } finally {
          // Frees both the struct and its rgba_data (dng_ffi_api.h:62-68).
          bindings.dngFreeResult(resultPtr);
        }
      } finally {
        malloc.free(pathPtr);
      }

      // Everything freed above -> the pool must be empty again.
      expect(bindings.poolCheckedOut(), 0);
    },
    timeout: const Timeout(Duration(minutes: 2)),
  );

  test(
    'an old dylib without raw symbols still constructs and reports '
    'rawDecodeAvailable == false',
    () {
      if (!oldDylibUsable) {
        markTestSkipped(oldDylibSkipReason);
        return;
      }
      final bindings = DngNativeBindings.fromPath(oldDylibPath);

      expect(bindings.rawDecodeAvailable, isFalse);
      expect(bindings.rawDecodeAndProcess, isNull);
      // Every DNG binding still resolves — that is what the guard buys.
      expect(bindings.dngDecodeAndProcess, isNotNull);
    },
  );
}
