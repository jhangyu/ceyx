/// Extension-based decode routing for the Dart layer.
///
/// IMPORTANT — this list is NOT a mirror of a native list. The native router
/// `dng_processor/native/src/raw_file_router.cpp` decides DNG vs generic by
/// probing magic bytes (`raw_probe_bytes`), and holds no extension table at
/// all. This const is the app-level PRE-FILTER: it decides which FFI entry
/// point Dart calls and which files the picker offers. The authoritative
/// route decision stays native.
///
/// Whenever a new container is added to the native generic route, add its
/// extension here too — otherwise the format decodes natively but is
/// unreachable from the app.
library;

/// Generic-RAW extensions handled by `raw_decode_and_process`.
const List<String> kRawExtensions = <String>[
  'arw',
  'cr3',
  'nef',
  'raf',
  'rw2',
  'orf',
  'pef',
  'srw',
];

/// Everything the decoder service accepts: DNG plus the generic RAW list.
const List<String> kSupportedDecodeExtensions = <String>[
  'dng',
  ...kRawExtensions,
];

/// Which native entry point a path resolves to.
enum DecodeRoute {
  /// `dng_decode_and_process` / `dng_decode_and_process_sized`.
  dng,

  /// `raw_decode_and_process`.
  raw,

  /// No native entry point accepts this file.
  unsupported,
}

/// Lowercased extension of [filePath] without the leading dot.
///
/// Returns `''` when the last path segment has no dot, ends with a dot, or is
/// empty. Both `/` and `\` are treated as separators so Windows paths behave.
String decodeExtensionOf(String filePath) {
  var start = 0;
  for (var i = filePath.length - 1; i >= 0; i--) {
    final c = filePath[i];
    if (c == '/' || c == '\\') {
      start = i + 1;
      break;
    }
  }
  final basename = filePath.substring(start);
  final dot = basename.lastIndexOf('.');
  if (dot < 0 || dot == basename.length - 1) return '';
  return basename.substring(dot + 1).toLowerCase();
}

/// Classify [filePath] by extension. Never throws.
DecodeRoute decodeRouteForPath(String filePath) {
  final ext = decodeExtensionOf(filePath);
  if (ext == 'dng') return DecodeRoute.dng;
  if (kRawExtensions.contains(ext)) return DecodeRoute.raw;
  return DecodeRoute.unsupported;
}
