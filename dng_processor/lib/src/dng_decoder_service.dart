import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';

import 'dng_bindings.dart';

/*
---
file_summary: "提供 Flutter 與 Native 之間的 FFI 解碼服務封裝與記憶體管理"
modules:
  - name: "DngImage"
    description: "解碼後的影像資料與耗時紀錄容器"
    lines: "27-54"
  - name: "Exceptions"
    description: "解碼錯誤定義"
    lines: "56-65"
  - name: "Worker Transfer"
    description: "worker isolate 回傳 Dart-owned RGBA bytes 的容器"
    lines: "67-91"
  - name: "DngDecoderService"
    description: "Native 方法調用，處理 Dart 端 ByteBuffer 複製與記憶體釋放；包含 getPreviewJpegOnWorker（preview isolate 化）"
    lines: "93-377"
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

class _DecodeWorkerResult {
  final TransferableTypedData rgbaData;
  final int width;
  final int height;
  final double decodeMs;
  final double processMs;

  _DecodeWorkerResult({
    required this.rgbaData,
    required this.width,
    required this.height,
    required this.decodeMs,
    required this.processMs,
  });

  DngImage toImage() {
    return DngImage(
      rgbaData: rgbaData.materialize().asUint8List(),
      width: width,
      height: height,
      decodeMs: decodeMs,
      processMs: processMs,
    );
  }
}

/// High-level DNG decoding service.
///
/// Uses dart:ffi to call native C++ code (DNG SDK + Halide pipeline).
/// Memory is managed by transferring the native RGBA buffer to a Dart
/// NativeFinalizer, while the surrounding result struct is freed immediately.
class DngDecoderService {
  late final DngNativeBindings _bindings;
  // Service-owned finalizer, lazily created on the first _decodeZeroCopy call.
  // Kept as a field (not a local) so its lifetime matches the service — if it
  // were per-call the GC could collect it before the attached DngImage, silently
  // leaking the native buffer (Gotcha #45, memory.md).
  // Worker-path callers (_decodeToTransferable) never attach to this finalizer;
  // they rely on dng_free_result() in their finally block to free rgba_data,
  // so we do NOT create the finalizer eagerly in initialize() — that would waste
  // a handle in every worker isolate that never uses zero-copy.
  NativeFinalizer? _rgbaFinalizer;
  bool _initialized = false;

  DngDecoderService();

  /// Initialize the service by loading the native library.
  void initialize() {
    if (_initialized) return;
    _bindings = DngNativeBindings.load();
    // _rgbaFinalizer is intentionally NOT created here; it is lazily created in
    // _decodeZeroCopy so that worker-isolate services (which only call
    // _decodeToTransferable) do not allocate a finalizer they will never use.
    _initialized = true;
  }

  /// Warm native resources for the common 24MP decode path off the UI isolate.
  Future<void> warmupForSize({int width = 6000, int height = 4000}) async {
    final result = await Isolate.run(() {
      final bindings = DngNativeBindings.load();
      return bindings.dngDecoderWarmupForSize(width, height);
    });
    if (result != 0) {
      throw DngDecodeException(result, 'Native warmup failed');
    }
  }

  /// Decode on a worker isolate so the UI isolate can keep painting preview
  /// and progress state while native full RAW processing runs.
  ///
  /// The worker intentionally does not send the zero-copy external RGBA view
  /// across isolate boundaries. It copies the native buffer into Dart-owned
  /// bytes, transfers those bytes with [TransferableTypedData], then frees the
  /// native result inside the worker isolate.
  Future<DngImage> decodeOnWorker(String filePath) async {
    final result = await Isolate.run(() => _decodeFileToTransferable(filePath));
    return result.toImage();
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

  /// Extracts the embedded JPEG preview from the DNG file on a worker isolate,
  /// so the UI isolate is not blocked during the native FFI call.
  ///
  /// Internally delegates to [getPreviewJpeg] running inside a fresh
  /// worker isolate. The resulting bytes are already Dart-owned [Uint8List]
  /// when returned — no native pointer crosses isolate boundaries.
  ///
  /// Returns null if extraction fails.
  Future<Uint8List?> getPreviewJpegOnWorker(String filePath) {
    return Isolate.run(() => _extractPreviewJpegOnWorker(filePath));
  }

  /// Worker-isolate entry point for preview JPEG extraction.
  /// Creates a fresh [DngDecoderService], calls the synchronous
  /// [getPreviewJpeg] (which already copies native bytes into Dart-owned
  /// [Uint8List] before returning), and returns those bytes.
  /// Static so [Isolate.run] does not accidentally capture parent-isolate state.
  static Uint8List? _extractPreviewJpegOnWorker(String filePath) {
    final service = DngDecoderService()..initialize();
    return service.getPreviewJpeg(filePath);
  }

  /// Decode a DNG file and return the processed RGBA image.
  ///
  /// The returned [DngImage] exposes `rgbaData` as a zero-copy [Uint8List]
  /// view backed directly by the native RGBA buffer — no memcpy
  /// happens on the success path. Ownership of that native allocation is
  /// transferred to a service-owned [NativeFinalizer]: when the [DngImage]
  /// (and therefore the Dart wrapper of the typed list) is garbage collected,
  /// `dng_free_rgba_buffer` is invoked automatically on the native pointer.
  /// The surrounding [DngResult] struct is always freed in `finally` via
  /// `dng_free_result`; on success its `rgbaData` field has been cleared so
  /// the struct teardown does not double-free the buffer.
  ///
  /// ⚠️ Do NOT capture the returned [DngImage] across isolate boundaries.
  /// The zero-copy `rgbaData` view is backed by a native pointer managed by
  /// this service's [NativeFinalizer]; sending it to another isolate transfers
  /// neither the finalizer nor the native ownership, risking use-after-free.
  /// Use [decodeOnWorker] instead when the result must cross isolate boundaries.
  /// (See also: Gotcha #45, memory.md — NativeFinalizer lifecycle.)
  ///
  /// Throws [DngDecodeException] on failure.
  DngImage decode(String filePath) {
    return _decodeZeroCopy(filePath);
  }

  static _DecodeWorkerResult _decodeFileToTransferable(String filePath) {
    final service = DngDecoderService()..initialize();
    return service._decodeToTransferable(filePath);
  }

  DngImage _decodeZeroCopy(String filePath) {
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
        throw DngDecodeException(code, _messageForErrorCode(code));
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

      // Lazily create the service-owned finalizer on the first zero-copy decode.
      // Using ??= keeps it alive for the entire service lifetime once created
      // (Gotcha #45: per-call finalizer risks GC before DngImage is collected).
      _rgbaFinalizer ??= NativeFinalizer(_bindings.dngFreeRgbaBufferPtr.cast());

      // Attach the service-owned NativeFinalizer to the DngImage.
      // When `image` is garbage collected, Dart calls `dng_free_rgba_buffer`
      // on the native pointer captured here.
      _rgbaFinalizer!.attach(image, result.rgbaData.cast(), detach: image);

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

  _DecodeWorkerResult _decodeToTransferable(String filePath) {
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
        throw DngDecodeException(code, _messageForErrorCode(code));
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
      // Copy native RGBA into Dart-owned bytes. We do NOT use zero-copy here
      // because TransferableTypedData cannot carry a native-backed typed list
      // across isolate boundaries safely.
      final rgbaCopy = Uint8List.fromList(
        result.rgbaData.asTypedList(bufferSize),
      );

      return _DecodeWorkerResult(
        rgbaData: TransferableTypedData.fromList([rgbaCopy]),
        width: width,
        height: height,
        decodeMs: result.decodeMs,
        processMs: result.processMs,
      );
    } finally {
      // dng_free_result frees BOTH the DngResult struct and its rgba_data
      // (rgba_data was NOT cleared above, unlike _decodeZeroCopy).
      // No separate dngFreeRgbaBuffer call is needed — no memory leak.
      if (resultPtr != nullptr) {
        _bindings.dngFreeResult(resultPtr);
      }
      malloc.free(pathPtr);
    }
  }

  String _messageForErrorCode(int code) {
    switch (code) {
      case -1:
        return 'File not found';
      case -2:
        return 'DNG parse error';
      case -3:
        return 'Unsupported format';
      case -4:
        return 'Memory allocation error';
      case -10:
        return 'Halide pipeline error';
      default:
        return 'Unknown error (code: $code)';
    }
  }
}
