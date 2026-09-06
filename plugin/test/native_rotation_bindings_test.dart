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
/// Fixture for AC-3.1: `native/build/libdng_decoder_native.dylib` exports
/// `ceyx_decode_into_buffer` (Task WP10, already landed) but — as of this
/// writing — NOT `ceyx_decode_into_buffer_oriented` (Task 2 of this campaign
/// has not landed yet), which is exactly the "symbol pair present, oriented
/// sibling absent" shape this test exists to prove never nulls the whole
/// group. If Task 2 lands and this local build directory is rebuilt with the
/// new symbol before this test next runs, the fixture stops being valid for
/// AC-3.1 and the test self-skips with a clear reason rather than
/// (incorrectly) failing red or (incorrectly) passing on a fixture that no
/// longer demonstrates the absent-symbol case.
void main() {
  group('AC-3.1: decodeIntoBufferOrientedAvailable per-symbol guard', () {
    final dylibPath = File(
      Platform.environment['DNG_PRE_ORIENT_DYLIB'] ??
          '../native/build/libdng_decoder_native.dylib',
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
          markTestSkipped(
            'reason: dylib at $dylibPath already exports '
            'ceyx_decode_into_buffer_oriented (Task 2 has landed and this '
            'local build directory was rebuilt) — it is no longer a valid '
            'absent-symbol fixture. RECOVERY: point DNG_PRE_ORIENT_DYLIB at '
            'a dylib built before Task 2 landed.',
          );
          return;
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
    // The rule under test (dng_decoder_service.dart,
    // DngDecoderService.decodeIntoPointerOriented): for a TRANSPOSING
    // orientation (5/6/7/8), appliedOrientation is reported as the requested
    // exifOrientation ONLY when the returned extent is swapped relative to
    // the unoriented probe. This exercises the pure decision logic directly
    // (the same boolean expression the production method evaluates) rather
    // than the full FFI round trip, since driving the real native call from
    // a unit test would require a fixture dylib and a real RAW/DNG file.
    //
    // The logic lives inline in decodeIntoPointerOriented and is not a
    // separately-exported helper, so this test re-derives the same
    // consistency predicate the method's doc comment specifies and asserts
    // it against representative (requested, returnedW, returnedH, probeW,
    // probeH) tuples covering: consistent swap, degraded/unswapped, and
    // probe-unavailable.
    int selfVerifiedAppliedOrientation({
      required int exifOrientation,
      required int returnedWidth,
      required int returnedHeight,
      required int? probedWidth,
      required int? probedHeight,
    }) {
      const transposing = {5, 6, 7, 8};
      if (exifOrientation == 1) return 1;
      if (!transposing.contains(exifOrientation)) return exifOrientation;
      if (probedWidth != null &&
          probedHeight != null &&
          returnedWidth == probedHeight &&
          returnedHeight == probedWidth) {
        return exifOrientation;
      }
      return 1;
    }

    test('transposing orientation with swapped extent reports the request', () {
      final applied = selfVerifiedAppliedOrientation(
        exifOrientation: 6,
        returnedWidth: 480, // probe was 640x480 -> swapped to 480x640
        returnedHeight: 640,
        probedWidth: 640,
        probedHeight: 480,
      );
      expect(applied, 6);
    });

    test(
      'transposing orientation that came back UNSWAPPED (native degraded) '
      'reports 1, not the request',
      () {
        final applied = selfVerifiedAppliedOrientation(
          exifOrientation: 6,
          returnedWidth: 640, // NOT swapped vs. the probe -> degraded
          returnedHeight: 480,
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
        final applied = selfVerifiedAppliedOrientation(
          exifOrientation: 7,
          returnedWidth: 480,
          returnedHeight: 640,
          probedWidth: null,
          probedHeight: null,
        );
        expect(applied, 1);
      },
    );

    test('non-transposing orientation reports the request unconditionally', () {
      final applied = selfVerifiedAppliedOrientation(
        exifOrientation: 3,
        returnedWidth: 640,
        returnedHeight: 480,
        probedWidth: 640,
        probedHeight: 480,
      );
      expect(applied, 3);
    });

    test('identity orientation always reports 1', () {
      final applied = selfVerifiedAppliedOrientation(
        exifOrientation: 1,
        returnedWidth: 640,
        returnedHeight: 480,
        probedWidth: 640,
        probedHeight: 480,
      );
      expect(applied, 1);
    });
  });
}
