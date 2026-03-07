#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Result structure returned by dng_decode_and_process.
/// The caller MUST free this with dng_free_result().
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

/// Free a DngResult previously returned by dng_decode_and_process.
void dng_free_result(DngResult *result);

/// Free a standalone memory buffer allocated by Halide / Native pipeline.
/// Used for zero-copy memory management from Dart via NativeFinalizer.
void dng_free_halide_buffer(void *ptr);

#ifdef __cplusplus
}
#endif
