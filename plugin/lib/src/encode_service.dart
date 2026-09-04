import 'dart:ffi' as ffi;
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:meta/meta.dart';

import 'codec_format.dart';
import 'dng_bindings.dart';
import 'encode_bindings.dart';
import 'encode_bindings_v2.dart';
import 'encode_options.dart';

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
///
/// `rgba` crosses the isolate boundary as a [TransferableTypedData] rather
/// than as a raw [Uint8List] captured by the [Isolate.run] closure. This
/// matters because sending a live, still-mutable [Uint8List] as part of an
/// isolate message requires the VM to copy it under an isolate-group-wide
/// safepoint (every isolate in the group is paused while the copy happens).
/// [TransferableTypedData.fromList] performs its copy synchronously on the
/// calling isolate, *before* any cross-isolate message is sent, so the
/// message itself carries only a cheap transferable handle — no
/// group safepoint on the hot path. Note this is NOT a zero-copy transfer:
/// [TransferableTypedData.fromList] still copies bytes once (the caller's
/// `rgba` remains valid and unmodified afterwards, since `fromList` does not
/// detach/neuter the source list), it just avoids paying for that copy
/// through the isolate-message safepoint mechanism. The encoded result is
/// transferred back the same way. The returned [Uint8List] is fully
/// Dart-owned — no native pointer survives the call, so no [Finalizable]
/// bookkeeping is needed here (contrast [DngDecoderService]'s zero-copy
/// decode path).
class CeyxEncodeService {
  /// Optional explicit dylib path, bypassing the platform candidate search
  /// in [DngNativeBindings.load]. Primarily for tests.
  final String? _libraryPath;

  /// Memoized [CeyxEncodeUnavailableException] outcomes, keyed by the
  /// resolved library path (or `null` for the platform default search).
  ///
  /// Without this, every encode call on a dylib lacking the encode symbols
  /// paid a fresh [Isolate.run] spawn + dylib-load attempt before degrading
  /// into the caller's fallback (reviewer nit #5, 2026-08-30 P13 review). A
  /// dylib's symbol set cannot change mid-process, so once a given
  /// [libraryPath] has been observed unavailable, every subsequent call for
  /// that same path can fail fast without paying the probe again. Only the
  /// unavailable outcome is cached: successful encodes still run on a fresh
  /// worker isolate every time, matching the existing off-UI-isolate
  /// contract.
  static final Map<String?, CeyxEncodeUnavailableException>
  _unavailableCache = {};

  CeyxEncodeService({String? libraryPath}) : _libraryPath = libraryPath;

  /// Test-only: clears the memoized unavailability cache so test cases don't
  /// leak state into one another.
  @visibleForTesting
  static void resetAvailabilityCacheForTesting() => _unavailableCache.clear();

  /// Test-only: seeds the memoized-unavailable cache for [libraryPath]
  /// without needing a dylib actually built without the encode symbols, so
  /// the memoization short-circuit can be exercised directly.
  @visibleForTesting
  static void debugMarkUnavailableForTesting(String? libraryPath) {
    _unavailableCache[libraryPath] = CeyxEncodeUnavailableException();
  }

  /// Test-only: counts how many times an encode call actually spawned the
  /// worker isolate (as opposed to short-circuiting from
  /// [_unavailableCache]). Not reset automatically — callers should read the
  /// delta across a test case.
  @visibleForTesting
  static int debugIsolateSpawnCount = 0;

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
    return _encode(
      rgba,
      width: width,
      height: height,
      quality: quality,
      isWebp: false,
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
    return _encode(
      rgba,
      width: width,
      height: height,
      quality: quality,
      isWebp: true,
    );
  }

  Future<Uint8List> _encode(
    Uint8List rgba, {
    required int width,
    required int height,
    required int quality,
    required bool isWebp,
  }) async {
    final libraryPath = _libraryPath;
    final memoized = _unavailableCache[libraryPath];
    if (memoized != null) {
      throw memoized;
    }

    debugIsolateSpawnCount++;
    final transferableRgba = TransferableTypedData.fromList([rgba]);
    try {
      final transferableResult = await Isolate.run(
        () => _encodeOnWorker(
          transferableRgba,
          width: width,
          height: height,
          quality: quality,
          libraryPath: libraryPath,
          isWebp: isWebp,
        ),
      );
      return transferableResult.materialize().asUint8List();
    } on CeyxEncodeUnavailableException catch (e) {
      _unavailableCache[libraryPath] = e;
      rethrow;
    }
  }

