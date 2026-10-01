// Shared worker-isolate body of HeifDecoderService and CeyxStillDecoderService
// (T8, 2026-10-02 techdebt campaign).
//
// Library-internal: ceyx.dart exports nothing from this file. The two public
// services stay thin facades that keep their own names, signatures, exception
// types and null-on-failure contracts; only the mechanics they used to repeat
// line for line live here (dylib/bindings loading, the metadata probe, the
// native decode + copy + release sequence).
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'dng_bindings.dart';
import 'encode_options.dart' show CeyxStillResult;

/// Extent + orientation of a still image's primary item. Structurally the
/// same record type as the public `HeifProbeResult` and `CeyxStillProbe`.
typedef StillImageExtent = ({int width, int height, int orientation});

/// A route's metadata probe, already bound to its native symbol.
typedef StillProbeCall =
    int Function(
      Pointer<Utf8> path,
      Pointer<Uint32> width,
      Pointer<Uint32> height,
      Pointer<Int32> orientation,
    );

/// A route's decode, already bound to its native symbol.
typedef StillDecodeCall =
    int Function(
      Pointer<Utf8> path,
      int maxDimension,
      Pointer<CeyxStillResult> out,
    );

/// A route's release, already bound to its native symbol.
typedef StillReleaseCall = void Function(Pointer<CeyxStillResult> out);

/// Resolves one route's bindings from the SAME library `DngNativeBindings`
/// loads, so the package has one dylib search order. Never throws: a missing
/// dylib or a missing symbol both yield null, because one route being
/// undecodable must not break the others.
TBindings? loadStillRouteBindings<TBindings>(
  String? libraryPath,
  TBindings Function(DynamicLibrary library) fromLibrary,
) {
  try {
    final dng = libraryPath == null
        ? DngNativeBindings.load()
        : DngNativeBindings.fromPath(libraryPath);
    return fromLibrary(dng.library);
  } catch (_) {
    return null;
  }
}

/// Metadata-only probe through [probe]. Returns null, never throws, when the
/// native side reports anything other than [successCode] or a zero extent.
StillImageExtent? probeStillImageExtent(
  String path,
  StillProbeCall probe,
  int successCode,
) {
  final pathPointer = path.toNativeUtf8();
  final width = calloc<Uint32>();
  final height = calloc<Uint32>();
  final orientation = calloc<Int32>();
  try {
    final returnCode = probe(pathPointer, width, height, orientation);
    if (returnCode != successCode) return null;
    if (width.value == 0 || height.value == 0) return null;
    return (
      width: width.value,
      height: height.value,
      orientation: orientation.value,
    );
  } finally {
    malloc.free(pathPointer);
    calloc.free(width);
    calloc.free(height);
    calloc.free(orientation);
  }
}

/// What one native decode produced, computed inside the worker isolate. Each
/// failure kind is distinct so each facade keeps its own error contract.
sealed class StillDecodeOutcome {
  const StillDecodeOutcome();
}

/// Dart-owned RGBA8 pixels, ready to cross the isolate boundary.
final class StillDecodePixels extends StillDecodeOutcome {
  const StillDecodePixels({
    required this.rgba,
    required this.width,
    required this.height,
    required this.orientation,
  });

  final TransferableTypedData rgba;
  final int width;
  final int height;
  final int orientation;
}

/// The native decode returned [code] instead of the route's success code.
final class StillDecodeNativeError extends StillDecodeOutcome {
  const StillDecodeNativeError(this.code);

  final int code;
}

/// The native decode reported success but returned no pixel buffer.
final class StillDecodeNullBuffer extends StillDecodeOutcome {
  const StillDecodeNullBuffer();
}

/// The native buffer length disagrees with width * height * 4.
final class StillDecodeLengthMismatch extends StillDecodeOutcome {
  const StillDecodeLengthMismatch({
    required this.rgbaLength,
    required this.expectedLength,
  });

  final int rgbaLength;
  final int expectedLength;
}

/// Runs one native decode into a caller-owned result struct and copies the
/// pixels into Dart-owned memory.
///
/// Checks run in the order both services always used: native return code,
/// then a null/empty buffer, then length == width * height * 4. Release order
/// matters: the native release frees the buffer and zeroes the struct, then
/// the struct itself is freed — freeing the struct first would leak the
/// buffer.
///
/// [CeyxStillResult] serves both routes: it is byte-identical to `HeifResult`
/// by contract (encode_options.dart), both layouts are pinned by the plugin's
/// layout tests, and the HEIF facade casts the pointer it hands its bindings.
StillDecodeOutcome decodeStillImage(
  String path,
  int maxDimension,
  StillDecodeCall decode,
  StillReleaseCall release,
  int successCode,
) {
  final pathPointer = path.toNativeUtf8();
  final out = calloc<CeyxStillResult>();
  try {
    final returnCode = decode(pathPointer, maxDimension, out);
    final result = out.ref;
    if (returnCode != successCode) return StillDecodeNativeError(returnCode);
    if (result.rgba == nullptr || result.rgbaLen <= 0) {
      return const StillDecodeNullBuffer();
    }
    final expected = result.width * result.height * 4;
    if (result.rgbaLen != expected) {
      return StillDecodeLengthMismatch(
        rgbaLength: result.rgbaLen,
        expectedLength: expected,
      );
    }
    // Copy into Dart-owned bytes: TransferableTypedData cannot carry a
    // native-backed typed list across an isolate boundary safely.
    final copy = Uint8List.fromList(result.rgba.asTypedList(result.rgbaLen));
    return StillDecodePixels(
      rgba: TransferableTypedData.fromList([copy]),
      width: result.width,
      height: result.height,
      orientation: result.orientation,
    );
  } finally {
    release(out);
    calloc.free(out);
    malloc.free(pathPointer);
  }
}
