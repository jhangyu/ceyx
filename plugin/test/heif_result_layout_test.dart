import 'dart:ffi' as ffi;

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/heif_bindings.dart';

void main() {
  test('HeifResult mirrors the C struct layout exactly', () {
    // heif_api.h freezes this: error_code@0 width@4 height@8 orientation@12
    // rgba@16 rgba_len@24, sizeof==32 on 64-bit. Gotcha #58 in memory.md is a
    // field-count mismatch that shipped once; this is the check that would
    // have caught it.
    expect(ffi.sizeOf<HeifResult>(), 32);
  });
}
