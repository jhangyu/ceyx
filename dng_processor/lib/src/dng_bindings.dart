import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:ffi/ffi.dart';

/*
---
file_summary: "dart:ffi 綁定設定，處理不同平台的動態函式庫載入"
modules:
  - name: "DngResult"
    description: "Native C-API 對應的資料結構"
    lines: "6-24"
  - name: "Type Definitions"
    description: "C 函數簽名綁定"
    lines: "26-33"
  - name: "DngNativeBindings"
    description: "尋找並載入 dll/so/dylib 函式庫"
    lines: "36-107"
---
*/

/// FFI struct matching C `DngResult` from dng_ffi_api.h
final class DngResult extends ffi.Struct {
  external ffi.Pointer<ffi.Uint8> rgbaData;

  @ffi.Int32()
  external int width;

  @ffi.Int32()
  external int height;

  @ffi.Int32()
  external int errorCode;

  @ffi.Double()
  external double decodeMs;

  @ffi.Double()
  external double processMs;
}

/// C function signatures
typedef DngDecodeAndProcessNative =
    ffi.Pointer<DngResult> Function(ffi.Pointer<Utf8> filePath);
typedef DngDecodeAndProcessDart =
    ffi.Pointer<DngResult> Function(ffi.Pointer<Utf8> filePath);

typedef DngFreeResultNative = ffi.Void Function(ffi.Pointer<DngResult> result);
typedef DngFreeResultDart = void Function(ffi.Pointer<DngResult> result);

typedef DngFreeHalideBufferNative =
    ffi.Void Function(ffi.Pointer<ffi.Void> ptr);
typedef DngFreeHalideBufferDart = void Function(ffi.Pointer<ffi.Void> ptr);
typedef DngFreeRgbaBufferNative = ffi.Void Function(ffi.Pointer<ffi.Void> ptr);
typedef DngFreeRgbaBufferDart = void Function(ffi.Pointer<ffi.Void> ptr);

typedef DngDecoderWarmupForSizeNative =
    ffi.Int32 Function(ffi.Int32 width, ffi.Int32 height);
typedef DngDecoderWarmupForSizeDart = int Function(int width, int height);

// R3-3: VkPipelineCache persistence (Android/Vulkan only; native returns -1
// "unsupported" on other platforms/builds — see dng_ffi_api.h).
typedef DngDecoderSetPipelineCachePathNative =
    ffi.Int32 Function(ffi.Pointer<Utf8> path);
typedef DngDecoderSetPipelineCachePathDart =
    int Function(ffi.Pointer<Utf8> path);

typedef DngDecoderSavePipelineCacheNative = ffi.Int32 Function();
typedef DngDecoderSavePipelineCacheDart = int Function();

typedef DngDecoderPipelineCacheStatusNative = ffi.Int32 Function();
typedef DngDecoderPipelineCacheStatusDart = int Function();

typedef dng_extract_preview_jpeg_func =
    ffi.Int32 Function(
      ffi.Pointer<Utf8> filePath,
      ffi.Pointer<ffi.Pointer<ffi.Uint8>> outBuffer,
      ffi.Pointer<ffi.Int32> outSize,
    );
typedef DngExtractPreviewJpeg =
    int Function(
      ffi.Pointer<Utf8> filePath,
      ffi.Pointer<ffi.Pointer<ffi.Uint8>> outBuffer,
      ffi.Pointer<ffi.Int32> outSize,
    );

typedef dng_free_buffer_func = ffi.Void Function(ffi.Pointer<ffi.Uint8> buffer);
typedef DngFreeBuffer = void Function(ffi.Pointer<ffi.Uint8> buffer);

/// Bindings to the native dng_decoder_native library
class DngNativeBindings {
  final ffi.DynamicLibrary _lib;

  late final DngDecodeAndProcessDart dngDecodeAndProcess;
  late final DngDecoderWarmupForSizeDart dngDecoderWarmupForSize;
  // R3-3: pipeline cache persistence controls.
  late final DngDecoderSetPipelineCachePathDart dngDecoderSetPipelineCachePath;
  late final DngDecoderSavePipelineCacheDart dngDecoderSavePipelineCache;
  late final DngDecoderPipelineCacheStatusDart dngDecoderPipelineCacheStatus;
  late final DngFreeResultDart dngFreeResult;
  late final DngFreeHalideBufferDart dngFreeHalideBuffer;
  late final DngFreeRgbaBufferDart dngFreeRgbaBuffer;

