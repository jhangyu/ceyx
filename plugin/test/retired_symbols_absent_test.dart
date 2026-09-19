import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

/// Regression fence for the pool-retire campaign (docs/logs/2026-09-07/
/// pool-retire-v2-round2-review.md finding S1/S2): the eight legacy entries
/// that campaign deleted from the native decode API must never reappear in
/// the dylib ceyx ships, and the replacement entry points must stay present.
///
/// UNCONDITIONAL by design — no old-dylib fixture required. The other
/// symbol-absence suites in this directory (raw_symbol_absent_test.dart,
/// wp10_decode_into_buffer_symbol_absent_test.dart) assert Dart-side
/// GRACEFUL DEGRADATION when a guarded lookup is missing, which genuinely
/// needs a dylib that predates the pair under test. This suite asserts a
/// simpler, permanent structural property against whatever dylib is
/// currently shipped — no fixture needed, so it can never silently skip.
///
/// flutter test runs with cwd == package root (plugin/), so the path below
/// is resolved relative to Directory.current, matching the sibling suites.
void main() {
  final dylibPath = File(
    'macos/Libraries/libdng_decoder_native.dylib',
  ).absolute.path;

  // The eight entries retired by the pool-retire campaign (nm evidence at
  // pool-retire-v2-round2-review.md:60-67). Any one of these reappearing in
  // the shipped dylib means the RGBA/RGB8 output pool it depended on came
  // back too.
  const retiredSymbols = <String>[
    'dng_decode_and_process',
    'dng_decode_and_process_sized',
    'dng_free_rgba_buffer',
    'dng_rgba_output_acquire',
    'dng_rgba_output_release',
    'dng_rgba_output_checked_out_count',
    'dng_debug_pool_checked_out',
    'raw_decode_and_process',
  ];

  // Symbols that must stay present — proves a failure above is a genuine
  // absence, not an empty/corrupt/wrong-architecture binary (same discipline
  // as the review's nm table).
  const positiveControlSymbols = <String>[
    'ceyx_decode_into_buffer',
    'ceyx_probe_output_size',
    'dng_free_result',
  ];

  late Set<String> exportedSymbols;

  setUpAll(() {
    expect(
      File(dylibPath).existsSync(),
      isTrue,
      reason: 'shipped dylib missing at $dylibPath',
    );
    // nm -gU: external, defined (non-undefined) symbols only. Captured to a
    // string first, then parsed — never piped into a `grep -q` under
    // pipefail, which SIGPIPEs the producer on the first match and inverts
    // the exit code (docs/logs lessons-learned: reverse gate under pipefail).
    final result = Process.runSync('nm', ['-gU', dylibPath]);
    expect(
      result.exitCode,
      0,
      reason: 'nm failed: ${result.stderr}',
    );
    final stdoutText = result.stdout as String;
    exportedSymbols = stdoutText
        .split('\n')
        .map((line) => line.trim())
        .where((line) => line.isNotEmpty)
        .map((line) {
          // Format: "<address> T _symbol_name" (macOS Mach-O leading
          // underscore). Take the last whitespace-separated token and strip
          // the underscore.
          final token = line.split(RegExp(r'\s+')).last;
          return token.startsWith('_') ? token.substring(1) : token;
        })
        .toSet();
  });

  test('positive controls are exported (proves the nm read is meaningful)', () {
    for (final symbol in positiveControlSymbols) {
      expect(
        exportedSymbols.contains(symbol),
        isTrue,
        reason:
            'expected $symbol to be exported by $dylibPath but it was not — '
            'the dylib may be stale, wrong-architecture, or corrupt',
      );
    }
  });

  test('no retired pool-retire-campaign symbol is exported', () {
    final reintroduced = retiredSymbols
        .where((symbol) => exportedSymbols.contains(symbol))
        .toList();
    expect(
      reintroduced,
      isEmpty,
      reason:
          'retired symbol(s) reappeared in the shipped dylib: $reintroduced '
          '— the RGBA/RGB8 output pool they depended on may have come back',
    );
  });
}
