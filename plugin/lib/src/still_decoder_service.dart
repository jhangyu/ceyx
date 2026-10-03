import 'dart:isolate';
import 'dart:typed_data';

import 'still_bindings.dart';
import 'gpu_shutdown.dart';
import 'still_error_codes.dart';
import 'still_worker.dart';

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

/// High-level generic still-image decoding service.
///
/// Shape deliberately mirrors [HeifDecoderService]: the same dylib search
/// order (it reuses `DngNativeBindings`' loader), the same worker-isolate
/// discipline (native bytes are copied into Dart-owned memory inside the
/// worker, and only [TransferableTypedData] crosses the isolate boundary --
/// no native pointer ever does), and the same guarded-symbol degradation.
/// T8 (2026-10-02): those shared mechanics live in still_worker.dart; this
/// class keeps its own contract -- null on every failure.
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
    _bindings = loadStillRouteBindings(
      _libraryPath,
      CeyxStillBindings.fromLibrary,
    );
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
    CeyxGpuShutdown.guardWork('decodeOnWorker');
    if (!stillDecodeAvailable) return null;
    final libraryPath = _libraryPath;
    final requested = maxDim > 0 ? maxDim : 0;
    try {
      final pixels = await Isolate.run(
        () => _decodeInIsolate(path, formatHint, libraryPath, requested),
      );
      if (pixels == null) return null;
      return CeyxStillImage(
        rgba: pixels.rgba.materialize().asUint8List(),
        width: pixels.width,
        height: pixels.height,
        orientation: pixels.orientation,
      );
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
    final bindings = loadStillRouteBindings(
      libraryPath,
      CeyxStillBindings.fromLibrary,
    );
    if (bindings == null || !bindings.available) return null;
    return probeStillImageExtent(
      path,
      (pathPointer, width, height, orientation) =>
          bindings.probe(pathPointer, formatHint, width, height, orientation),
      CeyxStillErrorCode.success,
    );
  }

  static StillDecodePixels? _decodeInIsolate(
    String path,
    int formatHint,
    String? libraryPath,
    int maxDim,
  ) {
    final bindings = loadStillRouteBindings(
      libraryPath,
      CeyxStillBindings.fromLibrary,
    );
    if (bindings == null || !bindings.available) return null;
    final outcome = decodeStillImage(
      path,
      maxDim,
      (pathPointer, maxDimension, out) =>
          bindings.decode(pathPointer, formatHint, maxDimension, out),
      bindings.release,
      CeyxStillErrorCode.success,
    );
    return outcome is StillDecodePixels ? outcome : null;
  }
}
