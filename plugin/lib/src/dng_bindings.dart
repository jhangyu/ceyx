import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:ffi/ffi.dart';

import 'raw_bindings.dart';

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
// WP5: the standalone RGBA free entry's typedef is gone with the symbol -- it had zero
// remaining callers. The two allocating-decode typedefs are RETAINED, for the
// same reason RawDecodeAndProcess* is: the current dylib no longer exports
// these entries, but several tests load PINNED OLD dylibs that do, and these
// typedefs are how such a dylib is described. A typedef describes a shape, not
// a dependency.
typedef DngDecodeAndProcessNative =
    ffi.Pointer<DngResult> Function(ffi.Pointer<Utf8> filePath);
typedef DngDecodeAndProcessDart =
    ffi.Pointer<DngResult> Function(ffi.Pointer<Utf8> filePath);

typedef DngDecodeAndProcessSizedNative =
    ffi.Pointer<DngResult> Function(
      ffi.Pointer<Utf8> filePath,
      ffi.Int32 maxDim,
    );
typedef DngDecodeAndProcessSizedDart =
    ffi.Pointer<DngResult> Function(ffi.Pointer<Utf8> filePath, int maxDim);

typedef DngFreeResultNative = ffi.Void Function(ffi.Pointer<DngResult> result);
typedef DngFreeResultDart = void Function(ffi.Pointer<DngResult> result);

// Generic RAW entry (Phase 17 native, Phase 18 binding). Reuses the FROZEN
// DngResult layout, so no struct change is needed. max_dim <= 0 means full
// resolution (dng_ffi_api.h:114, raw_ffi_api.cpp:24). Additive export: older
// dylibs lack it, so the lookup MUST be guarded.
typedef RawDecodeAndProcessNative =
    ffi.Pointer<DngResult> Function(
      ffi.Pointer<Utf8> filePath,
      ffi.Int32 maxDim,
    );
typedef RawDecodeAndProcessDart =
    ffi.Pointer<DngResult> Function(ffi.Pointer<Utf8> filePath, int maxDim);

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

typedef DngExtractPreviewJpegNative =
    ffi.Int32 Function(
      ffi.Pointer<Utf8> filePath,
      ffi.Pointer<ffi.Pointer<ffi.Uint8>> outBuffer,
      ffi.Pointer<ffi.Int32> outSize,
    );
typedef DngExtractPreviewJpegDart =
    int Function(
      ffi.Pointer<Utf8> filePath,
      ffi.Pointer<ffi.Pointer<ffi.Uint8>> outBuffer,
      ffi.Pointer<ffi.Int32> outSize,
    );

typedef DngFreeBufferNative = ffi.Void Function(ffi.Pointer<ffi.Uint8> buffer);
typedef DngFreeBufferDart = void Function(ffi.Pointer<ffi.Uint8> buffer);

// R4 item 1: native decode-slot configuration. ADDITIVE — absent from every
// dylib built before 2026-09-05, so these lookups MUST be guarded (Halcyon
// pins a ceyx release whose decoder predates them; an unguarded lookup would
// throw in the constructor and kill ALL decoding, not just slot config).
typedef DngDecodeConfigureSlotsNative = ffi.Int32 Function(ffi.Int32 requested);
typedef DngDecodeConfigureSlotsDart = int Function(int requested);

typedef DngDecodeConfiguredSlotsNative = ffi.Int32 Function();
typedef DngDecodeConfiguredSlotsDart = int Function();

typedef DngDecodeRecommendedSlotsNative = ffi.Int32 Function(ffi.Int64 pixels);
typedef DngDecodeRecommendedSlotsDart = int Function(int pixels);

typedef DngDecodeRecommendationClassPixelsNative =
    ffi.Int64 Function(ffi.Int32 index);
typedef DngDecodeRecommendationClassPixelsDart = int Function(int index);