  late final DngExtractPreviewJpeg extractPreviewJpeg;
  late final DngFreeBuffer freeBuffer;

  /// Pointer to the C `dng_free_result` function for NativeFinalizer (if we were finalizing the whole result)
  late final ffi.Pointer<ffi.NativeFunction<DngFreeResultNative>>
  dngFreeResultPtr;

  /// Pointer to the C `dng_free_rgba_buffer` function for NativeFinalizer
  late final ffi.Pointer<ffi.NativeFunction<DngFreeRgbaBufferNative>>
  dngFreeRgbaBufferPtr;

  /// Deprecated pointer to the C `dng_free_halide_buffer` alias.
  late final ffi.Pointer<ffi.NativeFunction<DngFreeHalideBufferNative>>
  dngFreeHalideBufferPtr;

  DngNativeBindings._(this._lib) {
    dngDecodeAndProcess = _lib
        .lookupFunction<DngDecodeAndProcessNative, DngDecodeAndProcessDart>(
          'dng_decode_and_process',
        );

    dngDecoderWarmupForSize = _lib
        .lookupFunction<
          DngDecoderWarmupForSizeNative,
          DngDecoderWarmupForSizeDart
        >('dng_decoder_warmup_for_size');

    // R3-3: pipeline cache persistence controls.
    dngDecoderSetPipelineCachePath = _lib
        .lookupFunction<
          DngDecoderSetPipelineCachePathNative,
          DngDecoderSetPipelineCachePathDart
        >('dng_decoder_set_pipeline_cache_path');
    dngDecoderSavePipelineCache = _lib
        .lookupFunction<
          DngDecoderSavePipelineCacheNative,
          DngDecoderSavePipelineCacheDart
        >('dng_decoder_save_pipeline_cache');
    dngDecoderPipelineCacheStatus = _lib
        .lookupFunction<
          DngDecoderPipelineCacheStatusNative,
          DngDecoderPipelineCacheStatusDart
        >('dng_decoder_pipeline_cache_status');

    dngFreeResult = _lib.lookupFunction<DngFreeResultNative, DngFreeResultDart>(
      'dng_free_result',
    );

    dngFreeHalideBuffer = _lib
        .lookupFunction<DngFreeHalideBufferNative, DngFreeHalideBufferDart>(
          'dng_free_halide_buffer',
        );
    dngFreeRgbaBuffer = _lib
        .lookupFunction<DngFreeRgbaBufferNative, DngFreeRgbaBufferDart>(
          'dng_free_rgba_buffer',
        );

    dngFreeResultPtr = _lib.lookup<ffi.NativeFunction<DngFreeResultNative>>(
      'dng_free_result',
    );

    dngFreeRgbaBufferPtr = _lib
        .lookup<ffi.NativeFunction<DngFreeRgbaBufferNative>>(
          'dng_free_rgba_buffer',
        );

    dngFreeHalideBufferPtr = _lib
        .lookup<ffi.NativeFunction<DngFreeHalideBufferNative>>(
          'dng_free_halide_buffer',
        );

    extractPreviewJpeg = _lib
        .lookup<ffi.NativeFunction<dng_extract_preview_jpeg_func>>(
          'dng_extract_preview_jpeg',
        )
        .asFunction();
    freeBuffer = _lib
        .lookup<ffi.NativeFunction<dng_free_buffer_func>>('dng_free_buffer')
        .asFunction();
  }

