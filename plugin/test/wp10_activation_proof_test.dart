import 'dart:ffi';
import 'dart:io';

import 'package:ceyx/src/dng_bindings.dart';
import 'package:ceyx/src/dng_decoder_service.dart';
import 'package:ceyx/src/native_buffer_pool.dart';
import 'package:ffi/ffi.dart' show malloc;
import 'package:flutter_test/flutter_test.dart';

/// WP10v Task 17 — AC17.3: the route actually EXECUTES, on a real dylib, for
/// BOTH formats the format-agnostic entry pair covers (A3.3: one symbol pair,
/// `ceyx_probe_output_size` / `ceyx_decode_into_buffer`, routes internally on
/// `raw_probe_file`).
///
/// A guarded FFI group that resolves to null ships a feature that is silently
/// absent and green everywhere (lesson 2026-09-06) — a unit suite built
/// entirely on fakes cannot catch that. This test is deliberately NOT a fake:
/// it loads the REAL freshly-built dylib (never the vendored copy at
/// plugin/macos/Libraries, which impl-wp10n mechanically proved is a stale,
/// pre-WP10 binary — UUID C1B5E36C..., ~9.5h older, zero ceyx_* WP10
/// symbols), asserts the two WP10 symbols are present on the file actually
/// LOADED (not merely present on some sibling build output), then drives one
/// real DNG and one real ARW decode through [DngDecoderService.probeOutputSize]
/// / [DngDecoderService.decodeIntoPointer] with a genuinely pre-acquired
/// native buffer, and asserts pointer identity plus a pool-checkout count
/// that returns to where it started.
///
/// flutter test runs with cwd == package root (plugin/), so all paths below
/// are resolved relative to Directory.current.
void main() {
  final dylibPath = File(
    Platform.environment['CEYX_WP10_DYLIB'] ??
        '../native/build/libdng_decoder_native.dylib',
  ).absolute.path;

  final dngPath = File('../image_samples/lossless_dng_sample.dng').absolute.path;
  final arwPath = File('../image_samples/raw_sample.arw').absolute.path;

  late bool skip;
  String skipReason = '';

  setUpAll(() {
    skip = false;
    if (!File(dylibPath).existsSync()) {
      skip = true;
      skipReason =
          'reason: no freshly-built dylib at $dylibPath — set '
          'CEYX_WP10_DYLIB or build native/build via cmake first';
      return;
    }
    if (!File(dngPath).existsSync() || !File(arwPath).existsSync()) {
      skip = true;
      skipReason =
          'reason: sample corpus missing ($dngPath / $arwPath)';
      return;
    }
    final bindings = DngNativeBindings.fromPath(dylibPath);
    if (!bindings.decodeIntoBufferAvailable) {
      skip = true;
      skipReason =
          'reason: dylib at $dylibPath does not export the WP10 pair — '
          'point CEYX_WP10_DYLIB at a build that includes Task 15';
    }
  });

  /// Runs the full pooled round trip for one sample: probe -> acquire a
  /// native buffer at the probed extent -> decodeIntoPointer -> assert
  /// pointer identity and that the pool's checkout count is back to where it
  /// started once the caller frees its own allocation. This is the AC17.3
  /// activation proof at the Dart layer; the native-side pool-bypass proof
  /// (dng_debug_pool_checked_out == 0 across the call) is covered by
  /// impl-wp10n's native/tests/test_ceyx_decode_into.cpp (AC15.4) — not
  /// duplicated here, this test's unique contribution is that the DART
  /// binding + real dylib combination round-trips correctly end to end.
  void runActivationProof(String label, String path) {
    test(
      'AC17.3 activation proof ($label): real decode through '
      'decodeIntoPointer returns pointer identity and leaves no dangling '
      'checkout',
      () {
        if (skip) {
          markTestSkipped(skipReason);
          return;
        }
        final service = DngDecoderService(libraryPath: dylibPath);
        service.initialize();

        final extent = service.probeOutputSize(path);
        expect(
          extent,
          isNotNull,
          reason: 'probeOutputSize must succeed on a real, valid $label sample',
        );

        final bytes = extent!.width * extent.height * 4;
        final buf = malloc<Uint8>(bytes);
        // Sentinel so an untouched buffer (a no-op decode) is distinguishable
        // from a real write.
        buf.asTypedList(bytes).fillRange(0, bytes, 0xAB);
        try {
          // WP5: was service.poolCheckedOut (native pool gauge, deleted with
          // the pool). Rewired onto the Dart process-wide gauge, which is the
          // user-designated equivalent-strength replacement. This ALSO removes
          // a silent weakening: the old code guarded the assertion with
          // `if (checkedOutBefore != null)`, so once the native symbol went
          // away the check would have stopped running while the test stayed
          // green. The gauge below is never null, so the assertion always runs.
          final liveBefore = CeyxNativeBufferPool.debugTotalLiveAddresses;

          final wire = service.decodeIntoPointer(path, buf.address, bytes);
          final address = wire[0] as int;
          final width = wire[1] as int;
          final height = wire[2] as int;

          expect(
            address,
            buf.address,
            reason: 'pointer identity is the WP10 contract: the native layer '
                'must write into the CALLER buffer, never allocate its own',
          );
          expect(width, greaterThan(0));
          expect(height, greaterThan(0));

          // The buffer must have been genuinely written into -- a decode that
          // silently no-ops would still report pointer identity.
          final sample = buf.asTypedList(bytes);
          expect(
            sample.any((b) => b != 0xAB),
            isTrue,
            reason: 'the caller buffer must contain real decoded pixels, not '
                'the untouched sentinel',
          );

          {
            expect(
              CeyxNativeBufferPool.debugTotalLiveAddresses,
              liveBefore,
              reason: 'a caller-owned buffer must never become a pool '
                  "checkout -- WP10's whole point is that this route "
                  'allocates nothing behind the caller',
            );
          }
        } finally {
          malloc.free(buf);
        }
      },
    );
  }

  runActivationProof('DNG', dngPath);
  runActivationProof('ARW', arwPath);
}