// R4 WP10 (AMENDMENT 3): ONE format-agnostic probe/decode-into pair.
//
// ADDITIVE — absent from every dylib built before 2026-09-06 (including the
// release Halcyon currently pins), so the lookup MUST be guarded like the slot
// group above. Signatures are FROZEN by plan A3.3; the native half is written
// against the same text.
//
// One pair, not two: the entries live in an ALWAYS-COMPILED translation unit
// (`native/src/ffi/ceyx_decode_into_ffi.cpp`, which no cmake EXCLUDE filter
// names), and the generic-RAW arm inside them is guarded in-source. So both
// symbols exist in every build configuration — including
// DNG_ENABLE_GENERIC_RAW=OFF, where a RAW input gets a clean
// kCeyxErrFormatUnsupportedInBuild instead of a missing symbol. That is what
// makes ONE availability flag correct here: the probe and the decode fail
// TOGETHER for a format this build cannot handle, so there is no state in
// which the pool believes a slot will be used and the worker ignores it.
typedef CeyxProbeOutputSizeNative =
    ffi.Int32 Function(
      ffi.Pointer<Utf8> filePath,
      ffi.Int32 maxDim,
      ffi.Pointer<ffi.Int32> outWidth,
      ffi.Pointer<ffi.Int32> outHeight,
    );
typedef CeyxProbeOutputSizeDart =
    int Function(
      ffi.Pointer<Utf8> filePath,
      int maxDim,
      ffi.Pointer<ffi.Int32> outWidth,
      ffi.Pointer<ffi.Int32> outHeight,
    );

typedef CeyxDecodeIntoBufferNative =
    ffi.Pointer<DngResult> Function(
      ffi.Pointer<Utf8> filePath,
      ffi.Int32 maxDim,
      ffi.Pointer<ffi.Uint8> dst,
      ffi.Size dstCapacity,
    );
typedef CeyxDecodeIntoBufferDart =
    ffi.Pointer<DngResult> Function(
      ffi.Pointer<Utf8> filePath,
      int maxDim,
      ffi.Pointer<ffi.Uint8> dst,
      int dstCapacity,
    );

// Native-rotation spec Task 3: orientation-aware sibling of
// ceyx_decode_into_buffer (spec §1.3, native-rotation-spec.md). ADDITIVE and
// resolved with its OWN guarded lookup, per-symbol — NOT folded into the
// `decodeIntoBufferAvailable` group above. This mirrors the pattern that
// group's own doc comment warns about: a dylib may ship
// `ceyx_decode_into_buffer` without yet shipping the oriented sibling (Task 2
// lands after Task 3 in this campaign's sequencing), and Halcyon's own
// lessons-learned records a guarded-lookup family that nulled an entire group
// when one symbol was missing, silently shipping an absent feature.
typedef CeyxDecodeIntoBufferOrientedNative =
    ffi.Pointer<DngResult> Function(
      ffi.Pointer<Utf8> filePath,
      ffi.Int32 maxDim,
      ffi.Pointer<ffi.Uint8> dst,
      ffi.Size dstCapacity,
      ffi.Int32 exifOrientation,
    );
typedef CeyxDecodeIntoBufferOrientedDart =
    ffi.Pointer<DngResult> Function(
      ffi.Pointer<Utf8> filePath,
      int maxDim,
      ffi.Pointer<ffi.Uint8> dst,
      int dstCapacity,
      int exifOrientation,
    );

/// Bindings to the native dng_decoder_native library
class DngNativeBindings {
  final ffi.DynamicLibrary _lib;

