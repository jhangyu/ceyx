import 'dart:ffi' as ffi;

import 'package:ffi/ffi.dart';

/// FFI bindings for the RGBA8 -> compressed still-image encode surface added
/// in `native/include/ceyx_encode_api.h` (2026-08-30). Mirrors [HeifNativeBindings]'s
/// guarded-lookup style: encode symbols are additive, so a dylib built
/// without them (or predating this drop) must leave [available] false rather
/// than throw during construction and take down decoding with it.

typedef CeyxEncodeErrorNameNative =
    ffi.Pointer<Utf8> Function(ffi.Int32 code);
typedef CeyxEncodeErrorNameDart = ffi.Pointer<Utf8> Function(int code);

typedef CeyxEncodeJpegRgba8Native =
    ffi.Int32 Function(
      ffi.Pointer<ffi.Uint8> rgba,
      ffi.Int32 width,
      ffi.Int32 height,
      ffi.Int32 quality,
      ffi.Pointer<ffi.Pointer<ffi.Uint8>> out,
      ffi.Pointer<ffi.Size> outLen,
    );
typedef CeyxEncodeJpegRgba8Dart =
    int Function(
      ffi.Pointer<ffi.Uint8> rgba,
      int width,
      int height,
      int quality,
      ffi.Pointer<ffi.Pointer<ffi.Uint8>> out,
      ffi.Pointer<ffi.Size> outLen,
    );

typedef CeyxEncodeWebpRgba8Native = CeyxEncodeJpegRgba8Native;
typedef CeyxEncodeWebpRgba8Dart = CeyxEncodeJpegRgba8Dart;

typedef CeyxEncodeFreeNative = ffi.Void Function(ffi.Pointer<ffi.Uint8> buf);
typedef CeyxEncodeFreeDart = void Function(ffi.Pointer<ffi.Uint8> buf);

/// Dart mirror of `CeyxEncodeErrorCode` (ceyx_encode_api.h). Any value or
/// spelling change there MUST be reflected here.
abstract final class CeyxEncodeErrorCode {
  static const int success = 0;
  static const int nullArg = -401;
  static const int badDimensions = -402;
  static const int badQuality = -403;
  static const int allocationFailed = -404;
  static const int encodeFailed = -405;
  static const int unsupported = -406;
  static const int unknownException = -407;
}

/// Guarded bindings to the encode entry points of `dng_decoder_native`.
///
/// Every lookup is inside a `try`/`catch`, exactly as [HeifNativeBindings]
/// does for its additive symbols: a dylib predating commit 1764a8f, or built
/// without the encoders, must leave [available] false rather than throw
/// during construction and kill ALL decoding rather than just encode.
class CeyxEncodeBindings {
  CeyxEncodeBindings._(this._jpeg, this._webp, this._free, this._errorName);

  final CeyxEncodeJpegRgba8Dart? _jpeg;
  final CeyxEncodeWebpRgba8Dart? _webp;
  final CeyxEncodeFreeDart? _free;
  final CeyxEncodeErrorNameDart? _errorName;

  bool get available =>
      _jpeg != null && _webp != null && _free != null && _errorName != null;

  CeyxEncodeJpegRgba8Dart get jpeg => _jpeg!;
  CeyxEncodeWebpRgba8Dart get webp => _webp!;
  CeyxEncodeFreeDart get free => _free!;

  /// Human-readable name for a [CeyxEncodeErrorCode] value, matching the
  /// native side's spelling for comparable log lines. Falls back to the raw
  /// code when the symbol is absent.
  String errorName(int code) {
    final fn = _errorName;
    if (fn == null) return 'code:$code';
    return fn(code).toDartString();
  }

  /// Loads from the SAME library [DngNativeBindings] resolves, so there is
  /// one dylib search order in this package rather than several that can
  /// disagree about which copy got loaded.
  factory CeyxEncodeBindings.fromLibrary(ffi.DynamicLibrary lib) {
    CeyxEncodeJpegRgba8Dart? jpeg;
    CeyxEncodeWebpRgba8Dart? webp;
    CeyxEncodeFreeDart? free;
    CeyxEncodeErrorNameDart? errorName;
    try {
      jpeg = lib
          .lookupFunction<CeyxEncodeJpegRgba8Native, CeyxEncodeJpegRgba8Dart>(
            'ceyx_encode_jpeg_rgba8',
          );
      webp = lib
          .lookupFunction<CeyxEncodeWebpRgba8Native, CeyxEncodeWebpRgba8Dart>(
            'ceyx_encode_webp_rgba8',
          );
      free = lib.lookupFunction<CeyxEncodeFreeNative, CeyxEncodeFreeDart>(
        'ceyx_encode_free',
      );
      errorName = lib
          .lookupFunction<CeyxEncodeErrorNameNative, CeyxEncodeErrorNameDart>(
            'ceyx_encode_error_name',
          );
    } catch (_) {
      // Partial success is treated as absence on purpose: some symbols
      // present is a broken drop, and calling into it would be worse than
      // degrading.
      jpeg = null;
      webp = null;
      free = null;
      errorName = null;
    }
    return CeyxEncodeBindings._(jpeg, webp, free, errorName);
  }
}
