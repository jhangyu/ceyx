import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';
import 'package:meta/meta.dart';

import 'dng_bindings.dart';
import 'native_buffer_pool.dart';
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

  /// Address of the native RGBA buffer [rgbaData] views, or 0 when the bytes
  /// are Dart-heap-owned (the legacy TransferableTypedData arm). Non-zero
  /// ONLY on the H2-A pointer-transfer path
  /// ([CeyxDecodePool]'s `_materialize`). The buffer's lifetime is still
  /// [rgbaData]'s lifetime (NativeFinalizer); an address holder MUST keep
  /// [rgbaData] reachable for as long as it uses the address.
  final int nativeAddress;

  /// WP6: how this frame's native buffer goes back to `CeyxNativeBufferPool`.
  /// Set by the pool path that allocated it; null for every Dart-heap-backed
  /// decode and for every buffer whose sole owner is a `NativeFinalizer`, where
  /// null means "nothing to release", never "leak".
  final void Function()? onReleaseToPool;

  bool _releasedToPool = false;

  /// Native-rotation spec Task 3: the EXIF orientation the native decoder
  /// has already applied to [rgbaData], or 1 (identity) when it applied
  /// none. Defaults to 1 so every pre-existing construction site keeps
  /// compiling unchanged (spec §1.3, AC-3.2).
  final int appliedOrientation;

  DngImage({
    required this.rgbaData,
    required this.width,
    required this.height,
    required this.decodeMs,
    required this.processMs,
    this.nativeAddress = 0,
    this.onReleaseToPool,
    this.appliedOrientation = 1,
  });

  /// Returns this frame's native buffer to the pool. Call at
  /// end-of-consumption. Idempotent, and a safe no-op when no pooled buffer
  /// backs this image. After this the [rgbaData] view MUST NOT be read.
  void releaseToPool() {
    if (_releasedToPool) return;
    _releasedToPool = true;
    onReleaseToPool?.call();
  }

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

  /// R4 WP10: the caller-provided destination buffer was null, zero-capacity,
  /// or shorter than width*height*4. Reported BEFORE any pixel work, with
  /// width/height filled in so the caller can re-acquire at the exact extent.
  static const int dstTooSmall = -9;

  /// R4 WP10: the output-extent probe reported success but produced a zero
  /// extent.
  static const int probeFailed = -10;
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

/// R4 WP10: Dart mirror of `enum CeyxDecodeIntoError`
/// (`native/include/ceyx_decode_into.h`), the format-agnostic decode-into
/// layer's own error scale.
///
/// Deliberately DISJOINT from both the DNG scale (0..-101,
/// `dng_error_codes.h`) and the RAW scale (-201..-212,
/// `raw_pipeline_contract.h:112-125`), so a Dart caller can tell WHICH layer
/// spoke without knowing which route the native side took. Any value change in
/// the C header MUST be reflected here, same rule [DngErrorCode] carries.
abstract final class CeyxDecodeIntoError {
  /// Caller buffer null, zero-capacity, or shorter than width*height*4.
  /// Reported before any pixel work, with the extent filled in.
  static const int dstTooSmall = -301;

  /// This build has no decoder for that format — the generic-RAW arm was
  /// compiled out (`DNG_ENABLE_GENERIC_RAW=OFF`). The SYMBOL still exists;
  /// only the format is unsupported, which is why availability is one flag and
  /// this is an ordinary error return rather than a missing lookup.
  static const int formatUnsupportedInBuild = -302;

  /// The probe reported success but produced a zero extent.
  static const int probeFailed = -303;
}

/// R4 WP10: the destination buffer handed to [DngDecoderService
/// .decodeIntoPointer] was too small. Carries the extent the NATIVE layer
/// reported, so the caller's retry is exact rather than a guess.
///
/// This is a cheap, self-healing miss: the native entry refuses before any
/// pixel work, so a stale size prediction costs one metadata parse.
class DngBufferTooSmallException implements Exception {
  DngBufferTooSmallException(this.width, this.height);

  final int width;
  final int height;

  /// Bytes the matching decode needs.
  int get requiredBytes => width * height * 4;

  @override
  String toString() =>
      'DngBufferTooSmallException(${width}x$height, '
      'requires $requiredBytes bytes)';
}

/// Productionization plan Task 9 (reconciliation 2/3): raised by
/// [DngDecoderService.selfVerifiedAppliedOrientation] when a transposing
/// request (EXIF 5/6/7/8) comes back with an extent that is NOT swapped
/// relative to the unoriented reference the caller supplied.
///
/// With the scratch-checkout degradation arm deleted (fusion applies
/// orientation on every route), there is no longer a benign reason for this
/// to happen — an unswapped extent on a transposing request now means the
/// GPU kernel silently failed to orient (spec §4.2's "runs on zeros /
/// does-nothing" silent-failure mode: dims correct, error code 0, timings
/// plausible). This is a CONTRACT VIOLATION, not a degradation signal, so it
/// is thrown rather than silently downgraded to `appliedOrientation = 1`.
class CeyxOrientationContractException implements Exception {
  CeyxOrientationContractException({
    required this.requestedOrientation,
    required this.width,
    required this.height,
    this.filePath,
  });