  // WP5: the current dylib no longer exports the allocating decode entries,
  // the standalone RGBA free, or the native pool gauge. Production decoding
  // goes through the decode-into pair, and the process-wide "nothing leaked"
  // gauge is CeyxNativeBufferPool.debugTotalLiveAddresses on this side.
  //
  // The lookups below are RETAINED but are now GUARDED (nullable) rather than
  // unguarded `late final`. Two reasons, both load-bearing:
  //   1. Several tests load PINNED OLD dylibs that still export these symbols,
  //      and these lookups are how an old dylib is described. Deleting an
  //      export is not the same as deleting the ability to describe one --
  //      exactly the rule already applied to the guarded RAW entry below.
  //   2. The unguarded legacy-DNG lookup used to throw inside
  //      this constructor when the symbol was missing, killing ALL decoding.
  //      That fragility is what made the native and Dart halves of this work
  //      package a single indivisible commit; guarding it removes the trap
  //      rather than merely stepping around it.
  // No production code path calls either entry; they are capability probes.
  DngDecodeAndProcessDart? _dngDecodeAndProcess;
  DngDecodeAndProcessSizedDart? _dngDecodeAndProcessSized;
  DngDebugPoolCheckedOutDart? _dngDebugPoolCheckedOut;

  // Guarded RAW entries — null when the loaded dylib predates Phase 17 or was
  // built with -DDNG_ENABLE_GENERIC_RAW=OFF.
  //
  // `rawDecodeAndProcess` is deliberately RETAINED even though WP5 deleted the
  // export: raw_bindings_layout_test.dart loads a PINNED OLD dylib that still
  // has it, and this guarded lookup is how that dylib is described. Deleting an
  // export is not the same as deleting the ability to describe an older one.
  RawDecodeAndProcessDart? _rawDecodeAndProcess;
  RawLastDiagnosticsDart? _rawLastDiagnostics;

  // R4 item 1: guarded slot-configuration entries. Null together — they ship
  // as one group, so a dylib exposing some but not all is a corrupt build and
  // degrades to "unsupported" rather than half-working.
  DngDecodeConfigureSlotsDart? _dngDecodeConfigureSlots;
  DngDecodeConfiguredSlotsDart? _dngDecodeConfiguredSlots;
  DngDecodeRecommendedSlotsDart? _dngDecodeRecommendedSlots;
  DngDecodeRecommendationClassPixelsDart? _dngDecodeRecommendationClassPixels;

  // R4 WP10: the guarded format-agnostic probe + decode-into pair. Null
  // TOGETHER — they ship in one commit, so a dylib exposing one but not the
  // other is a corrupt build and degrades to "unsupported" rather than
  // half-working (a resolved decode-into with an absent probe would size every
  // slot by guesswork).
  CeyxProbeOutputSizeDart? _ceyxProbeOutputSize;
  CeyxDecodeIntoBufferDart? _ceyxDecodeIntoBuffer;

  // Native-rotation spec Task 3: guarded PER-SYMBOL, independent of the pair
  // above — see the typedef comment for why this must not be folded in.
  CeyxDecodeIntoBufferOrientedDart? _ceyxDecodeIntoBufferOriented;

  late final DngDecoderWarmupForSizeDart dngDecoderWarmupForSize;
  // R3-3: pipeline cache persistence controls.
  late final DngDecoderSetPipelineCachePathDart dngDecoderSetPipelineCachePath;
  late final DngDecoderSavePipelineCacheDart dngDecoderSavePipelineCache;
  late final DngDecoderPipelineCacheStatusDart dngDecoderPipelineCacheStatus;
  late final DngFreeResultDart dngFreeResult;

  late final DngExtractPreviewJpegDart extractPreviewJpeg;
  late final DngFreeBufferDart freeBuffer;

  /// Pointer to the C `dng_free_result` function for NativeFinalizer (if we were finalizing the whole result)
  late final ffi.Pointer<ffi.NativeFunction<DngFreeResultNative>>
  dngFreeResultPtr;

  /// Guarded access to the generic RAW entry. Null when the loaded dylib does
  /// not export the legacy allocating RAW entry.
  RawDecodeAndProcessDart? get rawDecodeAndProcess => _rawDecodeAndProcess;

  /// Whether the loaded dylib exports the legacy allocating RAW entry.
  bool get rawDecodeAvailable => _rawDecodeAndProcess != null;

  /// Whether the loaded dylib exports `raw_last_diagnostics`.
  bool get rawDiagnosticsAvailable => _rawLastDiagnostics != null;

