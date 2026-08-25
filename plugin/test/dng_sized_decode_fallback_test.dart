import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_bindings.dart';
import 'package:ceyx/src/dng_decoder_service.dart';

/// Covers the guarded lookup skeleton for `dng_decode_and_process_sized`
/// (2026-08-23 handover,
/// docs/logs/2026-08-23/targetwidth-sized-decode-handover.md §5.1;
/// contract AC4, docs/logs/2026-08-23/targetwidth-sized-decode-contract.md).
///
/// R2 note (post-handoff): the dylib now committed at
/// `macos/Libraries/libdng_decoder_native.dylib` DOES export the sized
/// symbol (R2 landed it). Tests 1 and 2 below assert the symbol-ABSENT
/// contract from §3.2 — "symbol absent -> sizedDecodeAvailable==false,
/// constructor still succeeds, every other binding works" — which is the
/// macOS-ships-first / Windows-and-Android-lag guarantee AND is AC4 itself.
/// That contract cannot be exercised against a dylib that now has the
/// symbol, so those two tests load an OLD (symbol-less) dylib instead,
/// resolved from env `DNG_OLD_DYLIB` (default:
/// `../tmp/old-dylib/libdng_decoder_native.dylib`, relative to the package
/// root). The assertions themselves are UNCHANGED from before the
/// handoff — what moved is which dylib they point at, not what they check.
///
/// Durability trade-off: `<worktree>/tmp/` is untracked, so on a fresh
/// clone (or any worktree where nobody has copied an old dylib there) these
/// two tests SKIP via `markTestSkipped` rather than silently passing or
/// asserting the opposite of the contract. The authoritative, always-runnable
/// AC4 gate is `tool/run_prod_shape_probe.sh --expect=fallback` with
/// `DNG_PROBE_LIB_DIR` pointed at an old-dylib directory — that is precisely
/// why that override exists (see prod_shape_probe.dart header comment).
///
/// Tests 3 and 4 are the `this`-capture isolate regressions from R1; they
/// are dylib-agnostic (they only need decodeOnWorker to succeed, not any
/// particular sizedDecodeAvailable value) and stay bound to the shipped
/// dylib, unchanged.
///
/// flutter test runs with cwd == package root (plugin/), so all
/// paths below are resolved relative to Directory.current.
void main() {
  final dylibPath = File(
    'macos/Libraries/libdng_decoder_native.dylib',
  ).absolute.path;
  final oldDylibPath = File(
    Platform.environment['DNG_OLD_DYLIB'] ??
        '../tmp/old-dylib/libdng_decoder_native.dylib',
  ).absolute.path;
  final samplePath = File(
    '../image_samples/lossless_dng_sample.dng',
  ).absolute.path;

  var oldDylibUsable = false;
  var oldDylibSkipReason = '';

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

    if (!File(oldDylibPath).existsSync()) {
      oldDylibSkipReason =
          'reason: no old (symbol-less) dylib found at $oldDylibPath — set '
          'DNG_OLD_DYLIB or populate tmp/old-dylib/ to exercise the '
          'symbol-absent contract; see file header for the always-runnable '
          'AC4 alternative (prod_shape_probe.dart --expect=fallback)';
    } else if (DngNativeBindings.fromPath(oldDylibPath).sizedDecodeAvailable) {
      oldDylibSkipReason =
          'reason: dylib at $oldDylibPath DOES export '
          'dng_decode_and_process_sized — it is not the old (symbol-less) '
          'binary this contract needs; point DNG_OLD_DYLIB at a genuinely '
          'pre-R2 dylib';
    } else {
      oldDylibUsable = true;
    }
  });

  test(
    'DngNativeBindings.fromPath constructs without throwing against a '
    'dylib lacking dng_decode_and_process_sized',
    () {
      if (!oldDylibUsable) {
        markTestSkipped(oldDylibSkipReason);
        return;
      }
      final bindings = DngNativeBindings.fromPath(oldDylibPath);

      expect(bindings.sizedDecodeAvailable, isFalse);
      expect(bindings.dngDecodeAndProcessSized, isNull);
      // Existing full-resolution entry remains fully functional — proves
      // the guarded lookup did not disturb any other binding.
      expect(bindings.dngDecodeAndProcess, isNotNull);
    },
  );

  test(
    'decodeOnWorker(maxDim: 200) falls back to full resolution when the '
    'sized symbol is absent',
    () async {
      if (!oldDylibUsable) {
        markTestSkipped(oldDylibSkipReason);
        return;
      }
      final sizedService = DngDecoderService(libraryPath: oldDylibPath);
      final baseline = await sizedService.decodeOnWorker(samplePath);
      final sized = await sizedService.decodeOnWorker(
        samplePath,
        maxDim: 200,
      );

      // Fallback engaged: AC4 requires the maxDim request to be silently
      // ignored, producing output identical in shape to the plain decode —
      // not a 200px-bounded image.
      expect(sized.width, greaterThan(200));
      expect(sized.height, greaterThan(200));
      expect(sized.width, equals(baseline.width));
      expect(sized.height, equals(baseline.height));
    },
    timeout: const Timeout(Duration(minutes: 2)),
  );

  test(
    'decodeOnWorker succeeds after the service has already been '
    'initialize()d (regression: Isolate.run closure must not capture '
    '`this`)',
    () async {
      // Calling initialize() first populates _bindings with a live
      // DynamicLibrary/NativeFinalizer BEFORE decodeOnWorker runs. If
      // decodeOnWorker's Isolate.run closure references an instance field
      // (e.g. `_libraryPath`) directly instead of a hoisted local, Dart
      // captures the whole `this` object graph — including the
      // now-initialized native handles — which Isolate.run cannot send,
      // throwing ArgumentError. A service that is never explicitly
      // initialize()d before decodeOnWorker would NOT reproduce this, since
      // `_bindings` stays a `late final` unset field until first use.
      final service = DngDecoderService(libraryPath: dylibPath);
      service.initialize();

      final image = await service.decodeOnWorker(samplePath);

      expect(image.width, greaterThan(0));
      expect(image.height, greaterThan(0));
    },
    timeout: const Timeout(Duration(minutes: 2)),
  );

  test(
    'decodeOnWorker succeeds after a zero-copy decode() has run first '
    '(regression: _rgbaFinalizer variant of the `this`-capture bug)',
    () async {
      // decode() (zero-copy path) lazily creates _rgbaFinalizer, a
      // NativeFinalizer — a second non-sendable field that `this`-capture
      // would drag into the isolate message alongside _bindings._lib. This
      // reproduces the reviewer's probe.log CASE_C shape.
      final service = DngDecoderService(libraryPath: dylibPath);
      service.decode(samplePath);

      final image = await service.decodeOnWorker(samplePath);

      expect(image.width, greaterThan(0));
      expect(image.height, greaterThan(0));
    },
    timeout: const Timeout(Duration(minutes: 2)),
  );
}
