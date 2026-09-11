import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_bindings.dart';

/// R4 item 1's slot-configuration entries (`dng_decode_configure_slots` and
/// its three siblings, dng_bindings.dart:146-152) ship as one guarded group,
/// looked up in a single try/catch (dng_bindings.dart:299-329): a dylib
/// exposing none of the four degrades `slotConfigAvailable` to false with no
/// crash and no failing test — the delivered-feature-ships-inert finding at
/// docs/logs/2026-08-25/r4-round2-close-addendum.md §3.4. No test previously
/// exercised this degraded path.
///
/// Fixture: a stale vendored dylib that predates R4 item 1's slot-config
/// symbols, overridable via DNG_OLD_SLOTCFG_DYLIB. Skips with an explicit
/// reason when absent, following the pattern established by
/// dng_sized_decode_fallback_test.dart and raw_symbol_absent_test.dart.
///
/// flutter test runs with cwd == package root (plugin/), so all paths below
/// are resolved relative to Directory.current.
void main() {
  final dylibPath = File(
    'macos/Libraries/libdng_decoder_native.dylib',
  ).absolute.path;
  final oldDylibPath = File(
    Platform.environment['DNG_OLD_SLOTCFG_DYLIB'] ??
        '../tmp/old-dylib-slotcfg/libdng_decoder_native.dylib',
  ).absolute.path;

  var oldDylibUsable = false;
  var skipReason = '';

  setUpAll(() {
    expect(
      File(dylibPath).existsSync(),
      isTrue,
      reason: 'shipped dylib missing at $dylibPath',
    );

    if (!File(oldDylibPath).existsSync()) {
      skipReason =
          'reason: no old (slot-config-symbol-less) dylib found at '
          '$oldDylibPath — set DNG_OLD_SLOTCFG_DYLIB or populate '
          'tmp/old-dylib-slotcfg/ to exercise the symbol-absent-group '
          'contract for R4 item 1';
    } else if (DngNativeBindings.fromPath(oldDylibPath).slotConfigAvailable) {
      skipReason =
          'reason: dylib at $oldDylibPath DOES export '
          'dng_decode_configure_slots — it is not the pre-item-1 binary '
          'this contract needs; point DNG_OLD_SLOTCFG_DYLIB at a genuinely '
          'older dylib';
    } else {
      oldDylibUsable = true;
    }
  });

  test(
    'DngNativeBindings.fromPath constructs without throwing against a '
    'dylib lacking the R4 item 1 slot-configuration symbol group',
    () {
      if (!oldDylibUsable) {
        markTestSkipped(skipReason);
        return;
      }
      final bindings = DngNativeBindings.fromPath(oldDylibPath);

      expect(bindings.slotConfigAvailable, isFalse);
      expect(bindings.dngDecodeConfigureSlots, isNull);
      expect(bindings.configuredSlots(), isNull);
      expect(bindings.recommendedSlotsForPixels(0), isNull);
      expect(bindings.recommendationClassPixels(0), isNull);
      // Every other binding remains fully functional — proves the guarded
      // group's absence did not disturb unrelated lookups.
      expect(bindings.dngDecodeAndProcess, isNotNull);
    },
  );
}
