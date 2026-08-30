import 'dart:ffi' as ffi;

/// FFI mirror of C `CeyxEncodeOptions` (ceyx_encode_api.h).
///
/// ABI contract: 5 fields, this order. Layout on 64-bit: sizeof == 20,
/// structSize@0, quality@4, lossless@8, effort@12, reserved0@16.
/// `test/encode_options_layout_test.dart` pins the size — a field-count
/// mismatch has shipped once before in this package (Gotcha #58).
final class CeyxEncodeOptions extends ffi.Struct {
  @ffi.Uint32()
  external int structSize;

  @ffi.Int32()
  external int quality;

  @ffi.Int32()
  external int lossless;

  @ffi.Int32()
  external int effort;

  @ffi.Int32()
  external int reserved0;
}

/// FFI mirror of C `CeyxEncodeMetadata` (ceyx_encode_api.h).
///
/// Layout on 64-bit: sizeof == 56, structSize@0 (+4 pad), exif@8, exifLen@16,
/// xmp@24, xmpLen@32, icc@40, iccLen@48.
final class CeyxEncodeMetadata extends ffi.Struct {
  @ffi.Uint32()
  external int structSize;

  external ffi.Pointer<ffi.Uint8> exif;
  @ffi.Size()
  external int exifLen;

  external ffi.Pointer<ffi.Uint8> xmp;
  @ffi.Size()
  external int xmpLen;

  external ffi.Pointer<ffi.Uint8> icc;
  @ffi.Size()
  external int iccLen;
}

/// FFI mirror of C `CeyxStillResult` (ceyx_still_api.h).
///
/// Byte-identical to [HeifResult] on purpose, so one layout test covers both.
/// Layout on 64-bit: sizeof == 32, errorCode@0, width@4, height@8,
/// orientation@12, rgba@16, rgbaLen@24.
final class CeyxStillResult extends ffi.Struct {
  @ffi.Int32()
  external int errorCode;

  @ffi.Uint32()
  external int width;

  @ffi.Uint32()
  external int height;

  @ffi.Int32()
  external int orientation;

  external ffi.Pointer<ffi.Uint8> rgba;

  @ffi.Int64()
  external int rgbaLen;
}
