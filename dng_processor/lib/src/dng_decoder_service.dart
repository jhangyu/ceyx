import 'dart:ffi';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';

import 'dng_bindings.dart';

/// Decoded DNG image result with automatic native memory management.
class DngImage {
  /// RGBA pixel data (width * height * 4 bytes)
  final Uint8List rgbaData;

  /// Image width in pixels
  final int width;

  /// Image height in pixels
  final int height;

  /// DNG decompression time in milliseconds
  final double decodeMs;

  /// Halide pipeline processing time in milliseconds
  final double processMs;

  DngImage({
    required this.rgbaData,
    required this.width,
    required this.height,
    required this.decodeMs,
    required this.processMs,
  });

  /// Total processing time
  double get totalMs => decodeMs + processMs;
}

/// Error thrown when DNG decoding fails
class DngDecodeException implements Exception {
  final int errorCode;
  final String message;

  DngDecodeException(this.errorCode, this.message);

  @override
  String toString() => 'DngDecodeException($errorCode): $message';
}

/// High-level DNG decoding service.
///
/// Uses dart:ffi to call native C++ code (DNG SDK + Halide pipeline).
/// Memory is managed via Dart-side copy of the RGBA buffer, with the
/// native result freed immediately after copy.
class DngDecoderService {
  late final DngBindings _bindings;
  bool _initialized = false;

  DngDecoderService();

  /// Initialize the service by loading the native library.
  void initialize() {
    if (_initialized) return;
    _bindings = DngBindings.load();
    _initialized = true;
  }

  /// Decode a DNG file and return the processed RGBA image.
  ///
  /// The returned [DngImage] owns a Dart-side copy of the pixel data,
  /// so the native buffer is freed immediately. No manual cleanup needed.
  ///
  /// Throws [DngDecodeException] on failure.
  DngImage decode(String filePath) {
    if (!_initialized) {
      initialize();
    }

    final pathPtr = filePath.toNativeUtf8();
    Pointer<DngResult> resultPtr = nullptr;

    try {
      resultPtr = _bindings.dngDecodeAndProcess(pathPtr.cast());

      if (resultPtr == nullptr) {
        throw DngDecodeException(-1, 'Native function returned null');
      }

      final result = resultPtr.ref;

      if (result.errorCode != 0) {
        final code = result.errorCode;
        String msg;
        switch (code) {
          case -1:
            msg = 'File not found';
          case -2:
            msg = 'DNG parse error';
          case -3:
            msg = 'Unsupported format';
          case -4:
            msg = 'Memory allocation error';
          case -10:
            msg = 'Halide pipeline error';
          default:
            msg = 'Unknown error (code: $code)';
        }
        throw DngDecodeException(code, msg);
      }

      if (result.rgbaData == nullptr) {
        throw DngDecodeException(-1, 'RGBA buffer is null despite success code');
      }

      final width = result.width;
      final height = result.height;
      final bufferSize = width * height * 4;

      // Copy pixel data to Dart-managed memory so we can free native side
      final rgbaData = Uint8List(bufferSize);
      final nativeBytes = result.rgbaData.asTypedList(bufferSize);
      rgbaData.setAll(0, nativeBytes);

      return DngImage(
        rgbaData: rgbaData,
        width: width,
        height: height,
        decodeMs: result.decodeMs,
        processMs: result.processMs,
      );
    } finally {
      // Always free the native result
      if (resultPtr != nullptr) {
        _bindings.dngFreeResult(resultPtr);
      }
      malloc.free(pathPtr);
    }
  }
}
