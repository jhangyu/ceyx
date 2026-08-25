import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';

import 'dng_bindings.dart';
import 'raw_bindings.dart';
import 'raw_error_codes.dart';
import 'raw_route.dart';

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

/// W5 (M-6): Dart mirror of the C enum DngErrorCode (dng_error_codes.h).
/// Any value change in the C header MUST be reflected here.
///
/// Note: warmup uses its own -1/-2 return scale (not this enum).
/// Preview (dng_extract_preview_jpeg) uses four independent scales.
abstract final class DngErrorCode {
  static const int success = 0;
  static const int nullPath = -1;
  static const int parseFailed = -2;
  static const int stage3Failed = -3;
  static const int stage4Failed = -4;
  static const int stage2HandoffRestoreFailed = -5;
  static const int gpuUnavailable = -6;
  static const int rgbaAllocFailed = -7; // FFI layer only
  static const int ol2DispatchFailed = -8;
  static const int stdException = -100;
  static const int unknownException = -101;
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

  /// Optional explicit dylib path, bypassing the platform candidate search
  /// in [DngNativeBindings.load]. Primarily for tests and host apps with a
  /// non-standard library layout; production callers should leave this null.
  final String? _libraryPath;

  DngDecoderService({String? libraryPath}) : _libraryPath = libraryPath;

  /// Initialize the service by loading the native library.
  void initialize() {
    if (_initialized) return;
    _bindings = _libraryPath == null
        ? DngNativeBindings.load()
        : DngNativeBindings.fromPath(_libraryPath);
    // _rgbaFinalizer is intentionally NOT created here; it is lazily created in
    // _decodeZeroCopy so that worker-isolate services (which only call
    // _decodeToTransferable) do not allocate a finalizer they will never use.
    _initialized = true;
  }

  /// Whether the loaded native library exports `dng_decode_and_process_sized`.
  /// Initializes the service if needed.
  bool get sizedDecodeAvailable {
    if (!_initialized) initialize();
    return _bindings.sizedDecodeAvailable;
  }

  /// Whether the loaded native library exports `raw_decode_and_process`.
  /// Initializes the service if needed.
  bool get rawDecodeAvailable {
    if (!_initialized) initialize();
    return _bindings.rawDecodeAvailable;
  }

  /// Diagnostics for the most recent generic-RAW decode observed on the
  /// current OS thread.
  ///
  /// Native state is `thread_local` (raw_ffi_api.cpp:19), NOT per-isolate.
  /// After [decodeOnWorker], reading this on the calling isolate is
  /// unreliable in either direction: depending on OS thread reuse, it may
  /// return null, the worker's values, or — if this thread previously ran a
  /// decode itself — an unrelated earlier decode's values. Provenance is not
  /// verifiable from Dart, so callers must not rely on this after a worker
  /// decode. Also note a failed decode does not clear the native scratch
  /// state, so a subsequent read can still surface an earlier successful
  /// decode's diagnostics. Only a same-isolate read taken immediately after a
  /// successful [decode] call is meaningful.
  RawDiagnostics? get lastRawDiagnostics {
    if (!_initialized) initialize();
    return _bindings.lastRawDiagnostics();
  }