  /// Try to open dylib from a list of candidate paths.
  /// Returns the first one that loads successfully, or throws.
  /// W5 (L-10): logs the successfully loaded path to stderr for diagnostics.
  static ffi.DynamicLibrary _openFirst(List<String> paths) {
    Object? lastError;
    for (final path in paths) {
      try {
        final lib = ffi.DynamicLibrary.open(path);
        // W5 (L-10): log the loaded path so dylib provenance is traceable.
        stderr.writeln('[DngNativeBindings] loaded: $path');
        return lib;
      } catch (e) {
        lastError = e;
        continue;
      }
    }
    throw StateError(
      'Could not load native library from any of:\n'
      '  ${paths.join('\n  ')}\n'
      'Last error: $lastError',
    );
  }

  /// Load the native library based on the current platform
  factory DngNativeBindings() => DngNativeBindings.load();

  /// Load the native library based on the current platform
  factory DngNativeBindings.load() {
    final ffi.DynamicLibrary lib;

    if (Platform.isMacOS) {
      final execDir = File(Platform.resolvedExecutable).parent.path;
      final home = Platform.environment['HOME'] ?? '/tmp';

      // W7-6 (TD-18): dylib loader path 3/4 hardened.
      // Priority:
      //   1. System default (DYLD_LIBRARY_PATH) — no path prefix needed
      //   2. App bundle Frameworks/ — production distribution
      //   3. DNG_NATIVE_BUILD_DIR env override — CI / custom build directories
      //   4. Platform.script-relative — dart run from repo root (e.g. dart run bin/*)
      //   5. last-resort dev path — hardcoded fallback for local dev at HOME/project/...

      // Resolve paths 4a/4b relative to the script entry point (repo layout).
      final scriptDir = Platform.script.toFilePath(windows: false);
      final scriptParent = File(scriptDir).parent.path;
      // When running `dart run bin/benchmark_*.dart` the script is at
      // <repo>/dng_processor/bin/benchmark_*.dart → parent = dng_processor/bin
      // so ../native/build reaches dng_processor/native/build.
      final scriptRelativeDist =
          File('$scriptParent/../native/dist/libdng_decoder_native.dylib')
              .path;
      final scriptRelativeBuild =
          File('$scriptParent/../native/build/libdng_decoder_native.dylib')
              .path;

      // DNG_NATIVE_BUILD_DIR env override (path to the CMake build directory).
      final nativeBuildDir =
          Platform.environment['DNG_NATIVE_BUILD_DIR'];

      // W5 (L-10): dev absolute fallback paths gated behind DNG_DEV_FALLBACK=1.
      // Follows the DNG_NATIVE_BUILD_DIR precedent (env-gated, not unconditional).
      final devFallback =
          Platform.environment['DNG_DEV_FALLBACK'] == '1';

      lib = _openFirst([
        // 1. System default (DYLD_LIBRARY_PATH)
        'libdng_decoder_native.dylib',
        // 2. App bundle Frameworks directory
        '$execDir/../Frameworks/libdng_decoder_native.dylib',
        // 3. Env override: DNG_NATIVE_BUILD_DIR (CI / custom build dir)
        if (nativeBuildDir != null)
          '$nativeBuildDir/libdng_decoder_native.dylib',
        // 4a. Script-relative: dist artifact (dart run scenario)
        scriptRelativeDist,
        // 4b. Script-relative: CMake build cache (dart run scenario)
        scriptRelativeBuild,
        // 5. W5 (L-10): dev absolute paths, env-gated (DNG_DEV_FALLBACK=1)
        if (devFallback)
          '$home/project/flutter_dng_decoder/dng_processor/dist/libdng_decoder_native.dylib',
        if (devFallback)
          '$home/project/flutter_dng_decoder/dng_processor/native/build/libdng_decoder_native.dylib',
      ]);
    } else if (Platform.isWindows) {
      lib = ffi.DynamicLibrary.open('dng_decoder_native.dll');
    } else if (Platform.isLinux) {
      lib = ffi.DynamicLibrary.open('libdng_decoder_native.so');
    } else if (Platform.isAndroid) {
      lib = ffi.DynamicLibrary.open('libdng_decoder_native.so');
    } else if (Platform.isIOS) {
      lib = ffi.DynamicLibrary.process();
    } else {
      throw UnsupportedError(
        'DngNativeBindings: unsupported platform ${Platform.operatingSystem}',
      );
    }

    return DngNativeBindings._(lib);
  }
}
