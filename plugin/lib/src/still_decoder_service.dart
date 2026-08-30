import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'dng_bindings.dart';
import 'encode_options.dart';
import 'still_bindings.dart';
import 'still_error_codes.dart';

/// A decoded still image (HEIC/AVIF/WebP/JXL/JPEG): RGBA8 interleaved,
/// Dart-owned.
class CeyxStillImage {
  CeyxStillImage({
    required this.rgba,
    required this.width,
    required this.height,
    required this.orientation,
  });

  /// RGBA8 interleaved, length == width * height * 4.
  final Uint8List rgba;
  final int width;
  final int height;

  /// Always 1 -- see the orientation contract in `ceyx_still_api.h`: the
  /// decoder applies container transforms during decode, so these pixels are
  /// display-ready.
  final int orientation;
}

/// Extent + orientation of a still image's primary item, read from metadata
/// only.
typedef CeyxStillProbe = ({int width, int height, int orientation});

class _CeyxStillWorkerResult {
  _CeyxStillWorkerResult({
    required this.rgba,
    required this.width,
    required this.height,
    required this.orientation,
  });

  final TransferableTypedData rgba;
  final int width;
  final int height;
  final int orientation;

  CeyxStillImage toImage() => CeyxStillImage(
    rgba: rgba.materialize().asUint8List(),
    width: width,
    height: height,
    orientation: orientation,
  );
}

/// High-level generic still-image decoding service.
///
/// Shape deliberately mirrors [HeifDecoderService]: the same dylib search
/// order (it reuses [DngNativeBindings]' loader), the same worker-isolate
/// discipline (native bytes are copied into Dart-owned memory inside the
/// worker, and only [TransferableTypedData] crosses the isolate boundary --
/// no native pointer ever does), and the same guarded-symbol degradation.
class CeyxStillDecoderService {
  CeyxStillDecoderService({String? libraryPath}) : _libraryPath = libraryPath;

  final String? _libraryPath;
  CeyxStillBindings? _bindings;
  bool _initialized = false;

  /// Never throws: a missing dylib or a missing symbol both leave the
  /// service unavailable.
  void _initialize() {
    if (_initialized) return;
    _initialized = true;
    try {
      final dng = _libraryPath == null
          ? DngNativeBindings.load()
          : DngNativeBindings.fromPath(_libraryPath);
      _bindings = CeyxStillBindings.fromLibrary(dng.library);
    } catch (_) {
      _bindings = null;
    }
  }

  /// Whether this build of the native library exports the generic
  /// still-decode entry points.
  bool get stillDecodeAvailable {
    _initialize();
    return _bindings?.available ?? false;
  }

  /// Metadata-only probe of the primary item.
  ///
  /// Returns null -- never throws -- when the route is unavailable or the
  /// native side reports an error.
  Future<CeyxStillProbe?> probeOnWorker(
    String path, {
    int formatHint = 0,
  }) async {
    if (!stillDecodeAvailable) return null;
    final libraryPath = _libraryPath;
    try {
      return await Isolate.run(
        () => _probeInIsolate(path, formatHint, libraryPath),
      );
    } catch (_) {
      return null;
    }
  }

  /// Decodes the primary/first frame on a worker isolate.
  ///
  /// [maxDim] caps the long edge; it is a request, not a guarantee -- read
  /// back [CeyxStillImage.width]/[CeyxStillImage.height].
  ///
  /// Returns null when the route is unavailable or the native side reports
  /// an error, mirroring [HeifDecoderService]'s null-on-failure contract for
  /// its caller (Halcyon's image loader never throws).
  Future<CeyxStillImage?> decodeOnWorker(
    String path, {
    int maxDim = 0,
    int formatHint = 0,
  }) async {
    if (!stillDecodeAvailable) return null;
    final libraryPath = _libraryPath;
    final requested = maxDim > 0 ? maxDim : 0;
    try {
      final result = await Isolate.run(
        () => _decodeInIsolate(path, formatHint, libraryPath, requested),
      );
      return result?.toImage();
    } catch (_) {
      return null;
    }
  }

  /// Static so [Isolate.run] cannot capture parent-isolate state.
  static CeyxStillProbe? _probeInIsolate(
    String path,
    int formatHint,
    String? libraryPath,
  ) {
    final service = CeyxStillDecoderService(
      libraryPath: libraryPath,
    ).._initialize();
    final bindings = service._bindings;
    if (bindings == null || !bindings.available) return null;

    final pathPtr = path.toNativeUtf8();
    final width = calloc<Uint32>();
    final height = calloc<Uint32>();
    final orientation = calloc<Int32>();
    try {
      final rc = bindings.probe(pathPtr, formatHint, width, height, orientation);
      if (rc != CeyxStillErrorCode.success) return null;
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

  static _CeyxStillWorkerResult? _decodeInIsolate(
    String path,
    int formatHint,
    String? libraryPath,
    int maxDim,
  ) {
    final service = CeyxStillDecoderService(
      libraryPath: libraryPath,
    ).._initialize();
    final bindings = service._bindings;
    if (bindings == null || !bindings.available) return null;

    final pathPtr = path.toNativeUtf8();
    final out = calloc<CeyxStillResult>();
    try {
      final rc = bindings.decode(pathPtr, formatHint, maxDim, out);
      final result = out.ref;
      if (rc != CeyxStillErrorCode.success) return null;
      if (result.rgba == nullptr || result.rgbaLen <= 0) return null;
      final expected = result.width * result.height * 4;
      if (result.rgbaLen != expected) return null;
      // Copy into Dart-owned bytes: TransferableTypedData cannot carry a
      // native-backed typed list across an isolate boundary safely.
      final copy = Uint8List.fromList(
        result.rgba.asTypedList(result.rgbaLen),
      );
      return _CeyxStillWorkerResult(
        rgba: TransferableTypedData.fromList([copy]),
        width: result.width,
        height: result.height,
        orientation: result.orientation,
      );
    } finally {
      // ceyx_still_release frees the buffer and zeroes the struct;
      // calloc.free then releases the caller-owned struct itself. Order
      // matters: freeing the struct first would leak the buffer.
      if (bindings.available) bindings.release(out);
      calloc.free(out);
      malloc.free(pathPtr);
    }
  }
}
