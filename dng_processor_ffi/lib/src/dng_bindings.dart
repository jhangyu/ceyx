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
  ///
  /// 2026-08-17 (D4): the previous version kept only the LAST candidate's
  /// error, which hides the real failure. A candidate that exists but whose
  /// *dependencies* cannot be resolved (e.g. an absolute /opt/homebrew dep
  /// blocked by App Sandbox) fails with a completely different message than a
  /// candidate that is simply absent — and it was the discarded one. Every
  /// candidate now reports its own error string.
  static ffi.DynamicLibrary _openFirst(List<String> paths) {
    final errors = <String>[];
    for (final path in paths) {
      try {
        final lib = ffi.DynamicLibrary.open(path);
        // W5 (L-10): log the loaded path so dylib provenance is traceable.
        // D4: print the RESOLVED absolute path, not the candidate string —
        // downstream needs to know which copy actually got loaded.
        stderr.writeln(
          '[DngNativeBindings] loaded: ${_resolvedImagePath(lib, path)}',
        );
        return lib;
      } catch (e) {
        errors.add('  $path\n    -> $e');
        continue;
      }
    }
    throw StateError(
      'Could not load native library. Tried ${paths.length} candidate(s), '
      'each with its own error:\n'
      '${errors.join('\n')}',
    );
  }

  /// Test-only entry point for [_openFirst]. Not part of the public API and
  /// not exported by the package barrel; exists so the per-candidate error
  /// reporting stays covered by a runnable check.
  static ffi.DynamicLibrary openFirstForTesting(List<String> paths) =>
      _openFirst(paths);

  /// Best-effort ABSOLUTE path of the image that [lib] was actually loaded
  /// from, for logging only.
  ///
  /// D4 (2026-08-17): the candidate string is not good enough. The first
  /// candidate is the bare name `libdng_decoder_native.dylib`, which dyld
  /// resolves through its own search paths — so logging the candidate tells a
  /// downstream integrator nothing about which copy got loaded (this exact
  /// ambiguity produced an unsatisfiable acceptance criterion downstream).
  /// We ask the loader instead, via `dladdr` on a symbol of the freshly opened
  /// library, and only fall back to path arithmetic.
  static String _resolvedImagePath(ffi.DynamicLibrary lib, String candidate) {
    final viaLoader = _imagePathViaDladdr(lib);
    if (viaLoader != null) return viaLoader;
    if (!candidate.contains('/')) return candidate;
    try {
      return File(candidate).absolute.resolveSymbolicLinksSync();
    } catch (_) {
      return candidate;
    }
  }

  /// Resolve the on-disk path of an opened library via `dladdr` (POSIX only).
  /// Returns null if anything goes wrong — this is diagnostics, never fatal.
  static String? _imagePathViaDladdr(ffi.DynamicLibrary lib) {
    if (Platform.isWindows) return null;
    ffi.Pointer<ffi.Pointer<ffi.Void>>? info;
    try {
      // Any symbol belonging to the library identifies its image.
      ffi.Pointer<ffi.Void>? probe;
      for (final symbol in const ['dng_decode_and_process', 'dng_free_buffer']) {
        try {
          probe = lib.lookup<ffi.Void>(symbol);
          break;
        } catch (_) {
          continue;
        }
      }
      if (probe == null) return null;

      final dladdr = ffi.DynamicLibrary.process().lookupFunction<
          ffi.Int Function(
              ffi.Pointer<ffi.Void>, ffi.Pointer<ffi.Pointer<ffi.Void>>),
          int Function(ffi.Pointer<ffi.Void>,
              ffi.Pointer<ffi.Pointer<ffi.Void>>)>('dladdr');

      // Dl_info = { const char* dli_fname; void* dli_fbase;
      //             const char* dli_sname; void* dli_saddr; }
      info = calloc<ffi.Pointer<ffi.Void>>(4);
      if (dladdr(probe, info) == 0) return null;
      final fname = info[0];
      if (fname == ffi.nullptr) return null;
      return fname.cast<Utf8>().toDartString();
    } catch (_) {
      return null;
    } finally {
      if (info != null) calloc.free(info);
    }
  }

  /// Load the native library based on the current platform
  factory DngNativeBindings() => DngNativeBindings.load();

  /// Load the native library based on the current platform
  factory DngNativeBindings.load() {
    final ffi.DynamicLibrary lib;

    if (Platform.isMacOS) {
      final execDir = File(Platform.resolvedExecutable).parent.path;

      // W7-6 (TD-18): dylib loader path 3/4 hardened.
      // Priority:
      //   1. System default (DYLD_LIBRARY_PATH) — no path prefix needed
      //   2. App bundle Frameworks/ — production distribution, populated by the
      //      `dng_processor_ffi` plugin pod (see dng_processor_ffi/README.md)
      //   3. DNG_NATIVE_BUILD_DIR env override — CI / custom build directories
      //   4. Platform.script-relative — dart run from repo root (e.g. dart run bin/*)
      //
      // 2026-08-21 (D1): the former candidate 5, a pair of absolute
      // $HOME/project/... dev paths gated behind DNG_DEV_FALLBACK, is gone.
      // Host apps now get the dylib bundled into Frameworks/ by the plugin, so
      // candidate 2 covers what the dev fallback used to paper over, and
      // candidate 4 still covers `dart run` inside this repo.

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
