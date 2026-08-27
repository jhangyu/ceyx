import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'dng_bindings.dart';
import 'heif_bindings.dart';
import 'heif_error_codes.dart';

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

class _HeifWorkerResult {
  _HeifWorkerResult({
    required this.rgba,
    required this.width,
    required this.height,
    required this.orientation,
  });

  final TransferableTypedData rgba;
  final int width;
  final int height;
  final int orientation;

  HeifImage toImage() => HeifImage(
    rgba: rgba.materialize().asUint8List(),
    width: width,
    height: height,
    orientation: orientation,
  );
}

/// High-level HEIC/HEIF decoding service.
///
/// Shape deliberately mirrors [DngDecoderService]: the same dylib search order
/// (it reuses [DngNativeBindings]' loader), the same worker-isolate discipline
/// (native bytes are copied into Dart-owned memory inside the worker, and only
/// [TransferableTypedData] crosses the isolate boundary — no native pointer
/// ever does), and the same guarded-symbol degradation.
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
    try {
      final dng = _libraryPath == null
          ? DngNativeBindings.load()
          : DngNativeBindings.fromPath(_libraryPath);
      _bindings = HeifNativeBindings.fromLibrary(dng.library);
    } catch (_) {
      _bindings = null;
    }
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
    final result = await Isolate.run(
      () => _decodeInIsolate(path, libraryPath, requested),
    );
    return result.toImage();
  }

  /// Static so [Isolate.run] cannot capture parent-isolate state.
  static HeifProbeResult? _probeInIsolate(String path, String? libraryPath) {
    final service = HeifDecoderService(libraryPath: libraryPath).._initialize();
    final bindings = service._bindings;
    if (bindings == null || !bindings.available) return null;

    final pathPtr = path.toNativeUtf8();
    final width = calloc<Uint32>();
    final height = calloc<Uint32>();
    final orientation = calloc<Int32>();
    try {
      final rc = bindings.probe(pathPtr, width, height, orientation);
      if (rc != HeifErrorCode.success) return null;
      if (width.value == 0 || height.value == 0) return null;
      return (
        width: width.value,
        height: height.value,
        orientation: orientation.value,
      );
    } finally {
      malloc.free(pathPtr);
      calloc.free(width);
      calloc.free(height);
      calloc.free(orientation);
    }
  }

  static _HeifWorkerResult _decodeInIsolate(
    String path,
    String? libraryPath,
    int maxDim,
  ) {
    final service = HeifDecoderService(libraryPath: libraryPath).._initialize();
    final bindings = service._bindings;
    if (bindings == null || !bindings.available) {
      throw HeifUnavailableException(path);
    }

    final pathPtr = path.toNativeUtf8();
    final out = calloc<HeifResult>();
    try {
      final rc = bindings.decode(pathPtr, maxDim, out);
      final result = out.ref;
      if (rc != HeifErrorCode.success) {
        throw HeifDecodeException(
          rc,
          HeifErrorCode.name(rc),
          'native heif_decode_rgba failed for $path',
        );
      }
      if (result.rgba == nullptr || result.rgbaLen <= 0) {
        throw HeifDecodeException(
          HeifErrorCode.allocationFailed,
          HeifErrorCode.name(HeifErrorCode.allocationFailed),
          'RGBA buffer is null despite kHeifSuccess',
        );
      }
      final expected = result.width * result.height * 4;
      if (result.rgbaLen != expected) {
        // Native already checks this; re-checking here means a future ABI drift
        // surfaces as a typed exception rather than as a torn image.
        throw HeifDecodeException(
          HeifErrorCode.metadataInvalid,
          HeifErrorCode.name(HeifErrorCode.metadataInvalid),
          'rgba_len=${result.rgbaLen} but width*height*4=$expected',
        );
      }
      // Copy into Dart-owned bytes: TransferableTypedData cannot carry a
      // native-backed typed list across an isolate boundary safely.
      final copy = Uint8List.fromList(
        result.rgba.asTypedList(result.rgbaLen),
      );
      return _HeifWorkerResult(
        rgba: TransferableTypedData.fromList([copy]),
        width: result.width,
        height: result.height,
        orientation: result.orientation,
      );
    } finally {
      // heif_release frees the buffer and zeroes the struct; calloc.free then
      // releases the caller-owned struct itself. Order matters: freeing the
      // struct first would leak the buffer.
      if (bindings.available) bindings.release(out);
      calloc.free(out);
      malloc.free(pathPtr);
    }
  }
}
