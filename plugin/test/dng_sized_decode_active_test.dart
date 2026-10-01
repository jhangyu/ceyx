import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_decoder_service.dart';
import 'support/native_fixtures.dart';

/// Covers the sized-decode ACTIVE path — sized symbol present, sized entry
/// exercised end-to-end. Complements dng_sized_decode_fallback_test.dart,
/// which covers the symbol-ABSENT fallback path (AC4).
///
/// The test (maxDim 0 / maxDim -1 still use the old entry) needs no
/// new symbol and runs unconditionally — it guards the "0 and negatives are
/// treated as no-request, never forwarded to native" contract clause
/// (round-1-dart-handoff.md §3.3) against regressing once the sized symbol
/// becomes available and the `_bindings.sizedDecodeAvailable` gate in
/// `_decodeToTransferable` starts evaluating true.
///
/// flutter test runs with cwd == package root (plugin/), so all
/// paths below are resolved relative to Directory.current.
void main() {
  final dylibPath = File(shippedDylibPath).absolute.path;
  final samplePath = File(losslessDngSamplePath).absolute.path;

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
  });

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
