/// Dart mirror of `CeyxImageFormat` (native/include/ceyx_encode_api.h).
/// Values are append-only and are never reused.
///
/// This is the container format for ENCODE; it is NOT the decode pixel
/// layout, which is [CeyxOutputFormat] below. The two live one screen apart
/// and are both `Ceyx*Format`, so each names the other.
enum CeyxImageFormat {
  unknown(0),
  jpeg(1),
  webp(2),
  heic(3),
  avif(4),
  jxl(5);

  const CeyxImageFormat(this.value);

  /// The int32 the C ABI expects.
  final int value;
}

/// Dart mirror of `enum CeyxOutputFormat` (native/include/raw_ffi_api.h).
///
/// The pixel layout a DECODE produces. It is NOT the encode container
/// format, which is [CeyxImageFormat] above.
///
/// FROZEN by T12.0 (mem8 v3 campaign, Phase P0). Four tasks consume this
/// shape — T12 (kernel arms), T13 (converter), T14 (binding), T15a (Halcyon
/// seam); a consumer that needs it changed stops and reports rather than
/// editing it.
///
/// ADDITIVE: [rgba8] is the historical behaviour and remains ceyx's default
/// (R-B). Halcyon overrides the default to [yuv420] on its own side
/// (R-A/R-L); ceyx does not change its default for other consumers.
///
/// Values are append-only and are never renumbered — they cross the FFI
/// boundary as the `int32_t output_format` argument.
enum CeyxOutputFormat {
  /// 4 B/px interleaved RGBA8. `kCeyxOutputFormatRgba8`.
  rgba8(0),

  /// Planar 4:2:0, 1.5 B/px average. Single contiguous allocation, plane
  /// order Y, Cb, Cr, each plane tightly packed (row stride == plane width)
  /// with no inter-plane padding. Chroma planes are
  /// `ceil(w/2) x ceil(h/2)`. `kCeyxOutputFormatYuv420`.
  yuv420(1);

  const CeyxOutputFormat(this.value);

  /// The int32 the C ABI expects.
  final int value;
}

/// Bytes a decode of [width] x [height] in [format] occupies.
///
/// Dart mirror of `ceyx_output_format_byte_count` (raw_ffi_api.h) and part of
/// T12.0's frozen contract, not a convenience: T15's slot sizing and T14's
/// binding call THIS function. A caller that open-codes the arithmetic is a
/// defect by contract.
///
/// * [CeyxOutputFormat.rgba8]: `w*h*4`
/// * [CeyxOutputFormat.yuv420]: `w*h + 2*(ceil(w/2) * ceil(h/2))`
///
/// Throws [ArgumentError] for a non-positive extent, so an under-allocation
/// can never be produced silently — under yuv420 an under-allocation is a
/// heap overrun, not a miscount.
int ceyxOutputFormatByteCount(CeyxOutputFormat format, int width, int height) {
  if (width <= 0 || height <= 0) {
    throw ArgumentError('extent must be positive, got ${width}x$height');
  }
  switch (format) {
    case CeyxOutputFormat.rgba8:
      return width * height * 4;
    case CeyxOutputFormat.yuv420:
      final chromaWidth = (width + 1) ~/ 2;
      final chromaHeight = (height + 1) ~/ 2;
      return width * height + 2 * (chromaWidth * chromaHeight);
  }
}

/// Thrown when the LOADED native library cannot service the requested output
/// format — R-J's hard typed failure. Never a null, never a silent rgba8
/// fallback: the caller would interpret 4 B/px bytes as 1.5 B/px.
///
/// FROZEN at EXACTLY THREE fields (T12.0.2). A fourth — the expected pin
/// digest — is ruled out: it is already authoritative in Halcyon's
/// `scripts/ceyx_release_pin.json`, and copying it here creates a second
/// source of truth that can drift. The exception's job is to report what WAS
/// loaded, not to restate what should have been.
///
/// [libraryPath] is not optional: native libraries are placed per-machine by
/// `build_apps.py` from the pin and are deliberately not in version control,
/// so no reader can infer which file was opened — and a stale file at a path
/// nobody checked is exactly the condition being diagnosed.
///
/// `plugin/test/format_unsupported_exception_shape_test.dart` asserts the
/// field set and fails if one is added or removed.
class CeyxFormatUnsupportedException implements Exception {
  const CeyxFormatUnsupportedException({
    required this.format,
    required this.missingSymbol,
    required this.libraryPath,
  });

  /// The output format the caller asked for.
  final CeyxOutputFormat format;

  /// The native entry point the loaded library does not export.
  final String missingSymbol;

  /// Absolute path of the native library that was ACTUALLY loaded.
  final String libraryPath;

  @override
  String toString() =>
      'CeyxFormatUnsupportedException: the loaded native library cannot '
      'produce ${format.name} — it does not export "$missingSymbol". '
      'Loaded library: $libraryPath. Re-run the pinned build to place a '
      'current library; do NOT work around this by requesting rgba8.';
}
