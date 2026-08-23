// Permanent production-shape probe for the decodeOnWorker `this`-capture
// regression (2026-08-23, docs/logs/2026-08-23/targetwidth-sized-decode-*)
// AND for the R2 sized-decode wiring gate (contract AC4/AC5,
// docs/logs/2026-08-23/targetwidth-sized-decode-contract.md).
//
// Exercises the EXACT shape production callers use — default constructor,
// no `libraryPath:` override — because `libraryPath:` bypasses the real
// `DngNativeBindings.load()` candidate search this probe exists to cover:
//   dng_processor/lib/main.dart:59 field, :79 initialize(), :163
//   decodeOnWorker; dng_processor/bin/benchmark_zero_copy.dart:33/62.
//
// COVERAGE LIMIT: this probe is run via
// tool/run_prod_shape_probe.sh, which sets DNG_NATIVE_BUILD_DIR so
// `load()` resolves through candidate 3 (env override) — see
// dng_bindings.dart's `DngNativeBindings.load()` candidate list. It does
// NOT exercise candidate 1 (DYLD_LIBRARY_PATH bare name), candidate 2
// (app-bundle Frameworks/), or candidates 4a/4b (script-relative dart run
// paths). It DOES exercise the full production call shape end to end
// (default ctor -> initialize() -> decodeOnWorker), which is what would
// have caught the `this`-capture regression this probe guards against.
//
// TWO MODES (--expect=sized|fallback, default sized). The mode is chosen
// ONLY by the CLI flag, never inferred from `sizedDecodeAvailable` at
// runtime — an assertion that adapts itself to whatever the dylib happens
// to report is not a gate (round-1-dart-handoff.md §4.1: "any exit-code
// gate whose non-zero branch has never executed is an assertion about a
// gate, not a gate").
//
//   --expect=sized (DEFAULT, the R2 landing gate): requires
//     sizedDecodeAvailable == true AND sized(maxDim:200)'s long edge is
//     within +/-1px of 200 AND sized dims differ from the full-resolution
//     baseline. Run with no env override (or DNG_PROBE_LIB_DIR pointed at
//     the current shipped dylib) — this is EXPECTED TO FAIL (exit 1) against
//     a dylib built before the sized symbol landed; that failure is the red
//     proof that the gate is load-bearing, not a bug in the probe.
//
//   --expect=fallback (AC4 mode): requires sizedDecodeAvailable == false
//     AND sized(maxDim:200) dims == baseline dims (silent fallback engaged).
//     Run this against an old dylib snapshot via
//     `DNG_PROBE_LIB_DIR=<path-to-dir-containing-old-dylib>` (see
//     tool/run_prod_shape_probe.sh) to re-prove AC4 after the shipped dylib
//     is replaced with one that has the new symbol.
//
// Exit code is the machine verdict: 0 only when every assertion for the
// selected mode holds; 1 on any failure. Do not rely on stdout text alone.
//
// WHY THIS RUNS UNDER `dart run` AND NOT `flutter test`: it was confirmed
// infeasible to exercise `load()`'s default candidate search inside
// `flutter test`. A throwaway probe under `flutter test` showed
// `Platform.script` resolves to a synthetic `<pkg>/main.dart` at the
// package root (not under bin/), so load()'s script-relative candidates
// (4a/4b) never reach `dng_processor/native/dist|build`; and
// `Platform.resolvedExecutable` resolves to the Flutter SDK's own
// `flutter_tester` binary under `bin/cache/artifacts/engine/...`, so the
// execDir/../Frameworks candidate (2) resolves into the SDK's engine tree,
// not the app bundle. The only candidate reachable inside `flutter test`
// would be DNG_NATIVE_BUILD_DIR (3) via an ad-hoc env var — exactly the
// per-invocation dependency the flutter test suite must not carry. A
// standalone `dart run` probe with a committed runner script that sets its
// own environment internally (see tool/run_prod_shape_probe.sh) has no such
// problem: running the script is the whole interface.

import 'dart:io';

import 'package:dng_processor_ffi/src/dng_decoder_service.dart';