  /// RGBA pool buffers currently checked out process-wide; 0 when everything
  /// has been freed. Null when the dylib lacks the debug symbol.
  ///
  /// Note: a zero-copy [decode] keeps its buffer checked out until the
  /// returned [DngImage] is garbage collected, so a non-zero value right
  /// after a successful [decode] is correct, not a leak.
  int? get poolCheckedOut {
    if (!_initialized) initialize();
    return _bindings.poolCheckedOut();
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

  /// R3-3: Set the VkPipelineCache persistence file path (Android/Vulkan only).
  ///
  /// Call BEFORE [warmupForSize] / the first decode with a writable per-app
  /// path (e.g. `<cacheDir>/dng_vk_pipeline.cache`). Native state is
  /// process-global, so setting it here is visible to worker isolates.
  /// Returns 0 when applied, -1 when unsupported on this platform/build
  /// (macOS/Metal, or native built with DNG_VK_PIPELINE_CACHE=OFF).
  /// Never throws: cache problems must never break decoding.
  int setPipelineCachePath(String path) {
    if (!_initialized) initialize();
    final nativePath = path.toNativeUtf8();
    try {
      return _bindings.dngDecoderSetPipelineCachePath(nativePath);
    } finally {
      malloc.free(nativePath);
    }
  }

  /// R3-3: Flush the pipeline cache to disk now (also happens automatically
  /// after warmup and after each decode). 0 = saved/nothing-to-do,
  /// -1 = unsupported, -2 = non-fatal save failure.
  int savePipelineCache() {
    if (!_initialized) initialize();
    return _bindings.dngDecoderSavePipelineCache();
  }

  /// R3-3: Pipeline-cache status bitmask for diagnostics/evidence:
  /// 1 = enabled, 2 = cache object exists, 4 = cache file was loaded at
  /// startup (cross-launch hit), 8 = unsaved data pending. -1 = unsupported.
  int get pipelineCacheStatus {
    if (!_initialized) initialize();
    return _bindings.dngDecoderPipelineCacheStatus();
  }

  /// Decode on a worker isolate so the UI isolate can keep painting preview
  /// and progress state while native full RAW processing runs.
  ///
  /// The worker intentionally does not send the zero-copy external RGBA view
  /// across isolate boundaries. It copies the native buffer into Dart-owned
  /// bytes, transfers those bytes with [TransferableTypedData], then frees the
  /// native result inside the worker isolate.
  ///
  /// [maxDim] is a REQUEST for a decode whose longest output edge is
  /// approximately [maxDim] pixels — it is silently ignored (falling back to
  /// today's full-resolution entry point) whenever the loaded native library
  /// does not export `dng_decode_and_process_sized`, or when [maxDim] is
  /// null. Callers must read the returned [DngImage.width]/[DngImage.height]
  /// rather than assuming the request was honored.
  /// On the generic RAW route, `maxDim` is forwarded to the native `max_dim`
  /// parameter but is currently IGNORED by the native RAW route (no
  /// downsampling is applied there yet — measured: requesting 800 on a
  /// 6246x4170 RAF still returns full resolution). Callers must read the
  /// returned [DngImage.width]/[DngImage.height] rather than assuming the
  /// request was honored, exactly as for the DNG route above.
  Future<DngImage> decodeOnWorker(String filePath, {int? maxDim}) async {
    // Hoist to a local before the closure: referencing `_libraryPath`
    // directly inside Isolate.run's closure captures `this` (the whole
    // DngDecoderService, including its DynamicLibrary/NativeFinalizer once
    // initialized), which Isolate.run cannot send — it throws ArgumentError
    // for any service that has already been initialize()d.
    final libraryPath = _libraryPath;
    final result = await Isolate.run(
      () => _decodeFileToTransferable(filePath, libraryPath, maxDim),
    );
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
    if (!_initialized) {
      initialize();
    }
    switch (decodeRouteForPath(filePath)) {
      case DecodeRoute.dng:
        return _decodeZeroCopy(filePath);
      case DecodeRoute.raw:
        return _decodeRawZeroCopy(filePath);
      case DecodeRoute.unsupported:
        throw DngDecodeException(
          DngErrorCode.parseFailed,
          _unsupportedMessage(filePath),
        );
    }
  }

  static String _unsupportedMessage(String filePath) =>
      "Unsupported file extension '${decodeExtensionOf(filePath)}' for "
      '$filePath; supported: ${kSupportedDecodeExtensions.join(', ')}';

  static _DecodeWorkerResult _decodeFileToTransferable(
    String filePath,
    String? libraryPath,
    int? maxDim,
  ) {
    final service = DngDecoderService(libraryPath: libraryPath)..initialize();
    return service._decodeToTransferable(filePath, maxDim: maxDim);
  }

  DngImage _decodeZeroCopy(String filePath) {
    if (!_initialized) {
      initialize();
    }

    final pathPtr = filePath.toNativeUtf8();
    Pointer<DngResult> resultPtr = nullptr;

    try {
      resultPtr = _bindings.dngDecodeAndProcess(pathPtr.cast());
      return _finishZeroCopy(resultPtr, isRaw: false);
    } finally {
      // Always free the native result struct (which no longer owns the
      // rgbaData on success).
      if (resultPtr != nullptr) {
        _bindings.dngFreeResult(resultPtr);
      }
      malloc.free(pathPtr);
    }
  }

  /// Generic-RAW twin of [_decodeZeroCopy]. Same pool, same free function,
  /// same finalizer — only the entry point and the error scale differ.
  DngImage _decodeRawZeroCopy(String filePath) {
    if (!_initialized) {
      initialize();
    }

    final rawDecode = _bindings.rawDecodeAndProcess;
    if (rawDecode == null) {
      // Spec §4: typed exception, never a crash and never a silent fallback
      // to the DNG parser.
      throw RawUnavailableException(filePath);
    }

    final pathPtr = filePath.toNativeUtf8();
    Pointer<DngResult> resultPtr = nullptr;

    try {
      // max_dim == 0 means full resolution (dng_ffi_api.h:114).
      resultPtr = rawDecode(pathPtr.cast(), 0);
      return _finishZeroCopy(resultPtr, isRaw: true);
    } finally {
      if (resultPtr != nullptr) {
        _bindings.dngFreeResult(resultPtr);
      }
      malloc.free(pathPtr);
    }
  }

  /// Shared success/failure handling for both zero-copy routes.
  ///
  /// On success the native RGBA buffer is wrapped without a memcpy, ownership
  /// is transferred to the service-owned NativeFinalizer, and
  /// `result.rgbaData` is cleared so the caller's `dng_free_result` cannot
  /// double-free it.
  DngImage _finishZeroCopy(
    Pointer<DngResult> resultPtr, {
    required bool isRaw,
  }) {
    if (resultPtr == nullptr) {
      if (isRaw) {
        throw RawDecodeException(
          RawErrorCode.allocationFailed,
          RawErrorCode.name(RawErrorCode.allocationFailed),
          'Native raw_decode_and_process returned null',
        );
      }
      throw DngDecodeException(-1, 'Native function returned null');
    }

    final result = resultPtr.ref;

    if (result.errorCode != 0) {
      _throwDecodeError(result.errorCode, isRaw: isRaw);
    }

    if (result.rgbaData == nullptr) {
      if (isRaw) {
        throw RawDecodeException(
          RawErrorCode.allocationFailed,
          RawErrorCode.name(RawErrorCode.allocationFailed),
          'RGBA buffer is null despite kRawSuccess',
        );
      }
      throw DngDecodeException(-1, 'RGBA buffer is null despite success code');
    }

    final width = result.width;
    final height = result.height;
    final bufferSize = width * height * 4;

    // Zero-copy: view the native memory directly instead of copying.
    final rgbaData = result.rgbaData.asTypedList(bufferSize);

    final image = DngImage(
      rgbaData: rgbaData,
      width: width,
      height: height,
      decodeMs: result.decodeMs,
      processMs: result.processMs,
    );

    // Lazily create the service-owned finalizer (Gotcha #45: a per-call
    // finalizer risks being collected before the DngImage it guards).
    _rgbaFinalizer ??= NativeFinalizer(_bindings.dngFreeRgbaBufferPtr.cast());
    _rgbaFinalizer!.attach(image, result.rgbaData.cast(), detach: image);

    // Ownership handed to the finalizer — clear the struct field so
    // dng_free_result does not free the same pointer again.
    result.rgbaData = nullptr;

    return image;
  }

  /// Map a native `DngResult.error_code` onto the right exception type.
  /// RAW codes (<= -201) are disjoint from DNG codes by contract
  /// (raw_pipeline_contract.h:12-13).
  Never _throwDecodeError(int code, {required bool isRaw}) {
    if (isRaw || RawErrorCode.isRawError(code)) {
      throw RawDecodeException(
        code,
        RawErrorCode.name(code),
        _messageForRawErrorCode(code),
      );
    }
    throw DngDecodeException(code, _messageForErrorCode(code));
  }

  String _messageForRawErrorCode(int code) {
    switch (code) {
      case RawErrorCode.nullPath:
        return 'Null or empty file path';
      case RawErrorCode.probeFailed:
        return 'Container probe failed (not a recognised RAW/TIFF header)';
      case RawErrorCode.parseFailed:
        return 'RAW container parse failed';
      case RawErrorCode.unpackFailed:
        return 'RAW sample unpack failed';
      case RawErrorCode.layoutUnsupported:
        return 'Sensor layout not supported by this build';
      case RawErrorCode.metadataInvalid:
        return 'RAW metadata invalid or inconsistent';
      case RawErrorCode.gpuUnavailable:
        return 'GPU (Metal/Vulkan) unavailable';
      case RawErrorCode.kernelFailed:
        return 'GPU kernel dispatch failed';
      case RawErrorCode.allocationFailed:
        return 'Native allocation failed';
      case RawErrorCode.sizeOverflow:
        return 'Image dimensions exceed the supported pixel ceiling';
      case RawErrorCode.cancelled:
        return 'Decode cancelled by request';
      default:
        return 'Unknown RAW error (code: $code)';
    }
  }

  _DecodeWorkerResult _decodeToTransferable(
    String filePath, {
    int? maxDim,
  }) {
    if (!_initialized) {
      initialize();
    }
    switch (decodeRouteForPath(filePath)) {
      case DecodeRoute.dng:
        return _decodeDngToTransferable(filePath, maxDim);
      case DecodeRoute.raw:
        return _decodeRawToTransferable(filePath, maxDim);
      case DecodeRoute.unsupported:
        throw DngDecodeException(
          DngErrorCode.parseFailed,
          _unsupportedMessage(filePath),
        );
    }
  }

  _DecodeWorkerResult _decodeDngToTransferable(String filePath, int? maxDim) {
    final pathPtr = filePath.toNativeUtf8();
    Pointer<DngResult> resultPtr = nullptr;

    try {
      resultPtr =
          (maxDim != null && maxDim > 0 && _bindings.sizedDecodeAvailable)
          ? _bindings.dngDecodeAndProcessSized!(pathPtr.cast(), maxDim)
          : _bindings.dngDecodeAndProcess(pathPtr.cast());
      return _finishTransferable(resultPtr, isRaw: false);
    } finally {
      // dng_free_result frees BOTH the struct and its rgba_data (rgba_data
      // was NOT cleared, unlike the zero-copy path). No leak, no extra call.
      if (resultPtr != nullptr) {
        _bindings.dngFreeResult(resultPtr);
      }
      malloc.free(pathPtr);
    }
  }

  _DecodeWorkerResult _decodeRawToTransferable(String filePath, int? maxDim) {
    final rawDecode = _bindings.rawDecodeAndProcess;
    if (rawDecode == null) {
      throw RawUnavailableException(filePath);
    }

    // 0 and negatives mean "no request" -> full resolution, matching the DNG
    // sized-decode contract.
    final requested = (maxDim != null && maxDim > 0) ? maxDim : 0;

    final pathPtr = filePath.toNativeUtf8();
    Pointer<DngResult> resultPtr = nullptr;

    try {
      resultPtr = rawDecode(pathPtr.cast(), requested);
      return _finishTransferable(resultPtr, isRaw: true);
    } finally {
      if (resultPtr != nullptr) {
        _bindings.dngFreeResult(resultPtr);
      }
      malloc.free(pathPtr);
    }
  }

  _DecodeWorkerResult _finishTransferable(
    Pointer<DngResult> resultPtr, {
    required bool isRaw,
  }) {
    if (resultPtr == nullptr) {
      if (isRaw) {
        throw RawDecodeException(
          RawErrorCode.allocationFailed,
          RawErrorCode.name(RawErrorCode.allocationFailed),
          'Native raw_decode_and_process returned null',
        );
      }
      throw DngDecodeException(-1, 'Native function returned null');
    }

    final result = resultPtr.ref;

    if (result.errorCode != 0) {
      _throwDecodeError(result.errorCode, isRaw: isRaw);
    }

    if (result.rgbaData == nullptr) {
      if (isRaw) {
        throw RawDecodeException(
          RawErrorCode.allocationFailed,
          RawErrorCode.name(RawErrorCode.allocationFailed),
          'RGBA buffer is null despite kRawSuccess',
        );
      }
      throw DngDecodeException(-1, 'RGBA buffer is null despite success code');
    }

    final width = result.width;
    final height = result.height;
    final bufferSize = width * height * 4;
    // Copy into Dart-owned bytes: TransferableTypedData cannot carry a
    // native-backed typed list across isolate boundaries safely.
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
  }

  // W5 (M-6): messages aligned with unified DngErrorCode enum.
  String _messageForErrorCode(int code) {
    switch (code) {
      case DngErrorCode.nullPath:
        return 'Null or empty file path';
      case DngErrorCode.parseFailed:
        return 'DNG parse/validation failed';
      case DngErrorCode.stage3Failed:
        return 'Stage3 (demosaic) failed';
      case DngErrorCode.stage4Failed:
        return 'Stage4 (render) failed';
      case DngErrorCode.stage2HandoffRestoreFailed:
        return 'Stage2 device-handoff restore failed';
      case DngErrorCode.gpuUnavailable:
        return 'GPU (Metal/Vulkan) unavailable';
      case DngErrorCode.rgbaAllocFailed:
        return 'RGBA buffer allocation failed';
      case DngErrorCode.ol2DispatchFailed:
        return 'OpcodeList2 GPU dispatch failed';
      case DngErrorCode.stdException:
        return 'Internal C++ exception';
      case DngErrorCode.unknownException:
        return 'Unknown internal exception';
      default:
        if (code > 0) {
          return 'DNG SDK error (code: $code)';
        }
        return 'Unknown error (code: $code)';
    }
  }
}
