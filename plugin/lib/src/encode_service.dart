import 'dart:ffi' as ffi;
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'dng_bindings.dart';
import 'encode_bindings.dart';

/*
---
file_summary: "RGBA8 -> JPEG/WebP 編碼服務：off-UI-isolate FFI 封裝與記憶體管理"
modules:
  - name: "CeyxEncodeException"
    description: "編碼錯誤定義，附原生錯誤名稱"
    lines: "below"
  - name: "CeyxEncodeService"
    description: "encodeJpegNative / encodeWebpNative 高階入口，皆在 worker isolate 執行"
    lines: "below"
---
*/

/// Error thrown when a native encode call fails. [errorCode] is one of
/// [CeyxEncodeErrorCode]; [errorName] mirrors the native
/// `ceyx_encode_error_name` spelling.
class CeyxEncodeException implements Exception {
  final int errorCode;
  final String errorName;

  CeyxEncodeException(this.errorCode, this.errorName);

  @override
  String toString() => 'CeyxEncodeException($errorCode $errorName)';
}

/// Thrown when the loaded native library does not export the encode symbols
/// (predates commit 1764a8f, or built without them).
class CeyxEncodeUnavailableException implements Exception {
  @override
  String toString() =>
      'CeyxEncodeUnavailableException: native encode symbols not found in '
      'the loaded dylib';
}

/// High-level RGBA8 -> JPEG/WebP encode service.
///
/// Both entry points run on a worker [Isolate] (via [Isolate.run]) so the
/// caller's isolate is never blocked by the native encode call, matching the
/// off-UI-isolate convention [DngDecoderService.decodeOnWorker] uses.
/// `rgba` is copied into worker-isolate-owned bytes before crossing the
/// isolate boundary and the returned [Uint8List] is fully Dart-owned — no
/// native pointer survives the call, so no [Finalizable] bookkeeping is
/// needed here (contrast [DngDecoderService]'s zero-copy decode path).
class CeyxEncodeService {
  /// Optional explicit dylib path, bypassing the platform candidate search
  /// in [DngNativeBindings.load]. Primarily for tests.
  final String? _libraryPath;

  CeyxEncodeService({String? libraryPath}) : _libraryPath = libraryPath;

  /// Encodes `rgba` ([width] x [height], 4 bytes/pixel, tightly packed) as a
  /// baseline JPEG at [quality] (1..100; alpha is discarded).
  ///
  /// Throws [CeyxEncodeUnavailableException] if the loaded dylib lacks the
  /// encode symbols, or [CeyxEncodeException] on any native encode failure.
  Future<Uint8List> encodeJpegNative(
    Uint8List rgba, {
    required int width,
    required int height,
    required int quality,
  }) {
    final libraryPath = _libraryPath;
    return Isolate.run(
      () => _encodeOnWorker(
        rgba,
        width: width,
        height: height,
        quality: quality,
        libraryPath: libraryPath,
        isWebp: false,
      ),
    );
  }

  /// Same contract as [encodeJpegNative], producing a lossy WebP (alpha
  /// preserved). Throws [CeyxEncodeException] with
  /// [CeyxEncodeErrorCode.unsupported] when the native build was configured
  /// without libwebp.
  Future<Uint8List> encodeWebpNative(
    Uint8List rgba, {
    required int width,
    required int height,
    required int quality,
  }) {
    final libraryPath = _libraryPath;
    return Isolate.run(
      () => _encodeOnWorker(
        rgba,
        width: width,
        height: height,
        quality: quality,
        libraryPath: libraryPath,
        isWebp: true,
      ),
    );
  }

  /// Worker-isolate entry point. Static so [Isolate.run] does not capture
  /// parent-isolate state (matches [DngDecoderService._decodeFileToTransferable]).
  static Uint8List _encodeOnWorker(
    Uint8List rgba, {
    required int width,
    required int height,
    required int quality,
    required String? libraryPath,
    required bool isWebp,
  }) {
    final lib = libraryPath == null
        ? DngNativeBindings.load().library
        : DngNativeBindings.fromPath(libraryPath).library;
    final bindings = CeyxEncodeBindings.fromLibrary(lib);
    if (!bindings.available) {
      throw CeyxEncodeUnavailableException();
    }

    final rgbaPtr = malloc<ffi.Uint8>(rgba.length);
    rgbaPtr.asTypedList(rgba.length).setAll(0, rgba);
    final outPtr = calloc<ffi.Pointer<ffi.Uint8>>();
    final outLenPtr = calloc<ffi.Size>();

    try {
      final encode = isWebp ? bindings.webp : bindings.jpeg;
      final result = encode(
        rgbaPtr,
        width,
        height,
        quality,
        outPtr,
        outLenPtr,
      );

      if (result != CeyxEncodeErrorCode.success) {
        // Contract: *out is NULL and *out_len is 0 on failure, so nothing to
        // free here.
        throw CeyxEncodeException(result, bindings.errorName(result));
      }

      final buffer = outPtr.value;
      final len = outLenPtr.value;
      if (buffer == ffi.nullptr || len == 0) {
        throw CeyxEncodeException(
          CeyxEncodeErrorCode.encodeFailed,
          bindings.errorName(CeyxEncodeErrorCode.encodeFailed),
        );
      }

      try {
        return Uint8List.fromList(buffer.asTypedList(len));
      } finally {
        bindings.free(buffer);
      }
    } finally {
      malloc.free(rgbaPtr);
      calloc.free(outPtr);
      calloc.free(outLenPtr);
    }
  }
}