  /// Guarded access to the legacy allocating decode entry. Null on any dylib
  /// built after WP5 retired it; non-null only for a pinned older dylib.
  DngDecodeAndProcessDart? get dngDecodeAndProcess => _dngDecodeAndProcess;

  /// Guarded access to the legacy allocating sized-decode entry. Null on any
  /// dylib built after WP5 retired it.
  DngDecodeAndProcessSizedDart? get dngDecodeAndProcessSized =>
      _dngDecodeAndProcessSized;

  /// Whether the loaded dylib exports the legacy sized-decode entry.
  /// WP5: false for every current build; a capability report about the loaded
  /// image, not a switch any decode path consults.
  bool get sizedDecodeAvailable => _dngDecodeAndProcessSized != null;

  /// Whether the loaded dylib exports the native pool gauge.
  /// WP5: false for every current build. The live gauge is
  /// CeyxNativeBufferPool.debugTotalLiveAddresses.
  bool get poolStatsAvailable => _dngDebugPoolCheckedOut != null;

  /// Native RGBA pool buffers currently checked out. Null on every current
  /// build, because the native pool it counted no longer exists.
  int? poolCheckedOut() => _dngDebugPoolCheckedOut?.call();

  /// Guarded access to the R4 item 1 slot-configuration entry. Null when the
  /// loaded dylib predates the configurable native slot cap.
  DngDecodeConfigureSlotsDart? get dngDecodeConfigureSlots =>
      _dngDecodeConfigureSlots;

  /// Whether this library exposes the configurable native slot cap.
  bool get slotConfigAvailable => _dngDecodeConfigureSlots != null;

  /// Guarded access to the R4 WP10 metadata-only output-extent probe. Null
  /// when the loaded dylib predates the entry. Format-agnostic: the native
  /// side routes DNG vs generic-RAW internally.
  CeyxProbeOutputSizeDart? get ceyxProbeOutputSize => _ceyxProbeOutputSize;

  /// Guarded access to the R4 WP10 decode-into-caller-buffer entry. Null when
  /// the loaded dylib predates the entry. Format-agnostic, as above.
  CeyxDecodeIntoBufferDart? get ceyxDecodeIntoBuffer => _ceyxDecodeIntoBuffer;

  /// True only when BOTH WP10 symbols resolved. Partial availability is a
  /// corrupt build and reports as unsupported, so the host falls back to the
  /// allocating decode route as a whole rather than half-way through it.
  ///
  /// ONE flag for every format, because there is one symbol pair for every
  /// format. A build that cannot decode generic RAW still EXPORTS both symbols
  /// and answers a RAW input with `kCeyxErrFormatUnsupportedInBuild` from the
  /// PROBE — so that case is handled by the probe returning no extent, not by
  /// a second availability flag.
  bool get decodeIntoBufferAvailable =>
      _ceyxProbeOutputSize != null && _ceyxDecodeIntoBuffer != null;

  /// Guarded access to the native-rotation `ceyx_decode_into_buffer_oriented`
  /// entry. Null when the loaded dylib predates it. Resolved independently of
  /// [decodeIntoBufferAvailable] on purpose (see the typedef comment above) —
  /// a missing oriented symbol must never null out the unoriented group.
  CeyxDecodeIntoBufferOrientedDart? get ceyxDecodeIntoBufferOriented =>
      _ceyxDecodeIntoBufferOriented;

  /// Whether the loaded dylib exports `ceyx_decode_into_buffer_oriented`.
  /// Independent of [decodeIntoBufferAvailable]: a build may have the
  /// unoriented entry without (yet) having the oriented sibling.
  bool get decodeIntoBufferOrientedAvailable =>
      _ceyxDecodeIntoBufferOriented != null;

  /// The slot count the native layer is currently configured for, or null when
  /// the dylib predates the entry.
  int? configuredSlots() => _dngDecodeConfiguredSlots?.call();

