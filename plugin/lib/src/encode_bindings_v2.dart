import 'dart:ffi' as ffi;

import 'encode_options.dart';

/// FFI bindings for the GENERIC multi-format encode surface added in
/// `native/include/ceyx_encode_api.h` (2026-08-30 codec expansion).
///
/// A SEPARATE class from [CeyxEncodeBindings] on purpose, and this is the most
/// important compatibility decision on the Dart side: appending these lookups
/// to that class would make its `available` getter false against every dylib
/// that predates this drop, silently disabling the JPEG and WebP encode paths
/// that work perfectly well today. Two classes means an old library keeps its
/// old capability and simply reports the new one absent.

typedef CeyxEncodeSupportsNative = ffi.Int32 Function(ffi.Int32 format);
typedef CeyxEncodeSupportsDart = int Function(int format);

typedef CeyxEncodeRgba8Native =
    ffi.Int32 Function(
      ffi.Int32 format,
      ffi.Pointer<ffi.Uint8> rgba,
      ffi.Int32 width,
      ffi.Int32 height,
      ffi.Pointer<CeyxEncodeOptions> opts,
      ffi.Pointer<CeyxEncodeMetadata> meta,
      ffi.Pointer<ffi.Pointer<ffi.Uint8>> out,
      ffi.Pointer<ffi.Size> outLen,
    );
typedef CeyxEncodeRgba8Dart =
    int Function(
      int format,
      ffi.Pointer<ffi.Uint8> rgba,
      int width,
      int height,
      ffi.Pointer<CeyxEncodeOptions> opts,
      ffi.Pointer<CeyxEncodeMetadata> meta,
      ffi.Pointer<ffi.Pointer<ffi.Uint8>> out,
      ffi.Pointer<ffi.Size> outLen,
    );

/// Guarded bindings to the GENERIC encode entry points.
///
/// A SEPARATE class from `CeyxEncodeBindings` on purpose, and this is the most
/// important compatibility decision on the Dart side: appending these lookups
/// to that class would make its `available` getter false against every dylib
/// that predates this drop, silently disabling the JPEG and WebP encode paths
/// that work perfectly well today. Two classes means an old library keeps its
/// old capability and simply reports the new one absent.
class CeyxEncodeV2Bindings {
  CeyxEncodeV2Bindings._(this._encode, this._supports);

  final CeyxEncodeRgba8Dart? _encode;
  final CeyxEncodeSupportsDart? _supports;

  bool get available => _encode != null && _supports != null;

  CeyxEncodeRgba8Dart get encode => _encode!;
  CeyxEncodeSupportsDart get supports => _supports!;

  factory CeyxEncodeV2Bindings.fromLibrary(ffi.DynamicLibrary lib) {
    CeyxEncodeRgba8Dart? encode;
    CeyxEncodeSupportsDart? supports;
    try {
      encode = lib.lookupFunction<CeyxEncodeRgba8Native, CeyxEncodeRgba8Dart>(
        'ceyx_encode_rgba8',
      );
      supports =
          lib.lookupFunction<CeyxEncodeSupportsNative, CeyxEncodeSupportsDart>(
        'ceyx_encode_supports',
      );
    } catch (_) {
      // Partial success is treated as absence, exactly as
      // CeyxEncodeBindings does (encode_bindings.dart:107-115): some symbols
      // present is a broken drop, and calling into it is worse than degrading.
      encode = null;
      supports = null;
    }
    return CeyxEncodeV2Bindings._(encode, supports);
  }
}
