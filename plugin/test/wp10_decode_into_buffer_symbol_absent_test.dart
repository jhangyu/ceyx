import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_bindings.dart';

/// WP10v item (3) — absent-symbol matrix (AMENDMENT 3, A3.3: the format-
/// agnostic entry pair).
///
/// Target contract (per A3.4 errata, superseding AMENDMENT 2 Task 11/12):
/// ONE guarded lookup group for the native symbols `ceyx_probe_output_size` +
/// `ceyx_decode_into_buffer`, ONE flag `decodeIntoBufferAvailable` — no
/// per-format flag, since one symbol pair now covers every route. This test
/// asserts the CURRENT Dart-side contract (which as of this writing is still
/// mid-rename on impl-wp10d's in-flight commit: the getters are still named
/// `dngProbeOutputSize`/`dngDecodeIntoBuffer` and the underlying native lookup
/// still targets the pre-A3 `dng_*` symbol names). The behavioural assertion
/// — flag false, both getters null, unrelated bindings unaffected — is stable
/// across the A3 rename; only the getter/typedef names and the native symbol
/// strings the group looks up are expected to change. Re-check this file's
/// getter names against dng_bindings.dart once impl-wp10d lands the A3 rename
/// (`Ceyx*` typedefs per A3.3) — a mechanical rename, not a behavior change.
///
/// Mirrors the R4 item 1 slot-config group covered by this file's sibling
/// (`slot_config_symbol_absent_test.dart`): a dylib built before the pair
/// shipped constructs bindings without throwing and degrades the WHOLE group
/// to unsupported rather than half-working.
///
/// Fixture: the currently-pinned/shipped dylib predates the entry pair by
/// construction (WP10 has not shipped a release yet), so the "old" dylib is
/// just the vendored one at macos/Libraries — no separate old-dylib fixture
/// is needed, unlike the slot-config test (which needed a synthetic fixture
/// because the slot group HAD already shipped by the time that test was
/// written). If a later WP10-bearing dylib gets vendored into that path
/// before this test is retired, point DNG_OLD_WP10_DYLIB at a pre-WP10 copy
/// instead — same override convention as the slot-config sibling.
///
/// flutter test runs with cwd == package root (plugin/), so all paths below
/// are resolved relative to Directory.current.
void main() {
  final dylibPath = File(
    Platform.environment['DNG_OLD_WP10_DYLIB'] ??
        'macos/Libraries/libdng_decoder_native.dylib',
  ).absolute.path;

  test(
    'DngNativeBindings.fromPath constructs without throwing against a dylib '
    'lacking the R4 WP10 probe/decode-into-buffer symbol pair, and the pair '
    'degrades to unsupported as a GROUP',
    () {
      if (!File(dylibPath).existsSync()) {
        markTestSkipped(
          'reason: no dylib found at $dylibPath to exercise the WP10 '
          'symbol-absent-group contract; set DNG_OLD_WP10_DYLIB or build one',
        );
        return;
      }
      final bindings = DngNativeBindings.fromPath(dylibPath);

      if (bindings.decodeIntoBufferAvailable) {
        markTestSkipped(
          'reason: dylib at $dylibPath already exports the WP10 pair — it is '
          'not a pre-WP10 binary; point DNG_OLD_WP10_DYLIB at a genuinely '
          'older dylib to exercise the absent-group contract',
        );
        return;
      }

      expect(bindings.decodeIntoBufferAvailable, isFalse);
      expect(bindings.ceyxProbeOutputSize, isNull);
      expect(bindings.ceyxDecodeIntoBuffer, isNull);
      // The pooled decode-into route must be UNREACHABLE, not merely unused:
      // every other binding this dylib DOES export remains functional, proving
      // the guarded group's absence did not disturb unrelated lookups (same
      // shape as the slot-config sibling test).
      expect(bindings.dngDecodeAndProcess, isNotNull);
    },
  );
}