  /// Worker-isolate entry point. Static so [Isolate.run] does not capture
  /// parent-isolate state (matches [DngDecoderService._decodeFileToTransferable]).
  ///
  /// Takes and returns [TransferableTypedData] rather than [Uint8List] so
  /// both the input pixels and the encoded output cross the isolate
  /// boundary without paying an isolate-message safepoint copy (see the
  /// class dartdoc).
  static TransferableTypedData _encodeOnWorker(
    TransferableTypedData rgbaTransfer, {
    required int width,
    required int height,
    required int quality,
    required String? libraryPath,
    required bool isWebp,
  }) {
    final rgba = rgbaTransfer.materialize().asUint8List();
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
        final encoded = Uint8List.fromList(buffer.asTypedList(len));
        return TransferableTypedData.fromList([encoded]);
      } finally {
        bindings.free(buffer);
      }
    } finally {
      malloc.free(rgbaPtr);
      calloc.free(outPtr);
      calloc.free(outLenPtr);
    }
  }

  /// Encodes `rgba` as [format]. Runs on a worker isolate, like the two
  /// legacy entry points.
  ///
  /// Throws [CeyxEncodeUnavailableException] when the dylib lacks the generic
  /// symbols (an older drop), and [CeyxEncodeException] with
  /// [CeyxEncodeErrorCode.unsupported] when the symbol exists but the codec
  /// was excluded from this platform's build. Those are DIFFERENT states and
  /// Halcyon needs the distinction to decide whether to offer a format at all.
  Future<Uint8List> encodeNative(
    Uint8List rgba, {
    required CeyxImageFormat format,
    required int width,
    required int height,
    int quality = 90,
    bool lossless = false,
    int effort = 0,
    Uint8List? exif,
    Uint8List? xmp,
    Uint8List? icc,
  }) async {
    final libraryPath = _libraryPath;
    final memoized = _unavailableCache[libraryPath];
    if (memoized != null) throw memoized;

    debugIsolateSpawnCount++;
    final transferableRgba = TransferableTypedData.fromList([rgba]);
    try {
      final transferableResult = await Isolate.run(
        () => _encodeGenericOnWorker(
          transferableRgba,
          format: format.value,
          width: width,
          height: height,
          quality: quality,
          lossless: lossless,
          effort: effort,
          exif: exif,
          xmp: xmp,
          icc: icc,
          libraryPath: libraryPath,
        ),
      );
      return transferableResult.materialize().asUint8List();
    } on CeyxEncodeUnavailableException catch (e) {
      _unavailableCache[libraryPath] = e;
      rethrow;
    }
  }

  /// True when this build can encode [format]. Cheap: one native call, no
  /// encode. Returns false rather than throwing when the symbol is absent.
  Future<bool> supports(CeyxImageFormat format) async {
    final libraryPath = _libraryPath;
    if (_unavailableCache[libraryPath] != null) return false;
    try {
      return await Isolate.run(() {
        final lib = libraryPath == null
            ? DngNativeBindings.load().library
            : DngNativeBindings.fromPath(libraryPath).library;
        final b = CeyxEncodeV2Bindings.fromLibrary(lib);
        if (!b.available) return false;
        return b.supports(format.value) == 1;
      });
    } catch (_) {
      return false;
    }
  }

  /// Worker-isolate entry point for [encodeNative]. Static so [Isolate.run]
  /// does not capture parent-isolate state (matches [_encodeOnWorker]).
  ///
  /// Allocates [CeyxEncodeOptions]/[CeyxEncodeMetadata] with `calloc`, sets
  /// `structSize` to `sizeOf<...>()`, copies the metadata buffers into native
  /// memory, calls through, copies the result into a Dart-owned [Uint8List],
  /// and frees everything in a `finally` -- including the native output
  /// buffer via [CeyxEncodeBindings.free], which is reused rather than
  /// re-looked-up.
  static TransferableTypedData _encodeGenericOnWorker(
    TransferableTypedData rgbaTransfer, {
    required int format,
    required int width,
    required int height,
    required int quality,
    required bool lossless,
    required int effort,
    required Uint8List? exif,
    required Uint8List? xmp,
    required Uint8List? icc,
    required String? libraryPath,
  }) {
    final rgba = rgbaTransfer.materialize().asUint8List();
    final lib = libraryPath == null
        ? DngNativeBindings.load().library
        : DngNativeBindings.fromPath(libraryPath).library;
    final legacy = CeyxEncodeBindings.fromLibrary(lib);
    final v2 = CeyxEncodeV2Bindings.fromLibrary(lib);
    if (!v2.available) {
      throw CeyxEncodeUnavailableException();
    }

    final rgbaPtr = malloc<ffi.Uint8>(rgba.length);
    rgbaPtr.asTypedList(rgba.length).setAll(0, rgba);

    final optsPtr = calloc<CeyxEncodeOptions>();
    optsPtr.ref
      ..structSize = ffi.sizeOf<CeyxEncodeOptions>()
      ..quality = quality
      ..lossless = lossless ? 1 : 0
      ..effort = effort
      ..reserved0 = 0;

    final metaPtr = calloc<CeyxEncodeMetadata>();
    ffi.Pointer<ffi.Uint8> exifPtr = ffi.nullptr;
    ffi.Pointer<ffi.Uint8> xmpPtr = ffi.nullptr;
    ffi.Pointer<ffi.Uint8> iccPtr = ffi.nullptr;
    if (exif != null && exif.isNotEmpty) {
      exifPtr = malloc<ffi.Uint8>(exif.length);
      exifPtr.asTypedList(exif.length).setAll(0, exif);
    }
    if (xmp != null && xmp.isNotEmpty) {
      xmpPtr = malloc<ffi.Uint8>(xmp.length);
      xmpPtr.asTypedList(xmp.length).setAll(0, xmp);
    }
    if (icc != null && icc.isNotEmpty) {
      iccPtr = malloc<ffi.Uint8>(icc.length);
      iccPtr.asTypedList(icc.length).setAll(0, icc);
    }
    metaPtr.ref
      ..structSize = ffi.sizeOf<CeyxEncodeMetadata>()
      ..exif = exifPtr
      ..exifLen = exif?.length ?? 0
      ..xmp = xmpPtr
      ..xmpLen = xmp?.length ?? 0
      ..icc = iccPtr
      ..iccLen = icc?.length ?? 0;

    final outPtr = calloc<ffi.Pointer<ffi.Uint8>>();
    final outLenPtr = calloc<ffi.Size>();

    try {
      final result = v2.encode(
        format,
        rgbaPtr,
        width,
        height,
        optsPtr,
        metaPtr,
        outPtr,
        outLenPtr,
      );

      if (result != CeyxEncodeErrorCode.success) {
        // Contract: *out is NULL and *out_len is 0 on failure, so nothing to
        // free here.
        final name = legacy.available
            ? legacy.errorName(result)
            : 'code:$result';
        throw CeyxEncodeException(result, name);
      }

      final buffer = outPtr.value;
      final len = outLenPtr.value;
      if (buffer == ffi.nullptr || len == 0) {
        throw CeyxEncodeException(
          CeyxEncodeErrorCode.encodeFailed,
          legacy.available
              ? legacy.errorName(CeyxEncodeErrorCode.encodeFailed)
              : 'code:${CeyxEncodeErrorCode.encodeFailed}',
        );
      }

      try {
        final encoded = Uint8List.fromList(buffer.asTypedList(len));
        return TransferableTypedData.fromList([encoded]);
      } finally {
        if (legacy.available) {
          legacy.free(buffer);
        }
      }
    } finally {
      malloc.free(rgbaPtr);
      if (exifPtr != ffi.nullptr) malloc.free(exifPtr);
      if (xmpPtr != ffi.nullptr) malloc.free(xmpPtr);
      if (iccPtr != ffi.nullptr) malloc.free(iccPtr);
      calloc.free(optsPtr);
      calloc.free(metaPtr);
      calloc.free(outPtr);
      calloc.free(outLenPtr);
    }
  }
}
