import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_bindings.dart';

/// Guards the guarded lookup of the RAW symbols.
///
/// `raw_decode_and_process` is RETIRED — see retired_symbols_absent_test.dart,
/// which asserts it is absent from every current dylib, and export_manifest
/// .toml's `raw` group, whose `expected_on` is deliberately empty for exactly
/// that reason. So the only thing left to guard here is the OTHER direction:
/// that a pinned pre-retirement dylib still constructs, reports the symbol
/// missing, and leaves every DNG binding resolved. That is what the guarded
/// lookup buys and it is still worth a test.
///
/// flutter test runs with cwd == package root (plugin/), so all
/// paths below are resolved relative to Directory.current.
void main() {
  final oldDylibPath = File(
    Platform.environment['DNG_OLD_RAW_DYLIB'] ??
        '../tmp/old-dylib-raw/libdng_decoder_native.dylib',
  ).absolute.path;

  var oldDylibUsable = false;
  var oldDylibSkipReason = '';

  setUpAll(() {
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
