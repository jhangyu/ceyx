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
  /* --- appended 2026-08-30, codec expansion. Append-only: never renumber. --- */
  static const int badOptions = -408;
  static const int metadataRejected = -409;
  static const int badFormat = -410;
  static const int losslessUnsupported = -411;
  /* --- appended 2026-09-20, planar yuv420 encode. Append-only. --- */
  static const int badBufferSize = -412;
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

typedef CeyxEncodeJpegYuv420Native =
    ffi.Int32 Function(
      ffi.Pointer<ffi.Uint8> src,
      ffi.Size srcCapacity,
      ffi.Int32 width,
      ffi.Int32 height,
      ffi.Int32 quality,
      ffi.Pointer<ffi.Pointer<ffi.Uint8>> out,
      ffi.Pointer<ffi.Size> outLen,
    );
typedef CeyxEncodeJpegYuv420Dart =
    int Function(
      ffi.Pointer<ffi.Uint8> src,
      int srcCapacity,
      int width,
      int height,
      int quality,
      ffi.Pointer<ffi.Pointer<ffi.Uint8>> out,
      ffi.Pointer<ffi.Size> outLen,
    );

/// Guarded binding to `ceyx_encode_jpeg_yuv420` (2026-09-20 direct-encode
/// contract), looked up INDEPENDENTLY of [CeyxEncodeBindings]'s rgba8/webp
/// group and of [CeyxEncodeV2Bindings]'s generic group.
///
/// This is the fix for the hazard both of those classes' comments already
/// name: a group `try`/`catch` that nulls every symbol in the group when ONE
/// lookup throws is correct for symbols that always ship together, but wrong
/// here — a library exporting rgba8/webp encode but predating this entry must
/// keep those working. A THIRD class, with its own lookup and its own
/// [available] flag, is what keeps the yuv420 entry's absence from taking
/// down the rgba8 arm (and vice versa): each format's caller sees exactly its
/// own capability, never a capability it never asked about.
class CeyxEncodeYuv420Bindings {
  CeyxEncodeYuv420Bindings._(this._encode);

  final CeyxEncodeJpegYuv420Dart? _encode;

  bool get available => _encode != null;

  CeyxEncodeJpegYuv420Dart get encode => _encode!;

  factory CeyxEncodeYuv420Bindings.fromLibrary(ffi.DynamicLibrary lib) {
    CeyxEncodeJpegYuv420Dart? encode;
    try {
      encode = lib
          .lookupFunction<CeyxEncodeJpegYuv420Native, CeyxEncodeJpegYuv420Dart>(
        'ceyx_encode_jpeg_yuv420',
      );
    } catch (_) {
      encode = null;
    }
    return CeyxEncodeYuv420Bindings._(encode);
  }
}
