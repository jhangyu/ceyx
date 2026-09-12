import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_bindings.dart';

/// WI-11: one loader control flow for all platforms, with per-platform
/// *data* only. These tests exercise the data-generation seam
/// (`candidatesForTesting`, mirroring `openFirstForTesting`'s pattern) and
/// the enumerated-candidate diagnostics produced by [DngNativeBindings]'s
/// failure path when none of the candidates resolve.
void main() {
  group('S-C2: DNG_NATIVE_BUILD_DIR env override', () {
    test('candidate list includes the env-provided build dir when set', () {
      const bogusDir = '/nonexistent/wi11_bogus_build_dir';
      final candidates = DngNativeBindings.candidatesForTesting(
        'libdng_decoder_native.dylib',
        environment: const {'DNG_NATIVE_BUILD_DIR': bogusDir},
      );

      expect(
        candidates.any((c) => c.contains(bogusDir)),
        isTrue,
        reason: 'expected a candidate derived from DNG_NATIVE_BUILD_DIR',
      );
    });

    test(
        'a wholly-unresolvable candidate list raises a StateError naming '
        'the bogus DNG_NATIVE_BUILD_DIR path', () {
      const bogusDir = '/nonexistent/wi11_bogus_build_dir';
      final candidates = DngNativeBindings.candidatesForTesting(
        'libdng_decoder_native_wi11_probe.dylib',
        environment: const {'DNG_NATIVE_BUILD_DIR': bogusDir},
      );

      Object? thrown;
      try {
        DngNativeBindings.openFirstForTesting(candidates);
      } catch (e) {
        thrown = e;
      }

      expect(thrown, isA<StateError>());
      final message = (thrown as StateError).message;
      expect(message, contains(bogusDir));
    });

    test('candidate list omits the build-dir candidate when unset', () {
      final candidates = DngNativeBindings.candidatesForTesting(
        'libdng_decoder_native.dylib',
        environment: const {},
      );

      expect(
        candidates.any((c) => c.contains('DNG_NATIVE_BUILD_DIR')),
        isFalse,
      );
    });
  });

  group('S-C3: enumerated-candidate diagnostics on any platform', () {
    test(
        'a wholly-unresolvable candidate list reports >=2 distinct '
        'candidate paths and >=2 distinct error strings', () {
      const candidates = <String>[
        '/nonexistent/wi11_probe_a.so',
        '/nonexistent/wi11_probe_b.so',
        '/nonexistent/wi11_probe_c.so',
      ];

      Object? thrown;
      try {
        DngNativeBindings.openFirstForTesting(candidates);
      } catch (e) {
        thrown = e;
      }

      expect(thrown, isA<StateError>());
      final message = (thrown as StateError).message;

      final distinctPaths =
          candidates.where((c) => message.contains(c)).toSet();
      expect(distinctPaths.length, greaterThanOrEqualTo(2),
          reason: 'expected >=2 distinct candidate paths in the message');

      final errorLines = '    -> '.allMatches(message).length;
      expect(errorLines, greaterThanOrEqualTo(2),
          reason: 'expected >=2 distinct per-candidate error entries');
    });
  });
}
