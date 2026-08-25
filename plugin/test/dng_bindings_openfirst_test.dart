import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_bindings.dart';

/// D4 (2026-08-17): `_openFirst` used to keep only the LAST candidate's error,
/// which hid the real failure cause during the libjpeg/App-Sandbox blocker
/// (the failing candidate was #2, but the message showed #4's error).
/// This asserts every candidate contributes its own error entry.
void main() {
  test('_openFirst reports each candidate error individually', () {
    const candidates = <String>[
      '/nonexistent/dng_openfirst_probe_a.dylib',
      '/nonexistent/dng_openfirst_probe_b.dylib',
      '/nonexistent/dng_openfirst_probe_c.dylib',
    ];

    Object? thrown;
    try {
      DngNativeBindings.openFirstForTesting(candidates);
    } catch (e) {
      thrown = e;
    }

    expect(thrown, isA<StateError>());
    final message = (thrown as StateError).message;

    // Every candidate path appears...
    for (final c in candidates) {
      expect(message, contains(c),
          reason: 'candidate $c missing from failure message');
    }
    // ...each followed by its own error line (not just the last one).
    expect('    -> '.allMatches(message).length, candidates.length,
        reason: 'expected one per-candidate error entry per candidate');
  });
}
