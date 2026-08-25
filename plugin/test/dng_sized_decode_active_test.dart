import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:dng_processor_ffi/src/dng_bindings.dart';
import 'package:dng_processor_ffi/src/dng_decoder_service.dart';

/// Covers the sized-decode ACTIVE path — sized symbol present, sized entry
/// exercised end-to-end. Complements dng_sized_decode_fallback_test.dart,
/// which covers the symbol-ABSENT fallback path (AC4).
///
/// Honesty note: the first test below can only genuinely exercise the sized
/// entry once the dylib under test exports `dng_decode_and_process_sized`.
/// It detects symbol presence mechanically (`DngNativeBindings.fromPath(...)
/// .sizedDecodeAvailable` against the exact dylib this suite loads) and
/// prints an explicit SKIPPED-with-reason line when absent instead of
/// silently passing as if it had verified anything. This suite deliberately
/// does NOT assert the exact honored output shape (long edge ≈200±1px) —
/// per round-1-dart-handoff.md §3.3 the Dart layer passes native dimensions
/// through verbatim and asserts nothing about them; the ±1px shape
/// assertion is `prod_shape_probe.dart`'s job, not this unit test's.
///
/// The second test (maxDim 0 / maxDim -1 still use the old entry) needs no
/// new symbol and runs unconditionally — it guards the "0 and negatives are
/// treated as no-request, never forwarded to native" contract clause
/// (round-1-dart-handoff.md §3.3) against regressing once the sized symbol
/// becomes available and the `_bindings.sizedDecodeAvailable` gate in
/// `_decodeToTransferable` starts evaluating true.
///
/// flutter test runs with cwd == package root (dng_processor_ffi/), so all
/// paths below are resolved relative to Directory.current.
void main() {
  final dylibPath = File(
    'macos/Libraries/libdng_decoder_native.dylib',
  ).absolute.path;
  final samplePath = File(
    '../image_samples/lossless_dng_sample.dng',
  ).absolute.path;

  late bool sizedAvailable;

  setUpAll(() {
    expect(
      File(dylibPath).existsSync(),
      isTrue,
      reason: 'shipped dylib missing at $dylibPath',
    );
    expect(
      File(samplePath).existsSync(),
      isTrue,
      reason: 'sample DNG missing at $samplePath',
    );
    sizedAvailable = DngNativeBindings.fromPath(
      dylibPath,
    ).sizedDecodeAvailable;
  });

  test(
    'decodeOnWorker(maxDim: 200) dispatches to the sized entry (not the '
    'fallback) when the sized symbol is present in the dylib under test',
    () async {
      if (!sizedAvailable) {
        markTestSkipped(
          'reason: dylib at $dylibPath does not export '
          'dng_decode_and_process_sized in this worktree yet — this '
          'assertion is an inert no-op, not a verified pass; see AC5 for '
          'the shape-correctness gate that requires the real symbol',
        );
        return;
      }

      final service = DngDecoderService(libraryPath: dylibPath);
      final baseline = await service.decodeOnWorker(samplePath);
      final sized = await service.decodeOnWorker(samplePath, maxDim: 200);

      // The sized entry must actually have been dispatched, not silently
      // fallen back to full resolution — a same-dims result with the symbol
      // present would mean the gating in _decodeToTransferable regressed.
      expect(
        sized.width != baseline.width || sized.height != baseline.height,
        isTrue,
        reason:
            'sized(maxDim:200) returned baseline dimensions even though '
            'sizedDecodeAvailable==true — sized dispatch did not engage',
      );
      expect(sized.width, greaterThan(0));
      expect(sized.height, greaterThan(0));
    },
    timeout: const Timeout(Duration(minutes: 2)),
  );

  test(
    'decodeOnWorker(maxDim: 0) and (maxDim: -1) still use the old entry '
    'regardless of sized-symbol availability (0/negative = "no request", '
    'never forwarded to native)',
    () async {
      final service = DngDecoderService(libraryPath: dylibPath);
      final baseline = await service.decodeOnWorker(samplePath);
      final zero = await service.decodeOnWorker(samplePath, maxDim: 0);
      final negative = await service.decodeOnWorker(samplePath, maxDim: -1);

      expect(zero.width, equals(baseline.width));
      expect(zero.height, equals(baseline.height));
      expect(negative.width, equals(baseline.width));
      expect(negative.height, equals(baseline.height));
    },
    timeout: const Timeout(Duration(minutes: 2)),
  );
}
