/// Dart mirror of `CeyxImageFormat` (native/include/ceyx_encode_api.h).
/// Values are append-only and are never reused.
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
