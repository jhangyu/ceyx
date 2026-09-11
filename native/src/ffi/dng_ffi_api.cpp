#include "dng_ffi_api.h"
#include "dng_error_codes.h"  // W5: unified error codes
#include <cstdlib>
#include <cstring>
#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_ifd.h>
#include <dng_info.h>
#include <dng_exceptions.h>
#include <iostream>

#include "dng_pipeline.h"
// R4 item 1: kAbsoluteMaxDecodeSlots + the advisory recommendation helpers.
#include "dng_pipeline_config.h"
// R1-T1 (GPU copy-elimination C3): the render-parameter upload cache counters
// behind ceyx_debug_render_parameter_cache_counters below, and the declaration
// of that probe itself.
#include "raw_ffi_api.h"
#include "render_parameter_upload_cache.h"

#if defined(_WIN32)
#define FFI_EXPORT __declspec(dllexport)
#else
#define FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

// ---------------------------------------------------------------------------
// W7 (M-11): rgb_to_rgba_neon RETIRED. WP1 phase 3: pipeline.rgba_ptr is
// unconditionally set now, so the FFI layer takes the RGBA buffer as-is.
// G2 (Round 2): BOTH platforms now write RGBA8
// in-kernel (alpha=255) — the Android planar→RGBA host repack is retired.
// The ~96 MB RGBA buffer is pool-backed (checkout-style pool in
// dng_pipeline.cpp) to avoid page-faults on warm decodes.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// R3-3: VkPipelineCache persistence bridge (Android/Vulkan only).
// The implementation lives in the Halide Vulkan runtime fork
// (native/halide_runtime_fork/, CMake option DNG_VK_PIPELINE_CACHE). The
// symbols are referenced WEAK so this file still links when the fork is not
// compiled in (macOS/Metal, or the option is OFF): unresolved weak = nullptr
// on ELF, and the wrappers below degrade to "unsupported" no-ops.
// ---------------------------------------------------------------------------
#if defined(__ANDROID__)
extern "C" {
__attribute__((weak)) int dng_vk_pipeline_cache_set_path(const char *path);
__attribute__((weak)) int dng_vk_pipeline_cache_save(void);
__attribute__((weak)) int dng_vk_pipeline_cache_status(void);
}
#endif

namespace {
// Best-effort flush; must never affect the caller's result (red line:
// cache I/O failure on any path must never fail a decode).
inline void dngAutoSaveVkPipelineCache() {
#if defined(__ANDROID__)
  if (&dng_vk_pipeline_cache_save != nullptr) {
    (void)dng_vk_pipeline_cache_save();
  }
#endif
}
}  // namespace

