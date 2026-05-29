#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Result structure returned by dng_decode_and_process.
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

/// Decode a DNG file and process it through the Halide pipeline.
/// Returns a heap-allocated DngResult. Caller must free with dng_free_result().
DngResult *dng_decode_and_process(const char *file_path);

/// Warm process-scoped native resources for a likely decode size.
/// This warms shared lossless/lossy workspaces and lossy MapPolynomial state.
/// Returns 0 on success, or a negative error code.
int32_t dng_decoder_warmup_for_size(int32_t width, int32_t height);

/// Free a DngResult previously returned by dng_decode_and_process.
/// This function frees BOTH the DngResult struct AND its rgba_data buffer
/// (when rgba_data is non-NULL). Callers that transfer rgba_data ownership to
/// a NativeFinalizer (zero-copy path) MUST clear result->rgba_data = NULL
/// before calling this function to avoid a double-free.
void dng_free_result(DngResult *result);

/// Free a standalone RGBA buffer returned by dng_decode_and_process.
/// Used for zero-copy memory management from Dart via NativeFinalizer.
void dng_free_rgba_buffer(void *ptr);

/// Deprecated alias for dng_free_rgba_buffer().
/// Kept for ABI compatibility with older Dart builds.
void dng_free_halide_buffer(void *ptr);

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