  /// ADVISORY (ruling r-6): slots this machine is recommended to run for a
  /// frame of [pixels]; pass 0 for the default 61 MP sizing frame. Null when
  /// unsupported. Nothing clamps against this — it is for display only.
  int? recommendedSlotsForPixels(int pixels) =>
      _dngDecodeRecommendedSlots?.call(pixels);

  /// Pixel count of recommendation class [index] (0 = 24 MP, 1 = 61 MP,
  /// 2 = 108 MP), so the host need not hardcode the frame sizes. Null when
  /// unsupported.
  int? recommendationClassPixels(int index) =>
      _dngDecodeRecommendationClassPixels?.call(index);

  /// Diagnostics for the most recent RAW decode observed
  /// on the current OS thread.
  ///
  /// Native state is `thread_local` (raw_ffi_api.cpp:19), NOT per-isolate.
  /// If a decode ran on a worker isolate, reading this from another isolate
  /// is unreliable in either direction — depending on OS thread reuse it may
  /// return null, the worker's values, or an earlier decode's values from
  /// this same thread. Provenance is not verifiable from Dart. A failed
  /// decode does not clear this state, so it can also surface an earlier
  /// successful decode's diagnostics. Returns null when the symbol is
  /// absent, or when native reports -1 (no decode has run on this thread
  /// yet).
  RawDiagnostics? lastRawDiagnostics() {
    final fn = _rawLastDiagnostics;
    if (fn == null) return null;
    final scratch = calloc<RawDecodeDiagnostics>();
    try {
      if (fn(scratch) != 0) return null;
      return RawDiagnostics.fromStruct(scratch.ref);
    } finally {
      calloc.free(scratch);
    }
  }

  /// The resolved native library, so sibling binding sets (HEIF) can attach to
  /// the SAME image instead of re-running the candidate search and possibly
  /// loading a different copy.
  ffi.DynamicLibrary get library => _lib;