extern "C" {

FFI_EXPORT int32_t dng_decoder_set_pipeline_cache_path(const char *path) {
#if defined(__ANDROID__)
  if (&dng_vk_pipeline_cache_set_path != nullptr) {
    return static_cast<int32_t>(dng_vk_pipeline_cache_set_path(path));
  }
#else
  (void)path;
#endif
  return -1;  // unsupported on this build
}

FFI_EXPORT int32_t dng_decoder_save_pipeline_cache(void) {
#if defined(__ANDROID__)
  if (&dng_vk_pipeline_cache_save != nullptr) {
    return dng_vk_pipeline_cache_save() == 0 ? 0 : -2;
  }
#endif
  return -1;  // unsupported on this build
}

FFI_EXPORT int32_t dng_decoder_pipeline_cache_status(void) {
#if defined(__ANDROID__)
  if (&dng_vk_pipeline_cache_status != nullptr) {
    return static_cast<int32_t>(dng_vk_pipeline_cache_status());
  }
#endif
  return -1;  // unsupported on this build
}

// WP5: the two allocating full-resolution decode entries and their shared body
// are DELETED. They were this library's only routes that allocated RGBA output.
// Callers use ceyx_decode_into_buffer (ceyx_decode_into.h), which writes into a
// buffer the caller owns.

FFI_EXPORT int32_t dng_decoder_warmup_for_size(int32_t width, int32_t height) {
  if (width <= 0 || height <= 0) {
    return -1;
  }
  const int32_t rc = dng_pipeline_warmup_for_size(width, height) ? 0 : -2;
  // R3-3: warmup compiles all production pipelines — persist them so the
  // NEXT launch skips compilation. Save failure never fails the warmup.
  dngAutoSaveVkPipelineCache();
  return rc;
}

FFI_EXPORT int dng_extract_preview_jpeg(const char *filePath, uint8_t **outBuffer,
                                      int *outSize) {
  if (!filePath || !outBuffer || !outSize)
    return 5; // INVALID_ARGUMENT

  try {
    dng_host host;
    dng_file_stream stream(filePath);
    dng_info info;
    info.Parse(host, stream);
    info.PostParse(host);

    int bestPreviewIfd = -1;
    uint32 bestPreviewWidth = 0;
    for (uint32 i = 0; i < info.fIFDCount; ++i) {
      const dng_ifd &ifd = *info.fIFD[i];
      if (ifd.fCompression == 7 && ifd.fPhotometricInterpretation == 6 &&
          ifd.fNewSubFileType == 1 && ifd.fImageWidth > bestPreviewWidth) {
        bestPreviewWidth = ifd.fImageWidth;
        bestPreviewIfd = static_cast<int>(i);
      }
    }

    if (bestPreviewIfd < 0)
      return 1;

    const dng_ifd &ifd = *info.fIFD[bestPreviewIfd];
    const uint64 offset = ifd.fTileOffset[0];
    const uint32 byteCount = ifd.fTileByteCount[0];
    if (byteCount == 0)
      return 1;

    *outSize = static_cast<int>(byteCount);
    *outBuffer = new uint8_t[*outSize];
    stream.SetReadPosition(offset);
    stream.Get(*outBuffer, byteCount);
    return 0;
  } catch (const dng_exception &e) {
    return e.ErrorCode();
  } catch (...) {
    return -100;
  }
}

FFI_EXPORT void dng_free_buffer(uint8_t *buffer) {
  if (buffer)
    delete[] buffer;
}

FFI_EXPORT void dng_free_result(DngResult *result) {
  if (!result)
    return;
  // WP5: frees ONLY the struct. It deliberately does NOT touch rgba_data.
  //
  // This is not an omission, it is the ownership rule. With every allocating
  // decode entry deleted, result->rgba_data is ALWAYS a pointer the CALLER
  // supplied to ceyx_decode_into_buffer, so the library must never free it.
  // The previous body released it into the RGBA output pool, which -- once
  // every caller supplies its own buffer -- meant handing a caller-owned
  // address to the pool's free list on every call. The pool's "absorb unknown
  // pointers and log" arm hid that, so it never surfaced as a failure.
  //
  // Turning that arm into free()/delete[] instead of removing it would have
  // been strictly worse: callers pass the data() of a live std::vector or a
  // Dart-owned allocation, so it would be heap corruption rather than a silent
  // no-op. Removing the arm makes every call site correct by construction
  // rather than correct only if it remembered to null the field first.
  std::free(result);
}

// WP5: the standalone RGBA free entry and the native pool's checked-out gauge
// are DELETED with the pool they served. The process-wide "nothing leaked"
// gauge they backed is now CeyxNativeBufferPool.debugTotalLiveAddresses on the
// Dart side, which counts live addresses across every isolate -- strictly more
// reach than the native counter had (user ruling OQ-4 / plan O10).

// ---------------------------------------------------------------------------
// R4 item 1 — three-layer parallelism sync (rulings r-1, r-5, r-6).
//
// The host's user setting is the single source of truth for decode
// parallelism; these entries are how it reaches the native layer.
//
// NOTE THE CLAMP, AND ESPECIALLY WHAT IS ABSENT FROM IT. The request is
// bounded by [1, kAbsoluteMaxDecodeSlots] and by NOTHING ELSE. Ruling r-6
// forbids any memory- or CPU-derived limit on the user's value, so there is
// deliberately no min() against the machine's recommended width here — and
// the recommendation entries below are consulted by nothing in this file.
// They exist so the host can DISPLAY guidance. If a future change
// reintroduces such a clamp, test_slot_config group (e) is what fails.
// ---------------------------------------------------------------------------

FFI_EXPORT int32_t dng_decode_configure_slots(int32_t requested) {
  const size_t kMax = PipelineConfig::kAbsoluteMaxDecodeSlots;
  size_t want = requested < 1 ? size_t{1} : static_cast<size_t>(requested);
  if (want > kMax) want = kMax;
  dng_decode_resize_slots(want);
  return static_cast<int32_t>(want);
}

FFI_EXPORT int32_t dng_decode_configured_slots(void) {
  return static_cast<int32_t>(dng_decode_slot_count());
}

// ADVISORY ONLY. pixels <= 0 means "use the 61 MP default sizing frame".
FFI_EXPORT int32_t dng_decode_recommended_slots_for_pixels(int64_t pixels) {
  const size_t px = pixels <= 0
                        ? PipelineConfig::kDecodeDefaultSizingPixels
                        : static_cast<size_t>(pixels);
  return static_cast<int32_t>(
      PipelineConfig::decodeRecommendedSlotsForPixels(px));
}

FFI_EXPORT int64_t dng_decode_recommendation_class_pixels(int32_t index) {
  switch (index) {
  case 0:
    return static_cast<int64_t>(PipelineConfig::kDecodePixels24MP);
  case 1:
    return static_cast<int64_t>(PipelineConfig::kDecodePixels61MP);
  case 2:
    return static_cast<int64_t>(PipelineConfig::kDecodePixels108MP);
  default:
    return 0;
  }
}

// ---------------------------------------------------------------------------
// R1-T1 — C3 render-parameter upload cache probe.
//
// Debug/probe surface only (see the contract comment in raw_ffi_api.h): not
// Dart-visible, nothing added to DngResult. The counters are process-wide
// totals; the gate reads deltas across decodes and asserts on BOTH numbers,
// because zero uploads with zero hits means the cache never executed.
// ---------------------------------------------------------------------------

FFI_EXPORT int32_t ceyx_debug_render_parameter_cache_counters(
    uint64_t *out_uploads_performed, uint64_t *out_cache_hits) {
  if (!out_uploads_performed && !out_cache_hits) return -1;
  if (out_uploads_performed) {
    *out_uploads_performed = ceyx::render_parameter_uploads_performed();
  }
  if (out_cache_hits) {
    *out_cache_hits = ceyx::render_parameter_cache_hits();
  }
  return 0;
}

} // extern "C"
