import 'dart:ffi';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';

import 'dng_bindings.dart';

/*
---
file_summary: "提供 Flutter 與 Native 之間的 FFI 解碼服務封裝與記憶體管理"
modules:
  - name: "DngImage"
    description: "解碼後的影像資料與耗時紀錄容器"
    lines: "8-34"
  - name: "Exceptions"
    description: "解碼錯誤定義"
    lines: "37-45"
  - name: "DngDecoderService"
    description: "Native 方法調用，處理 Dart 端 ByteBuffer 複製與記憶體釋放"
    lines: "47-137"
---
*/

/// Decoded DNG image result with automatic native memory management.
class DngImage implements Finalizable {
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
  late final DngNativeBindings _bindings;
  bool _initialized = false;

  DngDecoderService();

  /// Initialize the service by loading the native library.
  void initialize() {
    if (_initialized) return;
    _bindings = DngNativeBindings.load();
    _initialized = true;
  }

  /// Extracts the embedded JPEG preview from the DNG file.
  /// Returns null if extraction fails.
  Uint8List? getPreviewJpeg(String filePath) {
    if (!_initialized) {
      initialize();
    }

    final pathPtr = filePath.toNativeUtf8();
    final outBuffer = calloc<Pointer<Uint8>>();
    final outSize = calloc<Int32>();

    try {
      final result = _bindings.extractPreviewJpeg(
        pathPtr.cast(),
        outBuffer.cast(),
        outSize.cast(),
      );

      if (result == 0 && outBuffer.value != nullptr && outSize.value > 0) {
        final bufferPtr = outBuffer.value;
        final size = outSize.value;

        // Copy bytes to a Dart Uint8List
        final bytes = Uint8List.fromList(bufferPtr.asTypedList(size));

        // Free the native buffer
        _bindings.freeBuffer(bufferPtr.cast());

        return bytes;
      }
      return null;
    } finally {
      malloc.free(pathPtr);
      calloc.free(outBuffer);
      calloc.free(outSize);
    }
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
        throw DngDecodeException(
          -1,
          'RGBA buffer is null despite success code',
        );
      }

      final width = result.width;
      final height = result.height;
      final bufferSize = width * height * 4;

      // Phase 6.4 Zero-copy: Instead of copying, we view the native memory directly.
      final rgbaData = result.rgbaData.asTypedList(bufferSize);

      // Create the DngImage container which wraps the zero-copy list
      final image = DngImage(
        rgbaData: rgbaData,
        width: width,
        height: height,
        decodeMs: result.decodeMs,
        processMs: result.processMs,
      );

      // Attach the NativeFinalizer to the Dart list object.
      // When the `image` object is garbage collected, Dart will automatically
      // call `dng_free_halide_buffer` with the native pointer.
      final finalizer = NativeFinalizer(
        _bindings.dngFreeHalideBufferPtr.cast(),
      );
      finalizer.attach(image, result.rgbaData.cast(), detach: image);

      // Since we handed ownership of `rgbaData` over to the Finalizer,
      // we must set it to null in the result struct so `dng_free_result`
      // does not delete it when freeing the struct itself!
      result.rgbaData = nullptr;

      return image;
    } finally {
      // Always free the native result struct (which no longer owns the rgbaData on success)
      if (resultPtr != nullptr) {
        _bindings.dngFreeResult(resultPtr);
      }
      malloc.free(pathPtr);
    }
  }
}
