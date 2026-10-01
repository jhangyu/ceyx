import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_decoder_service.dart';
import 'support/native_fixtures.dart';

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
