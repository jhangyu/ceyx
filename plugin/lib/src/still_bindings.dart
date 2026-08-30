import 'dart:ffi' as ffi;

import 'package:ffi/ffi.dart';

import 'encode_options.dart';

/// FFI bindings for the still-image decode surface added in
/// `native/include/ceyx_still_api.h` (2026-08-30 codec expansion). Mirrors
/// [HeifNativeBindings]'s guarded-lookup style: these symbols are additive, so
/// a dylib built without them (or predating this drop) must leave [available]
/// false rather than throw during construction and take down decoding with
/// it.

typedef CeyxStillDecodeSupportsNative = ffi.Int32 Function(ffi.Int32 format);
typedef CeyxStillDecodeSupportsDart = int Function(int format);

typedef CeyxStillProbeNative =
    ffi.Int32 Function(
      ffi.Pointer<Utf8> path,
      ffi.Int32 formatHint,
      ffi.Pointer<ffi.Uint32> width,
      ffi.Pointer<ffi.Uint32> height,
      ffi.Pointer<ffi.Int32> orientation,
    );
typedef CeyxStillProbeDart =
    int Function(
      ffi.Pointer<Utf8> path,
      int formatHint,
      ffi.Pointer<ffi.Uint32> width,
      ffi.Pointer<ffi.Uint32> height,
      ffi.Pointer<ffi.Int32> orientation,
    );

typedef CeyxStillDecodeRgbaNative =
    ffi.Int32 Function(
      ffi.Pointer<Utf8> path,
      ffi.Int32 formatHint,
      ffi.Int32 maxDim,
      ffi.Pointer<CeyxStillResult> out,
    );
typedef CeyxStillDecodeRgbaDart =
    int Function(
      ffi.Pointer<Utf8> path,
      int formatHint,
      int maxDim,
      ffi.Pointer<CeyxStillResult> out,
    );

typedef CeyxStillReleaseNative =
    ffi.Void Function(ffi.Pointer<CeyxStillResult> result);
typedef CeyxStillReleaseDart = void Function(
  ffi.Pointer<CeyxStillResult> result,
);

typedef CeyxStillErrorNameNative = ffi.Pointer<Utf8> Function(ffi.Int32 code);
typedef CeyxStillErrorNameDart = ffi.Pointer<Utf8> Function(int code);

/// Guarded bindings to the still-decode entry points of
/// `dng_decoder_native`.
///
/// Every lookup is inside a `try`/`catch`, exactly as [HeifNativeBindings]
/// does for its additive symbols: a dylib predating this drop, or built
/// without the still-decode surface, must leave [available] false rather
/// than throw during construction and kill ALL decoding rather than just
/// this route. `available` is true only when all five symbols resolve.
class CeyxStillBindings {
  CeyxStillBindings._(
    this._supports,
    this._probe,
    this._decode,
    this._release,
    this._errorName,
  );

  final CeyxStillDecodeSupportsDart? _supports;
  final CeyxStillProbeDart? _probe;
  final CeyxStillDecodeRgbaDart? _decode;
  final CeyxStillReleaseDart? _release;
  final CeyxStillErrorNameDart? _errorName;

  bool get available =>
      _supports != null &&
      _probe != null &&
      _decode != null &&
      _release != null &&
      _errorName != null;

  CeyxStillDecodeSupportsDart get supports => _supports!;
  CeyxStillProbeDart get probe => _probe!;
  CeyxStillDecodeRgbaDart get decode => _decode!;
  CeyxStillReleaseDart get release => _release!;

  /// Human-readable name for a `CeyxStillErrorCode` value, matching the
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
  factory CeyxStillBindings.fromLibrary(ffi.DynamicLibrary lib) {
    CeyxStillDecodeSupportsDart? supports;
    CeyxStillProbeDart? probe;
    CeyxStillDecodeRgbaDart? decode;
    CeyxStillReleaseDart? release;
    CeyxStillErrorNameDart? errorName;
    try {
      supports = lib
          .lookupFunction<
            CeyxStillDecodeSupportsNative,
            CeyxStillDecodeSupportsDart
          >('ceyx_still_decode_supports');
      probe = lib.lookupFunction<CeyxStillProbeNative, CeyxStillProbeDart>(
        'ceyx_still_probe',
      );
      decode = lib
          .lookupFunction<CeyxStillDecodeRgbaNative, CeyxStillDecodeRgbaDart>(
            'ceyx_still_decode_rgba',
          );
      release = lib
          .lookupFunction<CeyxStillReleaseNative, CeyxStillReleaseDart>(
            'ceyx_still_release',
          );
      errorName = lib
          .lookupFunction<CeyxStillErrorNameNative, CeyxStillErrorNameDart>(
            'ceyx_still_error_name',
          );
    } catch (_) {
      // Partial success is treated as absence on purpose: some symbols
      // present is a broken drop, and calling into it would be worse than
      // degrading.
      supports = null;
      probe = null;
      decode = null;
      release = null;
      errorName = null;
    }
    return CeyxStillBindings._(supports, probe, decode, release, errorName);
  }
}