const int _requestedMaxDim = 200;
const int _maxDimToleranceOnLongEdge = 1;

enum _Expect { sized, fallback }

Never _usageAndExit() {
  stderr.writeln(
    'usage: prod_shape_probe.dart <path-to-sample.dng> '
    '[--expect=sized|--expect=fallback]  (default: --expect=sized)',
  );
  exit(1);
}

Future<void> main(List<String> args) async {
  String? sample;
  var expect = _Expect.sized;

  for (final arg in args) {
    if (arg.startsWith('--expect=')) {
      final value = arg.substring('--expect='.length);
      if (value == 'sized') {
        expect = _Expect.sized;
      } else if (value == 'fallback') {
        expect = _Expect.fallback;
      } else {
        stderr.writeln('PROD_SHAPE_PROBE: FAIL unknown --expect value "$value"');
        exit(1);
      }
    } else if (!arg.startsWith('--')) {
      sample = arg;
    }
  }
  if (sample == null) _usageAndExit();
  final samplePath = sample;

  try {
    final service = DngDecoderService(); // production shape: default ctor
    service.initialize(); // production shape: eager initialize()

    final baseline = await service.decodeOnWorker(samplePath);
    if (baseline.width <= 0 || baseline.height <= 0) {
      stderr.writeln(
        'PROD_SHAPE_PROBE: FAIL baseline decode returned non-positive '
        'dimensions ${baseline.width}x${baseline.height}',
      );
      exit(1);
    }

    final available = service.sizedDecodeAvailable;
    final sized = await service.decodeOnWorker(sample, maxDim: _requestedMaxDim);
    final sameShapeAsBaseline =
        sized.width == baseline.width && sized.height == baseline.height;
    final sizedLongEdge = sized.width > sized.height ? sized.width : sized.height;

    final modeLabel = expect == _Expect.sized ? 'sized' : 'fallback';
    final summary =
        'mode=$modeLabel baseline=${baseline.width}x${baseline.height} '
        'sized(maxDim:$_requestedMaxDim)=${sized.width}x${sized.height} '
        'sizedDecodeAvailable=$available';

    if (expect == _Expect.sized) {
      if (!available) {
        stderr.writeln(
          'PROD_SHAPE_PROBE: FAIL --expect=sized but sizedDecodeAvailable=='
          'false ($summary) — the loaded dylib does not export '
          'dng_decode_and_process_sized',
        );
        exit(1);
      }
      if (sameShapeAsBaseline) {
        stderr.writeln(
          'PROD_SHAPE_PROBE: FAIL --expect=sized but sized(maxDim:'
          '$_requestedMaxDim) matches baseline dimensions exactly '
          '($summary) — sized dispatch did not engage (silent fallback)',
        );
        exit(1);
      }
      final delta = (sizedLongEdge - _requestedMaxDim).abs();
      if (delta > _maxDimToleranceOnLongEdge) {
        stderr.writeln(
          'PROD_SHAPE_PROBE: FAIL --expect=sized but sized long edge '
          '$sizedLongEdge is not within '
          '+/-$_maxDimToleranceOnLongEdge px of $_requestedMaxDim '
          '($summary)',
        );
        exit(1);
      }
    } else {
      if (available) {
        stderr.writeln(
          'PROD_SHAPE_PROBE: FAIL --expect=fallback but sizedDecodeAvailable'
          '==true ($summary) — this dylib exports the sized symbol, not the '
          'old-binary shape AC4 requires',
        );
        exit(1);
      }
      if (!sameShapeAsBaseline) {
        stderr.writeln(
          'PROD_SHAPE_PROBE: FAIL --expect=fallback but sized(maxDim:'
          '$_requestedMaxDim) dimensions differ from baseline ($summary) — '
          'fallback did not engage',
        );
        exit(1);
      }
    }

    stdout.writeln('PROD_SHAPE_PROBE: OK $summary');
    exit(0);
  } catch (e, st) {
    stderr.writeln('PROD_SHAPE_PROBE: THREW ${e.runtimeType}: $e');
    stderr.writeln(st);
    exit(1);
  }
}
