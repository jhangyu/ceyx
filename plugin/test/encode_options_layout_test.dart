import 'dart:ffi' as ffi;

import 'package:ceyx/src/encode_options.dart';
import 'package:ceyx/src/heif_bindings.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('CeyxEncodeOptions is 20 bytes', () {
    expect(ffi.sizeOf<CeyxEncodeOptions>(), 20);
  });

  test('CeyxStillResult is 32 bytes and matches HeifResult', () {
    expect(ffi.sizeOf<CeyxStillResult>(), 32);
    expect(ffi.sizeOf<CeyxStillResult>(), ffi.sizeOf<HeifResult>());
  });

  test('CeyxEncodeMetadata is 56 bytes on a 64-bit host', () {
    expect(ffi.sizeOf<ffi.Pointer<ffi.Void>>(), 8,
        reason: 'this expectation is 64-bit only');
    expect(ffi.sizeOf<CeyxEncodeMetadata>(), 56);
  });
}
