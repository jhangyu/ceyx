import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_bindings.dart';
import 'package:ceyx/src/dng_decoder_service.dart';
import 'support/native_fixtures.dart';

/// Native-rotation spec Task 3 (native-rotation-spec.md §1.3/§1.4,
/// native-rotation-contract.md) — Dart bindings + decoder-service unit tests.
///
/// AC-3.2: `DngImage()` constructed without `appliedOrientation` yields 1.
///
/// AC-3.3: the extent-consistency self-verification rule (AMENDED by
/// productionization plan Task 9, reconciliation 2/3: the scratch-degrade
/// fallback that used to make "unswapped extent" a benign signal is deleted,
/// so this rule now throws [CeyxOrientationContractException] instead of
/// reporting `appliedOrientation == 1` in that case; with no reference to
/// verify against it trusts the success code and reports the request).
///
/// AC-3.4 (dart analyze 0 issues) is verified out-of-band by the test runner,
/// not inside this file.
void main() {
  group('AC-3.1b: oriented symbol ACTIVATION on the shipped dylib', () {
    // Proves the guard doesn't under-report
    // on the library actually shipped to the app bundle. Without it, a pin
    // bump that silently dropped the oriented symbol would leave the whole
    // suite green (the FFI lookup is guarded, so a missing symbol degrades
    // quietly to the unoriented path rather than crashing).
    // flutter test runs with cwd == package root (plugin/).
    final shippedPath = File(shippedDylibPath).absolute.path;

    test('vendored macos/Libraries dylib exports the oriented entry', () {
      if (!File(shippedPath).existsSync()) {
        markTestSkipped(
          'reason: no vendored dylib at $shippedPath. RECOVERY: run '
          '`python3 scripts/build_apps.py --fetch-native` from the Halcyon '
          'checkout to place the pinned ceyx release libraries (the path is '
          'gitignored, so it is absent in a fresh clone and in CI).',
        );
        return;
      }

      final bindings = DngNativeBindings.fromPath(shippedPath);
      expect(
        bindings.decodeIntoBufferOrientedAvailable,
        isTrue,
        reason:
            'the shipped dylib at $shippedPath does NOT export '
            'ceyx_decode_into_buffer_oriented — the pinned ceyx release '
            'predates native-rotation Task 2, or the fetch placed a stale '
            'library. Native orientation would silently degrade to the '
            'unoriented path.',
      );
      expect(bindings.ceyxDecodeIntoBufferOriented, isNotNull);
      // The unoriented group must remain available alongside it.
      expect(bindings.decodeIntoBufferAvailable, isTrue);
    });
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
      'transposing orientation that came back UNSWAPPED now raises a '
      'contract error instead of silently reporting 1 (productionization '
      'plan Task 9, reconciliation 2/3: the scratch-degrade fallback that '
      'used to make this a benign signal is gone, so an unswapped extent on '
      'a transposing request means the kernel silently failed to orient)',
      () {
        expect(
          () => DngDecoderService.selfVerifiedAppliedOrientation(
            requested: 6,
            width: 640, // NOT swapped vs. the reference -> contract violation
            height: 480,
            probedWidth: 640,
            probedHeight: 480,
          ),
          throwsA(isA<CeyxOrientationContractException>()),
        );
      },
    );

    test(
      'transposing orientation with no unoriented reference to compare '
      'against (probedWidth/probedHeight both null) trusts the success code '
      'and reports the request — the old conservative "cannot verify -> '
      'report 1" no longer applies once the fallback it was guarding '
      'against is deleted',
      () {
        final applied = DngDecoderService.selfVerifiedAppliedOrientation(
          requested: 7,
          width: 480,
          height: 640,
          probedWidth: null,
          probedHeight: null,
        );
        expect(applied, 7);
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
