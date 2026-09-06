import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_bindings.dart';
import 'package:ceyx/src/dng_decoder_service.dart';

/// Native-rotation spec Task 3 (native-rotation-spec.md §1.3/§1.4,
/// native-rotation-contract.md) — Dart bindings + decoder-service unit tests.
///
/// AC-3.1: with a stub library lacking `ceyx_decode_into_buffer_oriented`,
/// `decodeIntoBufferOrientedAvailable` is false while `decodeIntoBufferAvailable`
/// stays true (the oriented symbol is resolved with its OWN guarded lookup,
/// independent of the existing probe/decode-into pair).
///
/// AC-3.2: `DngImage()` constructed without `appliedOrientation` yields 1.
///
/// AC-3.3: the extent-consistency self-verification rule reports
/// `appliedOrientation == 1` when a transposing request came back unswapped
/// (native degraded to the unoriented fallback per spec §1.3/Task 2 AC-2.6).
///
/// AC-3.4 (dart analyze 0 issues) is verified out-of-band by the test runner,
/// not inside this file.
///
/// Fixture for AC-3.1: `plugin/macos/Libraries/libdng_decoder_native.dylib`
/// (the vendored, in-tree copy consumed by the macOS app bundle) exports
/// `ceyx_decode_into_buffer` (Task WP10, already landed) but — as of this
/// writing — NOT `ceyx_decode_into_buffer_oriented` (this campaign's Task 2
/// lands via a fresh local `native/build` output, not by updating the
/// vendored copy), which is exactly the "symbol pair present, oriented
/// sibling absent" shape this test exists to prove never nulls the whole
/// group. Unlike a local `native/build` output, this vendored copy is
/// in-tree and not rebuilt by running the native build locally, so the test
/// runs by default without requiring an env var. `DNG_PRE_ORIENT_DYLIB`
/// remains a manual override — for example if the vendored copy is ever
/// updated to include the oriented symbol, or to test a specific dylib. If
/// that happens the test self-skips with a clear reason rather than
/// (incorrectly) failing red or (incorrectly) passing on a fixture that no
/// longer demonstrates the absent-symbol case.
void main() {
  group('AC-3.1: decodeIntoBufferOrientedAvailable per-symbol guard', () {
    final dylibPath = File(
      Platform.environment['DNG_PRE_ORIENT_DYLIB'] ??
          '../plugin/macos/Libraries/libdng_decoder_native.dylib',
    ).absolute.path;

    test(
      'oriented symbol absent does not null the unoriented '
      'decodeIntoBufferAvailable group',
      () {
        if (!File(dylibPath).existsSync()) {
          markTestSkipped(
            'reason: no dylib found at $dylibPath to exercise the '
            'oriented-symbol-absent contract. RECOVERY: build the native '
            'target (produces native/build/libdng_decoder_native.dylib) or '
            'point DNG_PRE_ORIENT_DYLIB at any dylib that exports '
            'ceyx_decode_into_buffer but not ceyx_decode_into_buffer_oriented.',
          );
          return;
        }

        final bindings = DngNativeBindings.fromPath(dylibPath);

        if (bindings.decodeIntoBufferOrientedAvailable) {
          // HARD FAILURE, not a skip (round-2 fix cycle 2, reviewer item 7):
          // the vendored fixture no longer demonstrating the absent-symbol
          // case is a maintenance action (repoint the fixture / set
          // DNG_PRE_ORIENT_DYLIB), not something this test should silently
          // pass over — a skip here would let AC-3.1 quietly stop being
          // exercised at the next pin bump without anyone noticing.
          fail(
            'dylib at $dylibPath already exports '
            'ceyx_decode_into_buffer_oriented — it is no longer a valid '
            'absent-symbol fixture for AC-3.1 (the vendored copy was '
            'likely updated to a build that includes the oriented symbol). '
            'RECOVERY: point DNG_PRE_ORIENT_DYLIB at a dylib that exports '
            'ceyx_decode_into_buffer but NOT ceyx_decode_into_buffer_oriented '
            '(e.g. a pre-Task-2 native/build output), or update this test\'s '
            'default fixture path to a dylib that still lacks the symbol.',
          );
        }

        // The symbol under test is absent...
        expect(bindings.decodeIntoBufferOrientedAvailable, isFalse);
        expect(bindings.ceyxDecodeIntoBufferOriented, isNull);

        // ...but the EXISTING unoriented group must be unaffected. This is
        // the crux of AC-3.1: a missing oriented symbol must not null out an
        // unrelated (already-shipped) group.
        if (!bindings.decodeIntoBufferAvailable) {
          markTestSkipped(
            'reason: dylib at $dylibPath does not even export the '
            'unoriented ceyx_decode_into_buffer pair, so it cannot '
            'demonstrate "absence of the oriented symbol does not affect '
            'the unoriented group". RECOVERY: use a dylib built after WP10 '
            'landed but before native-rotation Task 2.',
          );
          return;
        }
        expect(bindings.decodeIntoBufferAvailable, isTrue);
        expect(bindings.ceyxProbeOutputSize, isNotNull);
        expect(bindings.ceyxDecodeIntoBuffer, isNotNull);
        // Unrelated bindings must also stay functional.
        expect(bindings.dngDecodeAndProcess, isNotNull);
      },
    );
  });

  group('AC-3.2: DngImage.appliedOrientation default', () {
    test('DngImage() constructed without appliedOrientation yields 1', () {
      final image = DngImage(
        rgbaData: Uint8List(0),
        width: 1,
        height: 1,
        decodeMs: 0,
        processMs: 0,
      );
      expect(image.appliedOrientation, 1);
    });

    test('DngImage() honors an explicit appliedOrientation', () {
      final image = DngImage(
        rgbaData: Uint8List(0),
        width: 1,
        height: 1,
        decodeMs: 0,
        processMs: 0,
        appliedOrientation: 6,
      );
      expect(image.appliedOrientation, 6);
    });
  });

  group('AC-3.3: extent-consistency self-verification', () {
    // Calls DngDecoderService.selfVerifiedAppliedOrientation DIRECTLY — the
    // exact @visibleForTesting static decodeIntoPointerOriented uses in
    // production — rather than a re-derived copy of its logic, so this test
    // cannot go green while production logic drifts (round-2 review finding).
    test('transposing orientation with swapped extent reports the request', () {
      final applied = DngDecoderService.selfVerifiedAppliedOrientation(
        requested: 6,
        width: 480, // probe was 640x480 -> swapped to 480x640
        height: 640,
        probedWidth: 640,
        probedHeight: 480,
      );
      expect(applied, 6);
    });

    test(
      'transposing orientation that came back UNSWAPPED (native degraded) '
      'reports 1, not the request',
      () {
        final applied = DngDecoderService.selfVerifiedAppliedOrientation(
          requested: 6,
          width: 640, // NOT swapped vs. the probe -> degraded
          height: 480,
          probedWidth: 640,
          probedHeight: 480,
        );
        expect(applied, 1);
      },
    );

    test(
      'transposing orientation with an unavailable probe cannot be '
      'verified and reports 1',
      () {
        final applied = DngDecoderService.selfVerifiedAppliedOrientation(
          requested: 7,
          width: 480,
          height: 640,
          probedWidth: null,
          probedHeight: null,
        );
        expect(applied, 1);
      },
    );

    test(
      'KNOWN LIMITATION: a SQUARE probed frame reports the request even '
      'though a genuine swap and a silent scratch-exhaustion degrade '
      '(spec Task 2 AC-2.6) produce an IDENTICAL extent and are therefore '
      'indistinguishable here. This is spec-exact and deliberate: treating '
      'a square probe as "unverifiable -> report 1" was tried and reverted '
      '(round-2 fix cycle 2) because it double-rotates every ORDINARY '
      '(non-degraded) square-frame transposing decode, which is a '
      'deterministic wrong answer on the common path — worse than the '
      'rare misreport on an actually-degraded square frame this documents. '
      'PARKED fix: an explicit native degradation signal (Task 2 does not '
      'currently expose one); a process-global flag is racy under '
      'concurrent pool workers and needs a per-call design.',
      () {
        final applied = DngDecoderService.selfVerifiedAppliedOrientation(
          requested: 6,
          width: 512,
          height: 512,
          probedWidth: 512,
          probedHeight: 512,
        );
        expect(applied, 6);
      },
    );

    test('non-transposing orientation reports the request unconditionally', () {
      final applied = DngDecoderService.selfVerifiedAppliedOrientation(
        requested: 3,
        width: 640,
        height: 480,
        probedWidth: 640,
        probedHeight: 480,
      );
      expect(applied, 3);
    });

    test('identity orientation always reports 1', () {
      final applied = DngDecoderService.selfVerifiedAppliedOrientation(
        requested: 1,
        width: 640,
        height: 480,
        probedWidth: 640,
        probedHeight: 480,
      );
      expect(applied, 1);
    });
  });
}
