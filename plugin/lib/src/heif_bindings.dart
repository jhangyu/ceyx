import 'dart:ffi' as ffi;

import 'package:ffi/ffi.dart';

import 'dng_bindings.dart';

/// FFI struct matching C `HeifResult` from `heif_api.h`.
///
/// ABI contract: 6 fields, this order. Layout on 64-bit: sizeof==32,
/// error_code@0, width@4, height@8, orientation@12, rgba@16, rgba_len@24.
/// `test/heif_result_layout_test.dart` pins the size (Gotcha #58: a
/// field-count mismatch shipped once).
final class HeifResult extends ffi.Struct {
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

typedef HeifProbeNative =
    ffi.Int32 Function(
      ffi.Pointer<Utf8> path,
      ffi.Pointer<ffi.Uint32> width,
      ffi.Pointer<ffi.Uint32> height,
      ffi.Pointer<ffi.Int32> orientation,
    );
typedef HeifProbeDart =
    int Function(
      ffi.Pointer<Utf8> path,
      ffi.Pointer<ffi.Uint32> width,
      ffi.Pointer<ffi.Uint32> height,
      ffi.Pointer<ffi.Int32> orientation,
    );

typedef HeifDecodeRgbaNative =
    ffi.Int32 Function(
      ffi.Pointer<Utf8> path,
      ffi.Int32 maxDim,
      ffi.Pointer<HeifResult> out,
    );
typedef HeifDecodeRgbaDart =
    int Function(
      ffi.Pointer<Utf8> path,
      int maxDim,
      ffi.Pointer<HeifResult> out,
    );

typedef HeifReleaseNative = ffi.Void Function(ffi.Pointer<HeifResult> result);
typedef HeifReleaseDart = void Function(ffi.Pointer<HeifResult> result);

/// Guarded bindings to the HEIF entry points of `dng_decoder_native`.
///
/// Every lookup is inside a `try`/`catch`, exactly as [DngNativeBindings] does
/// for its additive symbols: a dylib built with `-DDNG_ENABLE_HEIF=OFF`, or
/// any older drop, must leave [available] false rather than throw during
/// construction and kill ALL decoding rather than just HEIC.
class HeifNativeBindings {
  HeifNativeBindings._(this._probe, this._decode, this._release);

  final HeifProbeDart? _probe;
  final HeifDecodeRgbaDart? _decode;
  final HeifReleaseDart? _release;

  bool get available => _probe != null && _decode != null && _release != null;

  HeifProbeDart get probe => _probe!;
  HeifDecodeRgbaDart get decode => _decode!;
  HeifReleaseDart get release => _release!;

  /// Loads from the SAME library [DngNativeBindings] resolves, so there is one
  /// dylib search order in this package rather than two that can disagree
  /// about which copy got loaded.
  factory HeifNativeBindings.fromLibrary(ffi.DynamicLibrary lib) {
    HeifProbeDart? probe;
    HeifDecodeRgbaDart? decode;
    HeifReleaseDart? release;
    try {
      probe = lib.lookupFunction<HeifProbeNative, HeifProbeDart>('heif_probe');
      decode = lib.lookupFunction<HeifDecodeRgbaNative, HeifDecodeRgbaDart>(
        'heif_decode_rgba',
      );
      release = lib.lookupFunction<HeifReleaseNative, HeifReleaseDart>(
        'heif_release',
      );
    } catch (_) {
      // Partial success is treated as absence on purpose: two of three symbols
      // is a broken drop, and calling into it would be worse than degrading.
      probe = null;
      decode = null;
      release = null;
    }
    return HeifNativeBindings._(probe, decode, release);
  }
}
