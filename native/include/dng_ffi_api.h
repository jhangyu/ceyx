#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Result structure returned by the decode entries (ceyx_decode_into.h).
/// The caller MUST free this with dng_free_result().
///
/// ABI contract: 6 fields; do NOT reorder or insert fields in the middle.
/// Layout on 64-bit: sizeof==40, rgba_data@0, width@8, height@12,
/// error_code@16, decode_ms@24, process_ms@32.
/// Any change here MUST be mirrored in DngResult (dng_bindings.dart).
/// See also: Gotcha #58 (memory.md) — field-count mismatch was a past bug.
typedef struct {
  uint8_t *rgba_data; ///< RGBA buffer (width*height*4 bytes), or NULL on error
  int32_t width;      ///< Image width
  int32_t height;     ///< Image height
  int32_t error_code; ///< 0 = success, negative = error
  double decode_ms;   ///< DNG decompression time (ms)
  double process_ms;  ///< Halide pipeline time (ms)
} DngResult;

/// Extract preview JPEG from a DNG file.
/// Returns 0 on success, or an error code.
/// Caller MUST free *outBuffer with dng_free_buffer().
int dng_extract_preview_jpeg(const char *filePath, uint8_t **outBuffer,
                             int *outSize);

/// Free a buffer allocated by dng_extract_preview_jpeg.
void dng_free_buffer(uint8_t *buffer);

/// WP5: the allocating decode entries that used to be declared here are
/// DELETED. Decoding goes through ceyx_decode_into_buffer (ceyx_decode_into.h),
/// which writes into a CALLER-OWNED buffer; the library allocates no
/// full-resolution RGBA output on any path. max_dim carries over unchanged.

/// Warm process-scoped native resources for a likely decode size.
/// This warms shared lossless/lossy workspaces and lossy MapPolynomial state.
/// Returns 0 on success, or a negative error code.
int32_t dng_decoder_warmup_for_size(int32_t width, int32_t height);

/// Free a DngResult previously returned by a decode entry.
/// WP5: this frees ONLY the struct. It does NOT free rgba_data, because
/// rgba_data always points at the buffer the CALLER supplied to the decode --
/// the library never owns it. Callers therefore do NOT need to clear
/// result->rgba_data first; doing so is harmless but no longer required.
void dng_free_result(DngResult *result);

/// WP5: the standalone RGBA free entry and the native pool's checked-out gauge
/// are DELETED with the RGBA output pool they served. The process-wide
/// "nothing leaked" gauge is now CeyxNativeBufferPool.debugTotalLiveAddresses
/// on the Dart side, which counts live addresses across every isolate
/// (user ruling OQ-4).

/// R4 item 1 (rulings r-1, r-5, r-6). Sets the number of concurrent full-frame
/// decode slots. The HOST'S USER SETTING IS THE SINGLE SOURCE OF TRUTH, and
/// this is how it reaches the native layer.
///
/// Ruling r-6 governs the clamping: `requested` is bounded ONLY by
/// [1, PipelineConfig::kAbsoluteMaxDecodeSlots (16)], which is an
/// allocation-sanity bound, not a policy. There is deliberately NO memory- or
/// CPU-derived clamp — a machine's recommended width (see
/// dng_decode_recommended_slots_for_pixels below) is advisory and is displayed
/// by the host, never enforced here. A user who selects 8 gets 8.
///
/// Applies immediately (ruling r-5): the pool grows at once, and a narrowing
/// tightens admission at once while running decodes finish undisturbed —
/// native decodes are uncancellable and are never pre-empted.
///
/// Safe to call from any thread, at any time, before or after the first decode
/// (the first call constructs the pool at the requested size). Idempotent.
/// Returns the effective slot count, always >= 1.
int32_t dng_decode_configure_slots(int32_t requested);

/// The currently configured slot count — the value the last
/// dng_decode_configure_slots() returned, or the pre-configuration default.
int32_t dng_decode_configured_slots(void);

/// ADVISORY ONLY (ruling r-6). The number of concurrent slots this machine is
/// RECOMMENDED to run for a frame of `pixels`, so the host's settings UI can
/// show the user what their hardware suits. NOTHING IN THIS PROCESS CLAMPS
/// AGAINST THIS VALUE.
///
/// Pass 0 for the default 61 MP sizing frame. The host displays three classes;
/// the pixel counts are exported below so it need not hardcode them.
/// Derivation: docs/logs/2026-09-05/slot-memory-rederivation.md.
int32_t dng_decode_recommended_slots_for_pixels(int64_t pixels);

/// The three resolution classes the settings UI displays, in pixels.
/// 0 = 24 MP (6000x4000), 1 = 61 MP (9504x6336, the default), 2 = 108 MP
/// (12000x9000). Returns 0 for an unknown index.
int64_t dng_decode_recommendation_class_pixels(int32_t index);

/// R3-3: Set the VkPipelineCache persistence file path (Android/Vulkan only).
/// Call BEFORE the first warmup/decode with a writable per-app path (e.g.
/// <cacheDir>/dng_vk_pipeline.cache). Pass NULL or "" to disable.
/// No-op (returns -1) on platforms/builds without the Halide Vulkan runtime
/// fork (macOS, or CMake -DDNG_VK_PIPELINE_CACHE=OFF). Never fails a decode:
/// invalid paths or I/O errors silently fall back to uncached compilation.
/// Returns 0 when the setting was applied.
int32_t dng_decoder_set_pipeline_cache_path(const char *path);

/// R3-3: Flush the pipeline cache to disk now (if dirty). Also invoked
/// automatically at the end of dng_decoder_warmup_for_size and of a decode.
/// Returns 0 on success or nothing-to-do, -1 when
/// unsupported on this build, -2 on (non-fatal) save failure.
int32_t dng_decoder_save_pipeline_cache(void);

/// R3-3: Observability for cache-hit evidence. Bitmask:
///   1 = feature enabled (path set, not disabled by env)
///   2 = VkPipelineCache object exists this session
///   4 = cache file was loaded & validated at startup (cross-launch HIT)
///   8 = unsaved pipeline data pending
/// Returns -1 when unsupported on this build.
int32_t dng_decoder_pipeline_cache_status(void);

/* The generic (non-DNG) RAW route's declarations moved to raw_ffi_api.h on
 * 2026-08-25. That header includes this one, because the RAW entry points
 * return this same DngResult and free it with dng_free_result(). */

#ifdef __cplusplus
}
#endif

// ---------------------------------------------------------------------------
// ABI compile-time sentinels (C++ only).
// If any assert fires, update the struct layout AND dng_bindings.dart.
// ---------------------------------------------------------------------------
#ifdef __cplusplus
#include <cstddef>
#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(DngResult) == 40,
              "ABI: DngResult size changed — update dng_bindings.dart");
static_assert(offsetof(DngResult, rgba_data) == 0,
              "ABI: DngResult.rgba_data offset mismatch");
static_assert(offsetof(DngResult, width) == 8,
              "ABI: DngResult.width offset mismatch");
static_assert(offsetof(DngResult, height) == 12,
              "ABI: DngResult.height offset mismatch");
static_assert(offsetof(DngResult, error_code) == 16,
              "ABI: DngResult.error_code offset mismatch");
static_assert(offsetof(DngResult, decode_ms) == 24,
              "ABI: DngResult.decode_ms offset mismatch");
static_assert(offsetof(DngResult, process_ms) == 32,
              "ABI: DngResult.process_ms offset mismatch");
#endif // INTPTR_MAX == INT64_MAX
#endif // __cplusplus
