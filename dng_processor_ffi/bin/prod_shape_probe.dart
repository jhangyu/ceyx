// Permanent production-shape probe for the decodeOnWorker `this`-capture
// regression (2026-08-23, docs/logs/2026-08-23/targetwidth-sized-decode-*).
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
// Exit code is the machine verdict: 0 only when every assertion below
// holds; 1 on any failure. Do not rely on stdout text alone.
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

Future<void> main(List<String> args) async {
  if (args.isEmpty) {
    stderr.writeln('usage: prod_shape_probe.dart <path-to-sample.dng>');
    exit(1);
  }
  final sample = args[0];

  try {
    final service = DngDecoderService(); // production shape: default ctor
    service.initialize(); // production shape: eager initialize()

    final baseline = await service.decodeOnWorker(sample);
    if (baseline.width <= 0 || baseline.height <= 0) {
      stderr.writeln(
        'PROD_SHAPE_PROBE: FAIL baseline decode returned non-positive '
        'dimensions ${baseline.width}x${baseline.height}',
      );
      exit(1);
    }

    final sized = await service.decodeOnWorker(sample, maxDim: 200);
    if (sized.width != baseline.width || sized.height != baseline.height) {
      stderr.writeln(
        'PROD_SHAPE_PROBE: FAIL maxDim:200 result '
        '${sized.width}x${sized.height} does not match baseline '
        '${baseline.width}x${baseline.height} (fallback not engaged)',
      );
      exit(1);
    }

    stdout.writeln(
      'PROD_SHAPE_PROBE: OK baseline=${baseline.width}x${baseline.height} '
      'sized(maxDim:200)=${sized.width}x${sized.height}',
    );
    exit(0);
  } catch (e, st) {
    stderr.writeln('PROD_SHAPE_PROBE: THREW ${e.runtimeType}: $e');
    stderr.writeln(st);
    exit(1);
  }
}
