import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_bindings.dart';

/// WP10v item (3) — absent-symbol matrix (AMENDMENT 3, A3.3: the format-
/// agnostic entry pair).
///
/// ONE guarded lookup group for the native symbols `ceyx_probe_output_size` +
/// `ceyx_decode_into_buffer`, ONE flag `decodeIntoBufferAvailable` — no
/// per-format flag, since one symbol pair now covers every route
/// (dng_bindings.dart `decodeIntoBufferAvailable`).
///
/// AMENDMENT (2026-09-19, docs/logs/2026-09-19/convergence-contract.md T4):
/// this suite used to gate its one case behind a pre-WP10 dylib snapshot at
/// `../tmp/old-dylib-wp10/`, recoverable only by fetching a pinned ceyx
/// v0.1.15 release asset by digest — a network-fetch step nobody wired up,
/// so per docs/logs/2026-09-07/pool-retire-v2-round2-review.md finding S2 the
/// case has always silently skipped in this worktree (a false green). Per the
/// T4 redesign directive, the fixture-gated degrade-behaviour case is deleted
/// rather than left to skip forever; restoring it (fetch-by-digest, or a
/// checked-in fixture) is parked, not solved here.
///
/// What replaces it: an unconditional structural check that the pair the
/// degrade-behaviour test cared about has not regressed OFF the currently
/// shipped dylib — the inverse property, always executable, no fixture
/// needed. It cannot prove the Dart-side null-degradation code path still
/// works (that needs a dylib genuinely lacking the pair), only that the pair
/// itself is still there. That gap is the same one raw_symbol_absent_test.dart
/// now documents for the RAW pair.
///
/// flutter test runs with cwd == package root (plugin/), so all paths below
/// are resolved relative to Directory.current.
void main() {
  final dylibPath = File(
    'macos/Libraries/libdng_decoder_native.dylib',
  ).absolute.path;

  test(
    'the shipped dylib exports the WP10 ceyx_probe_output_size + '
    'ceyx_decode_into_buffer pair as a group (decodeIntoBufferAvailable true, '
    'both getters non-null)',
    () {
      expect(
        File(dylibPath).existsSync(),
        isTrue,
        reason: 'shipped dylib missing at $dylibPath',
      );
      final bindings = DngNativeBindings.fromPath(dylibPath);

      expect(bindings.decodeIntoBufferAvailable, isTrue);
      expect(bindings.ceyxProbeOutputSize, isNotNull);
      expect(bindings.ceyxDecodeIntoBuffer, isNotNull);
    },
  );
}
