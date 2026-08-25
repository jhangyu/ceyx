import 'dart:ffi' as ffi;

/// FFI mirror of the generic-RAW diagnostics contract.
///
/// Source of truth: `dng_processor/native/include/raw_pipeline_contract.h`
/// (struct `RawDecodeDiagnostics`, lines 189-202). Field ORDER and WIDTH are
/// the contract; `test/raw_bindings_layout_test.dart` asserts both the size
/// and live field values after a real decode, because a size check alone
/// cannot catch two adjacent same-width fields being swapped.
///
/// This file deliberately has no import of the sibling bindings file that
/// defines `DngResult`: the dependency runs one way only (that file imports
/// this one), so anything mentioning `DngResult` lives there.

/// C struct `RawDecodeDiagnostics`. 64 bytes on 64-bit targets.
final class RawDecodeDiagnostics extends ffi.Struct {
  @ffi.Int32()
  external int frontend;

  @ffi.Int32()
  external int unpackBackend;

  @ffi.Uint32()
  external int rawspeedFlags;

  @ffi.Uint32()
  external int rawspeedWarningBits;

  @ffi.Int32()
  external int sampleModel;

  @ffi.Uint32()
  external int cfaRepeatWidth;

  @ffi.Uint32()
  external int cfaRepeatHeight;

  @ffi.Int32()
  external int gpuBackend;

  @ffi.Double()
  external double rawUnpackMs;

  @ffi.Double()
  external double gpuProcessMs;

  @ffi.Double()
  external double totalMs;

  @ffi.Int64()
  external int rawRepackBytes;
}

/// Dart-owned copy of [RawDecodeDiagnostics]. Safe to keep after the native
/// scratch buffer has been freed, and safe to send across isolates.
class RawDiagnostics {
  final int frontend;
  final int unpackBackend;
  final int rawspeedFlags;
  final int rawspeedWarningBits;
  final int sampleModel;
  final int cfaRepeatWidth;
  final int cfaRepeatHeight;
  final int gpuBackend;
  final double rawUnpackMs;
  final double gpuProcessMs;
  final double totalMs;
  final int rawRepackBytes;

  const RawDiagnostics({
    required this.frontend,
    required this.unpackBackend,
    required this.rawspeedFlags,
    required this.rawspeedWarningBits,
    required this.sampleModel,
    required this.cfaRepeatWidth,
    required this.cfaRepeatHeight,
    required this.gpuBackend,
    required this.rawUnpackMs,
    required this.gpuProcessMs,
    required this.totalMs,
    required this.rawRepackBytes,
  });

  factory RawDiagnostics.fromStruct(RawDecodeDiagnostics s) => RawDiagnostics(
    frontend: s.frontend,
    unpackBackend: s.unpackBackend,
    rawspeedFlags: s.rawspeedFlags,
    rawspeedWarningBits: s.rawspeedWarningBits,
    sampleModel: s.sampleModel,
    cfaRepeatWidth: s.cfaRepeatWidth,
    cfaRepeatHeight: s.cfaRepeatHeight,
    gpuBackend: s.gpuBackend,
    rawUnpackMs: s.rawUnpackMs,
    gpuProcessMs: s.gpuProcessMs,
    totalMs: s.totalMs,
    rawRepackBytes: s.rawRepackBytes,
  );

  @override
  String toString() =>
      'RawDiagnostics(frontend=$frontend unpack_backend=$unpackBackend '
      'gpu=$gpuBackend sample_model=$sampleModel '
      'cfa=${cfaRepeatWidth}x$cfaRepeatHeight '
      'unpack_ms=$rawUnpackMs gpu_ms=$gpuProcessMs total_ms=$totalMs '
      'repack_bytes=$rawRepackBytes)';
}

/// Mirror of C enum `RawFrontend`.
abstract final class RawFrontend {
  static const int unknown = 0;
  static const int dngSdk = 1;
  static const int libraw = 2;
}

/// Mirror of C enum `RawDecoderBackend`.
abstract final class RawDecoderBackend {
  static const int unknown = 0;
  static const int dngSdk = 1;
  static const int rawSpeed3 = 2;
  static const int libRawNative = 3;
}

/// Mirror of C enum `RawGpuBackend`.
abstract final class RawGpuBackend {
  static const int none = 0;
  static const int metal = 1;
  static const int vulkan = 2;
}

/// Mirror of C enum `RawSampleModel`.
abstract final class RawSampleModel {
  static const int cfa = 0;
  static const int monochrome = 1;
  static const int linearRgb = 2;
  static const int linearYCbCr = 3;
  static const int layered = 4;
  static const int multiFrame = 5;
  static const int unknown = 6;
}

typedef RawLastDiagnosticsNative =
    ffi.Int32 Function(ffi.Pointer<RawDecodeDiagnostics> out);
typedef RawLastDiagnosticsDart =
    int Function(ffi.Pointer<RawDecodeDiagnostics> out);

// dng_debug_pool_checked_out returns C size_t -> ffi.Size.
typedef DngDebugPoolCheckedOutNative = ffi.Size Function();
typedef DngDebugPoolCheckedOutDart = int Function();
