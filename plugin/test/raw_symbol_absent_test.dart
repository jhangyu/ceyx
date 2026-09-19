import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_bindings.dart';
import 'package:ceyx/src/dng_decoder_service.dart';

/// Spec §4: symbol absent (OFF build or old dylib) must produce a typed
/// RawUnavailableException — not a crash, and not a silent fallback to the
/// DNG parser (which would report a misleading DNG error code for a RAF).
///
/// AMENDMENT (2026-09-19, docs/logs/2026-09-19/convergence-contract.md T4):
/// this suite used to gate its two behavioural cases behind a pinned
/// pre-Phase-17 dylib snapshot at `../tmp/old-dylib-raw/`. That fixture is
/// gitignored, was never committed, and predates every commit currently in
/// the tree — it cannot be regenerated without checking out a pre-Phase-17
/// commit and doing a full native rebuild, which is out of scope for a
/// plugin/test/-only change and, per
/// docs/logs/2026-09-07/pool-retire-v2-round2-review.md finding S2, was
/// producing a permanent silent skip (a false green) rather than exercising
/// anything. Per the redesign directive in the T4 task, the fixture-gated
/// cases are deleted rather than left to skip forever.
///
/// What replaces them:
/// - The structural half of this file's contract — "a dylib that lacks
///   `raw_decode_and_process`/the decode-into pair never ships" — is now a
///   permanent, unconditional nm-based check in
///   retired_symbols_absent_test.dart (`raw_decode_and_process` is one of
///   the eight pool-retire-campaign retired symbols asserted absent there).
/// - The behavioural half — "decode() on a RAF throws RawUnavailableException
///   rather than crashing or silently falling back to the DNG parser when the
///   decode-into pair is absent" — has no test-time seam to construct
///   (DngDecoderService only takes a library path, and DngNativeBindings'
///   constructor has several UNGUARDED lookups that throw when loading any
///   dylib lacking the always-present entries, so no currently-available
///   dylib — vendored or otherwise — can stand in for "has everything except
///   the RAW pair"). This remains an acknowledged coverage gap until a real
///   pre-decode-into-pair dylib snapshot is restored; flagged to the lead
///   rather than silently dropped.
///
/// This file is kept (not deleted outright) as the place to restore that
/// behavioural coverage once a fixture is available again, and as a
/// same-process sanity check that decodeIntoBufferAvailable / rawDecodeAvailable
/// agree with the direct binding query on whatever dylib the default search
/// path resolves — this part is unconditional and always executes.
void main() {
  final dylibPath = File(
    'macos/Libraries/libdng_decoder_native.dylib',
  ).absolute.path;

  test(
    'DngDecoderService.rawDecodeAvailable matches the bindings-level '
    'decodeIntoBufferAvailable flag for the shipped dylib',
    () {
      final service = DngDecoderService(libraryPath: dylibPath)..initialize();
      final bindings = DngNativeBindings.fromPath(dylibPath);
      expect(service.rawDecodeAvailable, equals(bindings.decodeIntoBufferAvailable));
    },
  );

  test(
    'raw_decode_and_process is not exported by the shipped dylib (retired; '
    'see retired_symbols_absent_test.dart for the shipped-dylib fence)',
    () {
      final bindings = DngNativeBindings.fromPath(dylibPath);
      // rawDecodeAndProcess is the guarded legacy lookup itself — this is a
      // second, independent read of the same absence retired_symbols_absent_test
      // checks via nm, exercised through the real binding path.
      expect(bindings.rawDecodeAndProcess, isNull);
    },
  );
}