  /// The EXIF orientation (5, 6, 7, or 8) that was requested.
  final int requestedOrientation;

  /// The extent the native decode actually returned.
  final int width;
  final int height;

  /// The file being decoded, when available, for diagnostics.
  final String? filePath;

  @override
  String toString() =>
      'CeyxOrientationContractException(requested=$requestedOrientation, '
      'returned=${width}x$height'
      '${filePath != null ? ", path=$filePath" : ""}): kernel did not '
      'transpose extents for a transposing orientation request';
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
  // WP2: the service-owned `NativeFinalizer` that used to free a
  // dylib-allocated RGBA buffer is GONE. No decode route allocates through the
  // dylib any more, so that class of buffer does not exist; reclaim is the
  // pool's, via DngImage.releaseToPool() with `_poolSafetyNet` as the backstop
  // (and `debugFinalizerReleases`, asserted zero, as its defect gauge).
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
    _initialized = true;
  }

  /// Whether this service can perform a SIZED decode (cap the output long edge).
  ///
  /// WP5: this used to report "does the dylib export
  /// the legacy sized entry". That entry is deleted, so the old
  /// question answers "no" on every current build while sized decoding works —
  /// the same inverted-probe problem as [rawDecodeAvailable], and it was caught
  /// the same way: plugin/bin/prod_shape_probe.dart failed with
  /// `sizedDecodeAvailable==false` on the very same line that reported
  /// `sized(maxDim:200)=200x133`, i.e. its own output proved the capability it
  /// was denying.
  ///
  /// Re-pointed at the condition that actually determines it:
  /// `ceyx_decode_into_buffer` takes the SAME `max_dim` the deleted entry took
  /// and applies the same sizing rule, so the decode-into pair being present IS
  /// sized-decode being available.
  ///
  /// (The BINDINGS-level `DngNativeBindings.sizedDecodeAvailable` keeps its
  /// original meaning — it describes a loaded image, which is what the
  /// pinned-old-dylib tests assert about.)
  bool get sizedDecodeAvailable {
    if (!_initialized) initialize();
    return _bindings.decodeIntoBufferAvailable;
  }

  /// Whether this service can decode a generic RAW file.
  ///
  /// WP5: this used to report "does the dylib export the legacy RAW entry".
  /// That entry is deleted, so the old question now answers "no" on every
  /// current build while RAW decoding works perfectly — a capability probe that
  /// reports the OPPOSITE of the truth. It is re-pointed at the condition the
  /// RAW decode paths ACTUALLY gate on: `_decodeRawZeroCopy`,
  /// `_decodeRawToTransferable` and `_decodeRawToPointer` each throw
  /// [RawUnavailableException] when [decodeIntoBufferAvailable] is false, and
  /// nothing anywhere consults the legacy symbol. The probe and the gate now
  /// give the same answer by construction rather than by coincidence.
  ///
  /// (The BINDINGS-level `DngNativeBindings.rawDecodeAvailable` keeps its
  /// original meaning — it describes a loaded image, and pinned-old-dylib tests
  /// depend on that. This getter answers a different question: can I decode.)
  bool get rawDecodeAvailable {
    if (!_initialized) initialize();
    return _bindings.decodeIntoBufferAvailable;
  }

  /// Pushes the host's configured decode-lane width onto the native slot pool.
  ///
  /// The native pool is PROCESS-global — every worker isolate opens the same
  /// dylib in the same process — so calling this from any one worker
  /// configures it for all of them. Repeated identical calls are a no-op
  /// inside the pool.
  ///
  /// Ruling r-6: the request is honoured, not negotiated. The only bound is
  /// the native allocation-sanity constant (16), which sits above the host's
  /// slider maximum (8) and therefore cannot bite. A returned value differing
  /// from [requested] means the dylib bounded it, and the pool logs that
  /// loudly rather than absorbing it.
  ///
  /// Returns the effective slot count, or `-1` when the loaded library
  /// predates the configurable cap.
  int configureNativeSlots(int requested) {
    if (!_initialized) initialize();
    final fn = _bindings.dngDecodeConfigureSlots;
    if (fn == null) return -1;
    return fn(requested < 1 ? 1 : requested);
  }

  /// The slot count the native layer is currently configured for, or `-1` when
  /// the loaded library predates the entry.
  int get nativeConfiguredSlots {
    if (!_initialized) initialize();
    return _bindings.configuredSlots() ?? -1;
  }

  /// ADVISORY ONLY (ruling r-6). Slots this machine is recommended to run for
  /// a frame of [pixels]; pass 0 for the default 61 MP sizing frame. Returns
  /// `-1` when unsupported.
  ///
  /// This exists so the host can DISPLAY guidance next to its lane-width
  /// slider. No code path clamps a user's setting against it.
  int nativeRecommendedSlots({int pixels = 0}) {
    if (!_initialized) initialize();
    return _bindings.recommendedSlotsForPixels(pixels) ?? -1;
  }

  /// Pixel count of recommendation class [index] (0 = 24 MP, 1 = 61 MP,
  /// 2 = 108 MP), or `-1` when unsupported.
  int nativeRecommendationClassPixels(int index) {
    if (!_initialized) initialize();
    return _bindings.recommendationClassPixels(index) ?? -1;
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
    // WP6: replaces the native warmup's step-2 pool touch
    // (warmPipelinePoolsForSize), deleted with the native pools. The pages
    // that matter are now the Dart pool's, so the Dart pool commits them.
    // Run on the CALLING isolate (not the spawned worker above):
    // CeyxNativeBufferPool.shared lives on the main isolate only.
    await CeyxNativeBufferPool.shared.warmUpFor(width * height * 4);
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
  /// cannot size a decode, or when [maxDim] is
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

  /// Pool worker entry: decode [filePath] and return the transfer payload
  /// `[TransferableTypedData rgba, int width, int height, double decodeMs,
  /// double processMs]`.
  ///
  /// Same body as the [decodeOnWorker] worker half, minus the `Isolate.run` —
  /// a persistent pool worker is ALREADY on a worker isolate and has ALREADY
  /// loaded the dylib, so it calls this directly. Kept as a list rather than a
  /// private result class so the pool's wire protocol stays inspectable.
  ///
  /// Must only be called on a worker isolate.
  List<Object?> decodeForTransfer(String filePath, {int? maxDim}) {
    final result = _decodeToTransferable(filePath, maxDim: maxDim);
    return <Object?>[
      result.rgbaData,
      result.width,
      result.height,
      result.decodeMs,
      result.processMs,
    ];
  }

  /// Pool worker entry: decode [filePath] and hand the NATIVE RGBA buffer to
  /// the pool by ADDRESS, returning
  /// `[int rgbaAddress, int width, int height, double decodeMs,
  /// double processMs]` — five sendable primitives, no 97MB copy.
  ///
  /// Difference from [decodeForTransfer]: that path copies the native buffer
  /// into Dart-owned bytes and ships a [TransferableTypedData] (the copy at
  /// `_finishTransferable`), which lands on the receiving isolate as a fresh
  /// ~97MB Dart-heap object. This path ships ownership instead: the buffer is
  /// NOT copied, NOT freed here, and NO finalizer is attached in the worker
  /// (worker-path callers never use [_rgbaFinalizer], see the comments at
  /// :126/:148). `DngResult.rgbaData` is cleared before the route's `finally`
  /// runs so `dng_free_result` frees only the struct — the same anti
  /// double-free move as the zero-copy path (`_finishZeroCopy`).
  ///
  /// OWNERSHIP: from the moment this returns, the buffer is owned by NOBODY
  /// until the pool wraps it with a `NativeFinalizer`
  /// (`CeyxDecodePool._materialize`) or explicitly frees it (the pool's
  /// soft-cancel discard arm). If the message never reaches the pool (worker
  /// death, pool shutdown with queued results) exactly ONE buffer leaks; that
  /// bounded-leak inventory is documented on [CeyxDecodePool].
  ///
  /// Must only be called on a worker isolate.
  List<Object?> decodeForPointerTransfer(String filePath, {int? maxDim}) {
    if (!_initialized) {
      initialize();
    }
    switch (decodeRouteForPath(filePath)) {
      case DecodeRoute.dng:
        return _decodeDngToPointer(filePath, maxDim);
      case DecodeRoute.raw:
        return _decodeRawToPointer(filePath, maxDim);
      case DecodeRoute.unsupported:
        throw DngDecodeException(
          DngErrorCode.parseFailed,
          _unsupportedMessage(filePath),
        );
    }
  }

  /// Whether the loaded dylib can decode a DNG into a caller-owned buffer.
  bool get decodeIntoBufferAvailable {
    if (!_initialized) {
      initialize();
    }
    return _bindings.decodeIntoBufferAvailable;
  }

  /// R4 WP10: metadata-only output-extent probe.
  ///
  /// FORMAT-AGNOSTIC BY CONSTRUCTION — there is deliberately no route switch
  /// here (AC16.1). The native entry routes on `raw_probe_file`: a DNG takes
  /// the DNG metadata probe, a generic-RAW file takes the LibRaw one, and both
  /// skip their pipeline's expensive step.
  ///
  /// Returns null when the dylib predates the entry, when this build has no
  /// decoder for the format (`kCeyxErrFormatUnsupportedInBuild`), or when the
  /// probe fails — callers then fall back to the allocating route rather than
  /// guessing a size. A guess would be sized wrong on the non-Bayer downgrade
  /// (G4) or on a format whose geometry moves during unpack (R11.1).
  ///
  /// Must only be called on a worker isolate.
  /// Test seam: when set, [probeOutputSize] answers this instead of asking the
  /// dylib. Exists to drive a probe/decode extent DISAGREEMENT, which is the
  /// one condition the sync arms' resize retry exists for and which cannot
  /// otherwise be produced on demand (a correct probe never disagrees).
  @visibleForTesting
  static ({int width, int height})? debugProbeOutputSizeOverride;

  ({int width, int height})? probeOutputSize(String filePath, {int? maxDim}) {
    if (!_initialized) {
      initialize();
    }
    final override = debugProbeOutputSizeOverride;
    if (override != null) return override;
    final probe = _bindings.ceyxProbeOutputSize;
    if (probe == null) return null;

    final pathPtr = filePath.toNativeUtf8();
    final wPtr = calloc<Int32>();
    final hPtr = calloc<Int32>();
    try {
      final rc = probe(pathPtr.cast(), maxDim ?? 0, wPtr, hPtr);
      if (rc != 0 || wPtr.value <= 0 || hPtr.value <= 0) return null;
      return (width: wPtr.value, height: hPtr.value);
    } finally {
      calloc.free(wPtr);
      calloc.free(hPtr);
      malloc.free(pathPtr);
    }
  }

  /// R4 WP10: pointer-transfer decode INTO the caller's buffer at
  /// [dstAddress] ([dstCapacity] bytes).
  ///
  /// FORMAT-AGNOSTIC BY CONSTRUCTION — no route switch here either (AC16.1);
  /// the native entry routes internally, which is why one binding serves both
  /// a `.dng` and an `.arw`.
  ///
  /// Returns the SAME 5-element wire shape as [decodeForPointerTransfer],
  /// where `address == dstAddress` — pointer identity is part of the native
  /// contract, so the pool's slot and the payload are the same memory and no
  /// copy happens anywhere.
  ///
  /// Throws [DngBufferTooSmallException] when the native layer refuses the
  /// buffer, carrying the extent IT reported so the caller's re-acquire is
  /// exact. That refusal happens before any pixel work on the DNG route; on
  /// the RAW route it costs an unpack (AMENDMENT 2b A2.11 Q1), which is why
  /// the probe — not the refusal — is the primary sizing mechanism.
  ///
  /// Must only be called on a worker isolate.
  List<Object?> decodeIntoPointer(
    String filePath,
    int dstAddress,
    int dstCapacity, {
    int? maxDim,
  }) {
    if (!_initialized) {
      initialize();
    }
    final decodeInto = _bindings.ceyxDecodeIntoBuffer;
    if (decodeInto == null) {
      // Never reached from the pool, which checks decodeIntoBufferAvailable
      // first. Loud rather than a silent fallback: a caller that reached here
      // believes it handed over a slot, and quietly ignoring the address would
      // leak that slot while every test stayed green.
      throw StateError('ceyx_decode_into_buffer unavailable in this dylib');
    }

    final pathPtr = filePath.toNativeUtf8();
    Pointer<DngResult> resultPtr = nullptr;
    try {
      resultPtr = decodeInto(
        pathPtr.cast(),
        maxDim ?? 0,
        Pointer<Uint8>.fromAddress(dstAddress),
        dstCapacity,
      );
      if (resultPtr != nullptr &&
          resultPtr.ref.errorCode == CeyxDecodeIntoError.dstTooSmall) {
        // ONE "too small" code for ONE entry pair, so the retry stays
        // route-agnostic — which is what lets the pool's kMsgResize handler
        // stay route-agnostic too.
        throw DngBufferTooSmallException(
          resultPtr.ref.width,
          resultPtr.ref.height,
        );
      }
      // Reused UNCHANGED. It clears result.rgbaData before returning, which is
      // what stops dng_free_result in the finally below from handing the
      // CALLER's buffer to a native free path.
      //
      // `isRaw: false` is NOT a claim that the file is a DNG — this entry is
      // format-agnostic and Dart deliberately does not know the route
      // (AC16.1). It is safe because `_throwDecodeError` discriminates on the
      // CODE as well: `if (isRaw || RawErrorCode.isRawError(code))`, and the
      // two scales are disjoint by contract, so a RAW failure still raises a
      // RawDecodeException. The only behavioural difference is for a null
      // result or a null buffer WITH a success code, where the generic
      // DngDecodeException is raised instead of the RAW-flavoured one; both
      // are decode failures and callers treat any throw identically.
      return _finishPointerTransfer(resultPtr, isRaw: false);
    } finally {
      if (resultPtr != nullptr) {
        _bindings.dngFreeResult(resultPtr);
      }
      malloc.free(pathPtr);
    }
  }

  /// Native-rotation spec Task 3: whether the loaded dylib exports
  /// `ceyx_decode_into_buffer_oriented`. Independent of
  /// [decodeIntoBufferAvailable] — see the binding's own doc comment.
  bool get decodeIntoBufferOrientedAvailable {
    if (!_initialized) {
      initialize();
    }
    return _bindings.decodeIntoBufferOrientedAvailable;
  }

  /// True for the four EXIF orientations that swap width and height (5, 6,
  /// 7, 8). Mirrors the native `ceyx_orientation_transposes` predicate
  /// (`native/include/ceyx_orient.h`) — the in-repo source of truth — kept as
  /// a small local copy so this file's mechanical consistency check (below)
  /// does not need an extra FFI round trip. (Productionization plan Task 9:
  /// this file has no Dart-side rotation code of its own and does not mirror
  /// any `exif_orientation.dart` — that file is Halcyon's, not ceyx's.)
  static bool _orientationTransposes(int exifOrientation) =>
      exifOrientation == 5 ||
      exifOrientation == 6 ||
      exifOrientation == 7 ||
      exifOrientation == 8;

  /// The mechanical extent-consistency assertion used by
  /// [decodeIntoPointerOriented] to decide what to report as
  /// `appliedOrientation`. Hoisted to a standalone, directly-testable static
  /// so the production decision and the unit tests cannot drift apart (they
  /// call this exact function, not a re-derived copy).
  ///
  /// AMENDED by productionization plan Task 9 (reconciliation 2/3): with the
  /// scratch-checkout degradation arm deleted, fusion applies orientation on
  /// every route, so this is no longer a "trust vs. downgrade" decision —
  /// it is the one client-side proof that the kernel actually oriented
  /// (spec §4.2's silent-failure mode: kernel runs on zeros or does nothing,
  /// returns success, dims correct, timings plausible). It stays a FREE
  /// assertion: it only compares values the caller already has, and it no
  /// longer performs its own FFI probe round trip (that forced second call
  /// is deleted — see [decodeIntoPointerOriented]).
  ///
  /// - identity (`requested == 1`) always reports 1.
  /// - a non-transposing orientation (2/3/4) always reports [requested] —
  ///   there is no swap to verify, so a successful decode means it applied.
  /// - a transposing orientation (5/6/7/8):
  ///   - when the caller supplies the file's unoriented reference extent
  ///     ([probedWidth]/[probedHeight]) and the returned extent is swapped
  ///     relative to it, reports [requested] (verified).
  ///   - when the caller supplies that reference and the returned extent
  ///     came back UNSWAPPED, throws [CeyxOrientationContractException] —
  ///     this can no longer be a benign degrade (that arm is gone), so an
  ///     unswapped extent on a transposing request means the kernel silently
  ///     failed to orient.
  ///   - when the caller has no reference to compare against (both null),
  ///     there is nothing to verify against, so this reports [requested]
  ///     rather than conservatively downgrading — the fallback contract that
  ///     motivated the old conservative "report 1" no longer exists.
  ///
  /// KNOWN LIMITATION (unchanged from the prior revision): when the
  /// unoriented frame is exactly SQUARE, the swap check is trivially
  /// satisfied either way, so a genuine transpose cannot be distinguished
  /// from a hypothetical silent no-op on a square frame. This is accepted:
  /// the alternative (treating square as always-unverifiable) would falsely
  /// flag every ordinary square-frame transposing decode as a contract
  /// violation, which is worse than the rare square-frame miss this cannot
  /// catch.
  @visibleForTesting
  static int selfVerifiedAppliedOrientation({
    required int requested,
    required int width,
    required int height,
    required int? probedWidth,
    required int? probedHeight,
    String? filePathForError,
  }) {
    if (requested == 1) return 1;
    if (!_orientationTransposes(requested)) return requested;
    if (probedWidth == null || probedHeight == null) {
      // No unoriented reference to verify against. The (deleted)
      // scratch-degrade fallback was the only reason the extent could ever
      // legitimately come back unswapped; without a reference there is
      // nothing to detect that against, so trust the success code.
      return requested;
    }
    if (width == probedHeight && height == probedWidth) {
      return requested;
    }
    throw CeyxOrientationContractException(
      requestedOrientation: requested,
      width: width,
      height: height,
      filePath: filePathForError,
    );
  }

  /// Native-rotation spec Task 3: orientation-aware sibling of
  /// [decodeIntoPointer]. Same contract, same buffer-too-small translation,
  /// same [_finishPointerTransfer] reuse (isRaw: false) — the ONLY difference
  /// is the native call carries [exifOrientation] and the wire shape gains a
  /// trailing `appliedOrientation` element:
  /// `[address, width, height, decodeMs, processMs, appliedOrientation]`.
  ///
  /// AMENDED by productionization plan Task 9 (reconciliation 2/3): this no
  /// longer calls `probeOutputSize` internally (that was a full header parse
  /// on every transposing decode, and the arm it was defending against is
  /// gone). Instead [probedWidth]/[probedHeight] let the CALLER supply the
  /// unoriented reference extent it already computed for its own
  /// buffer-capacity check — `CeyxDecodePool` does exactly this (it already
  /// probes for slot sizing) — so [selfVerifiedAppliedOrientation] verifies
  /// for free instead of re-deriving the reference via FFI. Both null (no
  /// caller-supplied reference) means "nothing to verify against", so a
  /// success reports [exifOrientation] as applied without throwing.
  ///
  /// [decodeIntoPointer] (unoriented) is untouched by this method and stays
  /// byte-identical — it is this method's A/B control.
  ///
  /// Must only be called on a worker isolate.
  List<Object?> decodeIntoPointerOriented(
    String filePath,
    int dstAddress,
    int dstCapacity, {
    int? maxDim,
    required int exifOrientation,
    int? probedWidth,
    int? probedHeight,
  }) {
    if (!_initialized) {
      initialize();
    }
    final decodeIntoOriented = _bindings.ceyxDecodeIntoBufferOriented;
    if (decodeIntoOriented == null) {
      // Never reached from a caller that checks
      // decodeIntoBufferOrientedAvailable first (same discipline as
      // decodeIntoPointer above). Loud rather than silent: a caller that
      // reached here believes it handed over a slot for an oriented decode.
      throw StateError(
        'ceyx_decode_into_buffer_oriented unavailable in this dylib',
      );
    }

    final pathPtr = filePath.toNativeUtf8();
    Pointer<DngResult> resultPtr = nullptr;
    try {
      resultPtr = decodeIntoOriented(
        pathPtr.cast(),
        maxDim ?? 0,
        Pointer<Uint8>.fromAddress(dstAddress),
        dstCapacity,
        exifOrientation,
      );
      if (resultPtr != nullptr &&
          resultPtr.ref.errorCode == CeyxDecodeIntoError.dstTooSmall) {
        throw DngBufferTooSmallException(
          resultPtr.ref.width,
          resultPtr.ref.height,
        );
      }
      final transfer = _finishPointerTransfer(resultPtr, isRaw: false);
      final width = transfer[1] as int;
      final height = transfer[2] as int;

      // No internal probe round trip anymore (see the doc comment above):
      // probedWidth/probedHeight come from the caller when it already has
      // them (CeyxDecodePool's slot-sizing probe); null when it doesn't,
      // in which case the assertion trusts the success code rather than
      // throwing a false contract violation.
      final appliedOrientation = selfVerifiedAppliedOrientation(
        requested: exifOrientation,
        width: width,
        height: height,
        probedWidth: probedWidth,
        probedHeight: probedHeight,
        filePathForError: filePath,
      );

      return <Object?>[...transfer, appliedOrientation];
    } finally {
      if (resultPtr != nullptr) {
        _bindings.dngFreeResult(resultPtr);
      }
      malloc.free(pathPtr);
    }
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
  /// the pool's finalizer is invoked automatically on the native pointer.
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

  /// WP2: this isolate's pool. A Dart `static final` initialises once PER
  /// ISOLATE, so an in-isolate decode gets its own free list while the native
  /// addresses stay process-global — which is exactly the ownership model this
  /// campaign wants: every live address belongs to the pool of the isolate
  /// that acquired it.
  CeyxNativeBufferPool get _buffers => CeyxNativeBufferPool.shared;

  /// WP2 safety net for a service-decoded frame, mirroring the decode pool's
  /// (`decode_pool.dart`). A pooled buffer is meant to come back through
  /// `DngImage.releaseToPool()`; this catches the callers that forget, and its
  /// reclaim is counted separately so a test can assert it never fired.
  /// Keyed on (buffer, the checkout it was armed for) rather than a bare
  /// address: the pool reuses buffer instances, so after a release-and-reacquire
  /// an address alone cannot tell "still mine" from "someone else's now".
  static final Finalizer<(CeyxNativeBuffer, int)> _poolSafetyNet =
      Finalizer<(CeyxNativeBuffer, int)>(((CeyxNativeBuffer, int) armed) {
        CeyxNativeBufferPool.shared.releaseFromFinalizer(armed.$1, armed.$2);
      });

  /// Probes the output extent and checks out a buffer for it, SYNCHRONOUSLY.
  ///
  /// WP2 decision, recorded because it is the one place the synchronous public
  /// API forces a shape the async path does not need: [decode] cannot await, so
  /// it uses [CeyxNativeBufferPool.acquireOrNull] and, when the pool is at its
  /// cap, allocates and hands the address straight to [adoptUnpooled]. The
  /// buffer is then unpooled-but-pool-owned: it takes no slot (the bound stays
  /// a bound), and every reclaim route still works on it.
  CeyxNativeBuffer _acquireForSync(
    String filePath,
    int? maxDim, {
    required bool isRaw,
  }) {
    final extent = probeOutputSize(filePath, maxDim: maxDim);
    if (extent == null) {
      // The probe now fails where the allocating decode entry used to, so it
      // must raise that route's exception TYPE — the RAW route's callers (and
      // this repo's own raw_decode_service_test) match on RawDecodeException,
      // and a probe failure is exactly kRawErrProbeFailed.
      if (isRaw) {
        throw RawDecodeException(
          RawErrorCode.probeFailed,
          RawErrorCode.name(RawErrorCode.probeFailed),
          'Container probe failed (not a recognised RAW/TIFF header)',
        );
      }
      throw DngDecodeException(
        DngErrorCode.parseFailed,
        'output extent probe failed for $filePath',
      );
    }
    final bytes = extent.width * extent.height * 4;
    final pooled = _buffers.acquireOrNull(bytes);
    if (pooled != null) return pooled;
    return _buffers.adoptUnpooled(malloc<Uint8>(bytes).address, bytes);
  }

  /// Round-1 review F2: probe -> acquire -> decode-into, WITH the one-shot
  /// resize retry the pooled route has always had (`kMsgResize`).
  ///
  /// The probe and the decode can disagree about the extent — that is precisely
  /// what `DngBufferTooSmallException` reports, and the pool treats it as
  /// routine. Without a retry here the same disagreement came out of the PUBLIC
  /// synchronous API as a hard error, which would be a new way for a photo that
  /// opens today to stop opening (contract R-A). The refusal carries native's
  /// own extent, so the second attempt is sized exactly, and a second refusal
  /// stays an error rather than looping.
  ///
  /// Returns the wire AND the buffer that backs it, because the caller decides
  /// the buffer's fate: the zero-copy arm hands ownership to the DngImage, the
  /// transferable arm copies out and returns it immediately.
  ({List<Object?> wire, CeyxNativeBuffer buffer}) _decodeIntoPooledBuffer(
    String filePath,
    int? maxDim, {
    required bool isRaw,
  }) {
    var buffer = _acquireForSync(filePath, maxDim, isRaw: isRaw);
    try {
      final wire = decodeIntoPointer(
        filePath,
        buffer.address,
        buffer.capacity,
        maxDim: maxDim,
      );
      return (wire: wire, buffer: buffer);
    } on DngBufferTooSmallException catch (e) {
      // The probe was wrong. Give the slot back before taking another, so a
      // pool at its cap can reuse this very buffer for the retry.
      _buffers.release(buffer);
      buffer = _acquireExactly(e.requiredBytes);
      try {
        final wire = decodeIntoPointer(
          filePath,
          buffer.address,
          buffer.capacity,
          maxDim: maxDim,
        );
        return (wire: wire, buffer: buffer);
      } catch (_) {
        _buffers.release(buffer);
        rethrow;
      }
    } catch (_) {
      _buffers.release(buffer);
      rethrow;
    }
  }

  /// Checks out exactly [bytes], falling back to malloc + adoption when the
  /// pool is at its cap — the synchronous caller cannot wait for a slot.
  CeyxNativeBuffer _acquireExactly(int bytes) =>
      _buffers.acquireOrNull(bytes) ??
      _buffers.adoptUnpooled(malloc<Uint8>(bytes).address, bytes);

  /// WP2: probe -> pool acquire -> decode-into, replacing the dylib's
  /// allocating legacy DNG entry. The public signature of [decode] is
  /// unchanged (R-C), only what allocates underneath it.
  DngImage _decodeZeroCopy(String filePath, {bool isRaw = false}) {
    if (!_initialized) {
      initialize();
    }
    final decoded = _decodeIntoPooledBuffer(filePath, null, isRaw: isRaw);
    return _imageFromPooledWire(decoded.wire, decoded.buffer);
  }

  /// Generic-RAW twin of [_decodeZeroCopy].
  ///
  /// WP2: `ceyx_decode_into_buffer` is format-agnostic (it routes on the
  /// container, exactly as the probe does), so this is the identical body. It
  /// stays a separate named method so [decode]'s route switch is unchanged.
  /// [RawUnavailableException] survives with its type intact — Halcyon may
  /// match on it — but its condition moves from "the dylib has no
  /// the legacy RAW entry" to "the dylib has no decode-into pair", which is
  /// the entry this route now depends on.
  DngImage _decodeRawZeroCopy(String filePath) {
    if (!_initialized) {
      initialize();
    }
    if (!decodeIntoBufferAvailable) {
      // Spec §4: typed exception, never a crash and never a silent fallback
      // to the DNG parser.
      throw RawUnavailableException(filePath);
    }
    return _decodeZeroCopy(filePath, isRaw: true);
  }

  /// Builds the [DngImage] over a pool-owned address: a zero-copy view plus the
  /// pool safety net. NEVER a `NativeFinalizer` — the buffer belongs to the
  /// pool, and handing it to the dylib's free would take it away from the pool.
  DngImage _imageFromPooledWire(List<Object?> wire, CeyxNativeBuffer buffer) {
    final address = wire[0] as int;
    final width = wire[1] as int;
    final height = wire[2] as int;
    final bytes = Pointer<Uint8>.fromAddress(
      address,
    ).asTypedList(width * height * 4);
    _poolSafetyNet.attach(bytes, (buffer, buffer.checkoutGeneration),
        detach: bytes);
    return DngImage(
      rgbaData: bytes,
      width: width,
      height: height,
      decodeMs: wire[3] as double,
      processMs: wire[4] as double,
      nativeAddress: address,
      onReleaseToPool: () {
        // Round-1 review F1: DISARM before returning the buffer. Without this
        // the net stays armed on a buffer the pool may hand to someone else,
        // and a later collection of `bytes` reclaims it under its new owner.
        _poolSafetyNet.detach(bytes);
        CeyxNativeBufferPool.noteSafetyNetDetach();
        _buffers.release(buffer);
      },
    );
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

  /// WP2: probe -> pool acquire -> decode-into -> COPY into Dart-owned bytes,
  /// then return the pool buffer. The copy is the point of this route (the
  /// bytes must cross an isolate boundary), so the native buffer's job ends
  /// with the call and the slot goes straight back — in a `finally`, so a
  /// throw cannot strand it.
  _DecodeWorkerResult _decodeDngToTransferable(
    String filePath,
    int? maxDim, {
    bool isRaw = false,
  }) {
    final decoded = _decodeIntoPooledBuffer(filePath, maxDim, isRaw: isRaw);
    try {
      return _transferableFromPooledWire(decoded.wire);
    } finally {
      _buffers.release(decoded.buffer);
    }
  }

  /// RAW twin of [_decodeDngToTransferable]; see [_decodeRawZeroCopy] for why
  /// the bodies are identical and why [RawUnavailableException] survives.
  _DecodeWorkerResult _decodeRawToTransferable(String filePath, int? maxDim) {
    if (!decodeIntoBufferAvailable) {
      throw RawUnavailableException(filePath);
    }
    return _decodeDngToTransferable(filePath, maxDim, isRaw: true);
  }

  /// Copies a pool-owned decode result into Dart-owned bytes.
  /// [TransferableTypedData] cannot carry a native-backed typed list across
  /// isolate boundaries safely, so the copy here is load-bearing, not waste.
  _DecodeWorkerResult _transferableFromPooledWire(List<Object?> wire) {
    final address = wire[0] as int;
    final width = wire[1] as int;
    final height = wire[2] as int;
    final rgbaCopy = Uint8List.fromList(
      Pointer<Uint8>.fromAddress(address).asTypedList(width * height * 4),
    );
    return _DecodeWorkerResult(
      rgbaData: TransferableTypedData.fromList([rgbaCopy]),
      width: width,
      height: height,
      decodeMs: wire[3] as double,
      processMs: wire[4] as double,
    );
  }

  // --- pointer-transfer route (H2-A) --------------------------------------
  // Deliberately parallel to _decodeDngToTransferable / _decodeRawToTransferable
  // rather than a shared generic: those two are the A/B control for this one
  // (`decodeOnWorker` still uses them byte-for-byte) and must not change shape.
  // Only the finish step differs.

  /// WP2: probe -> pool acquire -> decode-into, shipping the ADDRESS onward.
  /// Unlike the transferable arms this must NOT release: ownership travels with
  /// the payload, exactly as it did when the dylib allocated it. The receiver
  /// owns the reclaim — for an address acquired on this isolate that means
  /// `CeyxNativeBufferPool.shared.tryReleaseByAddress`, and for one shipped to
  /// another isolate, that isolate's adoption (see `decode_pool.dart`). On a
  /// throw the buffer is returned here, so no address escapes an error path.
  List<Object?> _decodeDngToPointer(
    String filePath,
    int? maxDim, {
    bool isRaw = false,
  }) {
    return _decodeIntoPooledBuffer(filePath, maxDim, isRaw: isRaw).wire;
  }

  /// RAW twin of [_decodeDngToPointer]; see [_decodeRawZeroCopy].
  List<Object?> _decodeRawToPointer(String filePath, int? maxDim) {
    if (!decodeIntoBufferAvailable) {
      throw RawUnavailableException(filePath);
    }
    return _decodeDngToPointer(filePath, maxDim, isRaw: true);
  }

  /// Success/failure handling for the pointer-transfer route. Same validation
  /// ladder as [_finishTransferable]; the only difference is the finish step,
  /// which hands the raw address over instead of copying the bytes.
  List<Object?> _finishPointerTransfer(
    Pointer<DngResult> resultPtr, {
    required bool isRaw,
  }) {
    if (resultPtr == nullptr) {
      if (isRaw) {
        throw RawDecodeException(
          RawErrorCode.allocationFailed,
          RawErrorCode.name(RawErrorCode.allocationFailed),
          'Native RAW decode-into entry returned null',
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

    final address = result.rgbaData.address;
    final width = result.width;
    final height = result.height;

    // Ownership shipped raw: clear the struct field LAST, after every throw
    // site above, so a failure still lets dng_free_result reclaim the buffer.
    result.rgbaData = nullptr;

    return <Object?>[
      address,
      width,
      height,
      result.decodeMs,
      result.processMs,
    ];
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
