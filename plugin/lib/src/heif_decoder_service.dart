import 'dart:isolate';
import 'dart:typed_data';

import 'heif_bindings.dart';
import 'heif_error_codes.dart';
import 'still_worker.dart';

/// A decoded HEIC image: RGBA8 interleaved, Dart-owned.
class HeifImage {
  HeifImage({
    required this.rgba,
    required this.width,
    required this.height,
    required this.orientation,
  });

  /// RGBA8 interleaved, length == width * height * 4.
  final Uint8List rgba;
  final int width;
  final int height;

  /// Always 1 in phase 2: libheif applies the container's irot/imir transform
  /// during decode, so these pixels are display-ready. See `heif_api.h`.
  final int orientation;
}

/// Extent + orientation of a HEIC's primary item, read from metadata only.
typedef HeifProbeResult = ({int width, int height, int orientation});

/// High-level HEIC/HEIF decoding service.
///
/// Shape deliberately mirrors [DngDecoderService]: the same dylib search order
/// (it reuses `DngNativeBindings`' loader), the same worker-isolate discipline
/// (native bytes are copied into Dart-owned memory inside the worker, and only
/// [TransferableTypedData] crosses the isolate boundary — no native pointer
/// ever does), and the same guarded-symbol degradation. T8 (2026-10-02): the
/// worker mechanics shared with [CeyxStillDecoderService] live in
/// still_worker.dart; this class keeps the HEIF-specific contract — typed
/// exceptions on decode failure, null on probe failure.
class HeifDecoderService {
  HeifDecoderService({String? libraryPath}) : _libraryPath = libraryPath;

  final String? _libraryPath;
  HeifNativeBindings? _bindings;
  bool _initialized = false;

  /// Never throws: a missing dylib or a missing symbol both leave the service
  /// unavailable, because HEIC being undecodable must not break RAW decoding.
  void _initialize() {
    if (_initialized) return;
    _initialized = true;
    _bindings = loadStillRouteBindings(
      _libraryPath,
      HeifNativeBindings.fromLibrary,
    );
  }

  /// Whether this build of the native library exports the HEIF entry points.
  bool get heifAvailable {
    _initialize();
    return _bindings?.available ?? false;
  }

  /// Metadata-only probe of the primary item.
  ///
  /// Returns null — never throws — when the route is unavailable or the native
  /// side reports an error. Its caller is Halcyon's image loader, which is
  /// documented as never throwing, so null is the only usable failure channel.
  Future<HeifProbeResult?> probeOnWorker(String path) async {
    if (!heifAvailable) return null;
    final libraryPath = _libraryPath;
    try {
      return await Isolate.run(() => _probeInIsolate(path, libraryPath));
    } catch (_) {
      return null;
    }
  }

  /// Decodes the primary item on a worker isolate.
  ///
  /// [maxDim] caps the long edge; it is a request, not a guarantee — read back
  /// [HeifImage.width]/[HeifImage.height].
  ///
  /// Throws [HeifUnavailableException] when the route is absent and
  /// [HeifDecodeException] when the native side reports an error. Halcyon's
  /// dispatcher turns either into the uniform permanent miss.
  Future<HeifImage> decodeOnWorker(String path, {int? maxDim}) async {
    if (!heifAvailable) throw HeifUnavailableException(path);
    final libraryPath = _libraryPath;
    // Hoisted to locals before the closure: referencing a field would capture
    // `this`, and an initialized service holds a DynamicLibrary that
    // Isolate.run cannot send (the same trap DngDecoderService documents).
    final requested = (maxDim != null && maxDim > 0) ? maxDim : 0;
    final pixels = await Isolate.run(
      () => _decodeInIsolate(path, libraryPath, requested),
    );
    return HeifImage(
      rgba: pixels.rgba.materialize().asUint8List(),
      width: pixels.width,
      height: pixels.height,
      orientation: pixels.orientation,
    );
  }

  /// Static so [Isolate.run] cannot capture parent-isolate state.
  static HeifProbeResult? _probeInIsolate(String path, String? libraryPath) {
    final bindings = loadStillRouteBindings(
      libraryPath,
      HeifNativeBindings.fromLibrary,
    );
    if (bindings == null || !bindings.available) return null;
    return probeStillImageExtent(path, bindings.probe, HeifErrorCode.success);
  }

  /// Throws inside the worker, exactly as before T8, so [Isolate.run]
  /// rethrows the same exception types with the same code, name and message.
  static StillDecodePixels _decodeInIsolate(
    String path,
    String? libraryPath,
    int maxDim,
  ) {
    final bindings = loadStillRouteBindings(
      libraryPath,
      HeifNativeBindings.fromLibrary,
    );
    if (bindings == null || !bindings.available) {
      throw HeifUnavailableException(path);
    }
    final outcome = decodeStillImage(
      path,
      maxDim,
      (pathPointer, maxDimension, out) =>
          bindings.decode(pathPointer, maxDimension, out.cast<HeifResult>()),
      (out) => bindings.release(out.cast<HeifResult>()),
      HeifErrorCode.success,
    );
    switch (outcome) {
      case StillDecodePixels():
        return outcome;
      case StillDecodeNativeError(:final code):
        throw HeifDecodeException(
          code,
          HeifErrorCode.name(code),
          'native heif_decode_rgba failed for $path',
        );
      case StillDecodeNullBuffer():
        throw HeifDecodeException(
          HeifErrorCode.allocationFailed,
          HeifErrorCode.name(HeifErrorCode.allocationFailed),
          'RGBA buffer is null despite kHeifSuccess',
        );
      case StillDecodeLengthMismatch(:final rgbaLength, :final expectedLength):
        // Native already checks this; re-checking here means a future ABI
        // drift surfaces as a typed exception rather than as a torn image.
        throw HeifDecodeException(
          HeifErrorCode.metadataInvalid,
          HeifErrorCode.name(HeifErrorCode.metadataInvalid),
          'rgba_len=$rgbaLength but width*height*4=$expectedLength',
        );
    }
  }
}
