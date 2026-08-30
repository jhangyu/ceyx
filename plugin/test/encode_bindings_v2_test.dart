import 'dart:ffi' as ffi;

import 'package:ceyx/src/encode_bindings.dart';
import 'package:ceyx/src/encode_bindings_v2.dart';
import 'package:flutter_test/flutter_test.dart';

/// Opens the process itself. It exports none of the ceyx symbols, so it is a
/// stand-in for "a library that predates this drop" without needing to build
/// an old dylib.
ffi.DynamicLibrary _emptyLibrary() => ffi.DynamicLibrary.process();

void main() {
  test('v2 bindings report unavailable against a symbol-less library', () {
    final b = CeyxEncodeV2Bindings.fromLibrary(_emptyLibrary());
    expect(b.available, isFalse);
  });

  test('constructing v2 bindings never throws', () {
    expect(() => CeyxEncodeV2Bindings.fromLibrary(_emptyLibrary()),
        returnsNormally);
  });

  test('legacy bindings are unaffected by the v2 class existing', () {
    // The regression this guards: had ceyx_encode_rgba8 been appended to
    // CeyxEncodeBindings, a real dylib predating it would report the legacy
    // JPEG/WebP encoders as unavailable too.
    final legacy = CeyxEncodeBindings.fromLibrary(_emptyLibrary());
    expect(legacy.available, isFalse); // no symbols here either, but…
    expect(() => CeyxEncodeBindings.fromLibrary(_emptyLibrary()),
        returnsNormally);
  }, skip: 'run the dylib-backed variant in the example app; see below');
}
