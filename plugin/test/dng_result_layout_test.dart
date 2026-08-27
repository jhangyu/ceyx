import 'dart:ffi' as ffi;
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/dng_bindings.dart';

/// Guards the C<->Dart layout of the primary result struct DngResult
/// (native/include/dng_ffi_api.h:18-25), whose C side is protected by 7
/// compile-time static_asserts (dng_ffi_api.h:120-133). The Dart mirror in
/// dng_bindings.dart:25-42 had zero automated verification; a silent drift
/// (field reorder / wrong @Int32 vs @Double / missing field) would mis-read
/// native memory at runtime instead of failing a test — Gotcha #58.
///
/// Field offsets are verified by writing distinct values through the struct
/// API and reading the raw bytes back at the expected byte offsets.
void main() {
  test('DngResult matches the C layout (sizeOf == 40)', () {
    // ptr(8)@0 + 3x int32(4)@8,12,16 + pad(4) + 2x double(8)@24,32 = 40.
    expect(ffi.sizeOf<DngResult>(), 40);
  });

  test('DngResult field offsets match the C static_asserts', () {
    final ptr = calloc<DngResult>();
    try {
      ptr.ref.rgbaData = ffi.Pointer<ffi.Uint8>.fromAddress(0x1122334455667788);
      ptr.ref.width = 0x11111111;
      ptr.ref.height = 0x22222222;
      ptr.ref.errorCode = 0x33333333;
      ptr.ref.decodeMs = 1.5;
      ptr.ref.processMs = 2.5;

      final bytes = ptr.cast<ffi.Uint8>().asTypedList(ffi.sizeOf<DngResult>());
      final bd = ByteData.sublistView(Uint8List.fromList(bytes));

      // rgba_data @ 0 (native little-endian pointer).
      expect(bd.getUint64(0, Endian.little), 0x1122334455667788);
      // width @ 8, height @ 12, error_code @ 16.
      expect(bd.getInt32(8, Endian.little), 0x11111111);
      expect(bd.getInt32(12, Endian.little), 0x22222222);
      expect(bd.getInt32(16, Endian.little), 0x33333333);
      // decode_ms @ 24, process_ms @ 32.
      expect(bd.getFloat64(24, Endian.little), 1.5);
      expect(bd.getFloat64(32, Endian.little), 2.5);
    } finally {
      calloc.free(ptr);
    }
  });
}
