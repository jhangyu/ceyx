import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

/// FFI struct matching C `DngResult` from dng_ffi_api.h
final class DngResult extends Struct {
  external Pointer<Uint8> rgbaData;

  @Int32()
  external int width;

  @Int32()
  external int height;

  @Int32()
  external int errorCode;

  @Double()
  external double decodeMs;

  @Double()
  external double processMs;
}

/// C function signatures
typedef DngDecodeAndProcessNative = Pointer<DngResult> Function(
    Pointer<Utf8> filePath);
typedef DngDecodeAndProcessDart = Pointer<DngResult> Function(
    Pointer<Utf8> filePath);

typedef DngFreeResultNative = Void Function(Pointer<DngResult> result);
typedef DngFreeResultDart = void Function(Pointer<DngResult> result);

/// Bindings to the native dng_decoder_native library
class DngBindings {
  final DynamicLibrary _lib;

  late final DngDecodeAndProcessDart dngDecodeAndProcess;
  late final DngFreeResultDart dngFreeResult;

  /// Pointer to the C `dng_free_result` function for NativeFinalizer
  late final Pointer<NativeFunction<DngFreeResultNative>> dngFreeResultPtr;

  DngBindings._(this._lib) {
    dngDecodeAndProcess = _lib
        .lookupFunction<DngDecodeAndProcessNative, DngDecodeAndProcessDart>(
            'dng_decode_and_process');

    dngFreeResult =
        _lib.lookupFunction<DngFreeResultNative, DngFreeResultDart>(
            'dng_free_result');

    dngFreeResultPtr =
        _lib.lookup<NativeFunction<DngFreeResultNative>>('dng_free_result');
  }

  /// Try to open dylib from a list of candidate paths.
  /// Returns the first one that loads successfully, or throws.
  static DynamicLibrary _openFirst(List<String> paths) {
    Object? lastError;
    for (final path in paths) {
      try {
        return DynamicLibrary.open(path);
      } catch (e) {
        lastError = e;
        continue;
      }
    }
    throw StateError(
        'Could not load native library from any of:\n'
        '  ${paths.join('\n  ')}\n'
        'Last error: $lastError');
  }

  /// Load the native library based on the current platform
  factory DngBindings.load() {
    final DynamicLibrary lib;

    if (Platform.isMacOS) {
      final execDir = File(Platform.resolvedExecutable).parent.path;
      final home = Platform.environment['HOME'] ?? '/tmp';
      lib = _openFirst([
        // 1. System default (DYLD_LIBRARY_PATH)
        'libdng_decoder_native.dylib',
        // 2. App bundle Frameworks directory
        '$execDir/../Frameworks/libdng_decoder_native.dylib',
        // 3. Development: project native/build directory
        '$home/Documents/flutter_dng_decoder/dng_processor/native/build/libdng_decoder_native.dylib',
      ]);
    } else if (Platform.isWindows) {
      lib = DynamicLibrary.open('dng_decoder_native.dll');
    } else if (Platform.isLinux) {
      lib = DynamicLibrary.open('libdng_decoder_native.so');
    } else if (Platform.isAndroid) {
      lib = DynamicLibrary.open('libdng_decoder_native.so');
    } else if (Platform.isIOS) {
      lib = DynamicLibrary.process();
    } else {
      throw UnsupportedError(
          'DngBindings: unsupported platform ${Platform.operatingSystem}');
    }

    return DngBindings._(lib);
  }
}