  DngNativeBindings._(this._lib) {
    // WP5: guarded. Absent on every current dylib, present on the pinned old
    // dylibs the symbol-absence tests load.
    try {
      _dngDecodeAndProcess = _lib
          .lookupFunction<DngDecodeAndProcessNative, DngDecodeAndProcessDart>(
            'dng_decode_and_process',
          );
    } catch (_) {
      _dngDecodeAndProcess = null;
    }

    try {
      _dngDecodeAndProcessSized = _lib
          .lookupFunction<
            DngDecodeAndProcessSizedNative,
            DngDecodeAndProcessSizedDart
          >('dng_decode_and_process_sized');
    } catch (_) {
      _dngDecodeAndProcessSized = null;
    }

    try {
      _dngDebugPoolCheckedOut = _lib
          .lookupFunction<
            DngDebugPoolCheckedOutNative,
            DngDebugPoolCheckedOutDart
          >('dng_debug_pool_checked_out');
    } catch (_) {
      _dngDebugPoolCheckedOut = null;
    }

    try {
      _rawDecodeAndProcess = _lib
          .lookupFunction<RawDecodeAndProcessNative, RawDecodeAndProcessDart>(
            'raw_decode_and_process',
          );
    } catch (_) {
      // Symbol absent -> rawDecodeAvailable stays false and the service
      // throws RawUnavailableException instead of crashing.
      _rawDecodeAndProcess = null;
    }

    try {
      _rawLastDiagnostics = _lib
          .lookupFunction<RawLastDiagnosticsNative, RawLastDiagnosticsDart>(
            'raw_last_diagnostics',
          );
    } catch (_) {
      _rawLastDiagnostics = null;
    }

    // R4 item 1. One try block for all four on purpose: they are added by the
    // same commit and ship together, so partial availability means a corrupt
    // build. Degrading the whole group to "unsupported" is safer than letting
    // a caller configure the cap but be unable to read it back.
    try {
      _dngDecodeConfigureSlots = _lib
          .lookupFunction<
            DngDecodeConfigureSlotsNative,
            DngDecodeConfigureSlotsDart
          >('dng_decode_configure_slots');
      _dngDecodeConfiguredSlots = _lib
          .lookupFunction<
            DngDecodeConfiguredSlotsNative,
            DngDecodeConfiguredSlotsDart
          >('dng_decode_configured_slots');
      _dngDecodeRecommendedSlots = _lib
          .lookupFunction<
            DngDecodeRecommendedSlotsNative,
            DngDecodeRecommendedSlotsDart
          >('dng_decode_recommended_slots_for_pixels');
      _dngDecodeRecommendationClassPixels = _lib
          .lookupFunction<
            DngDecodeRecommendationClassPixelsNative,
            DngDecodeRecommendationClassPixelsDart
          >('dng_decode_recommendation_class_pixels');
    } catch (_) {
      _dngDecodeConfigureSlots = null;
      _dngDecodeConfiguredSlots = null;
      _dngDecodeRecommendedSlots = null;
      _dngDecodeRecommendationClassPixels = null;
    }

    // R4 WP10. One try block for both on purpose, same rule as the slot group
    // above: they are added by the same commit, so partial availability means
    // a corrupt build. Degrading the pair to "unsupported" keeps the pooled
    // decode route unreachable rather than half-wired — a slot marked in use
    // while the worker ignores its address is a leak that tests green.
    //
    // ONE group for every format (plan A3.1): these two live in an
    // always-compiled TU, so there is no build in which one format's entry is
    // present and another's is absent.
    try {
      _ceyxProbeOutputSize = _lib
          .lookupFunction<CeyxProbeOutputSizeNative, CeyxProbeOutputSizeDart>(
            'ceyx_probe_output_size',
          );
      _ceyxDecodeIntoBuffer = _lib
          .lookupFunction<CeyxDecodeIntoBufferNative, CeyxDecodeIntoBufferDart>(
            'ceyx_decode_into_buffer',
          );
    } catch (_) {
      _ceyxProbeOutputSize = null;
      _ceyxDecodeIntoBuffer = null;
    }

    // Native-rotation spec Task 3: its OWN try block, deliberately separate
    // from the pair above. A missing oriented symbol must not null out the
    // unoriented `ceyx_decode_into_buffer` group.
    try {
      _ceyxDecodeIntoBufferOriented = _lib
          .lookupFunction<
            CeyxDecodeIntoBufferOrientedNative,
            CeyxDecodeIntoBufferOrientedDart
          >('ceyx_decode_into_buffer_oriented');
    } catch (_) {
      _ceyxDecodeIntoBufferOriented = null;
    }

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

    dngFreeResultPtr = _lib.lookup<ffi.NativeFunction<DngFreeResultNative>>(
      'dng_free_result',
    );

    extractPreviewJpeg = _lib
        .lookup<ffi.NativeFunction<DngExtractPreviewJpegNative>>(
          'dng_extract_preview_jpeg',
        )
        .asFunction();
    freeBuffer = _lib
        .lookup<ffi.NativeFunction<DngFreeBufferNative>>('dng_free_buffer')
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

  /// Load bindings from an explicit dylib path, bypassing the
  /// platform-specific candidate search in [load]. Useful for host apps with
  /// non-standard library layouts, and for tests that need to exercise the
  /// guarded legacy sized-decode lookup against a specific dylib
  /// without depending on the app-bundle / script-relative search paths that
  /// only resolve at runtime.
  factory DngNativeBindings.fromPath(String path) =>
      DngNativeBindings._(_openFirst([path]));

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
      for (final symbol in const [
        'ceyx_decode_into_buffer',
        'dng_free_buffer',
      ]) {
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
      //      `ceyx` plugin pod (see plugin/README.md)
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
      // <repo>/app/bin/benchmark_*.dart → parent = <repo>/app/bin, and
      // native/ now sits at the repo root (2026-08-26 layout move), so the
      // repo root is TWO levels up: ../../native/{dist,build}.
      final scriptRelativeDist =
          File('$scriptParent/../../native/dist/libdng_decoder_native.dylib')
              .path;
      final scriptRelativeBuild =
          File('$scriptParent/../../native/build/libdng_decoder_native.dylib')
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
