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

#include "dng_pipeline_v2.h"

#if defined(_WIN32)
#define FFI_EXPORT __declspec(dllexport)
#else
#define FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

// ---------------------------------------------------------------------------
// W7 (M-11): rgb_to_rgba_neon RETIRED. The pipeline now sets fuse_rgba_output
// on all platforms, so pipeline.rgba_ptr is always set and the FFI layer takes
// the RGBA buffer as-is. On macOS the Stage4 generator writes RGBA8 in-kernel
// (alpha=255); on Android the bridge fuses planar→RGBA via repackPlanarToRGBAMT.
// The ~96 MB RGBA buffer is pool-backed (checkout-style pool in
// dng_pipeline_v2.cpp) to avoid page-faults on warm decodes.
// ---------------------------------------------------------------------------

extern "C" {

FFI_EXPORT DngResult *dng_decode_and_process(const char *file_path) {
  DngResult *result =
      static_cast<DngResult *>(std::calloc(1, sizeof(DngResult)));
  if (!result)
    return nullptr;

  DngPipelineV2Result pipeline;
  if (!dng_pipeline_v2_decode_to_rgb(file_path, pipeline)) {
    result->error_code = pipeline.error_code;
    result->decode_ms = pipeline.decode_ms;
    result->process_ms = pipeline.process_ms;
    std::cerr << "[FFI] Pipeline v2 failed: " << result->error_code << "\n";
    return result;
  }

  // W7 (M-11): fuse_rgba_output is now always true — pipeline.rgba_ptr is the
  // RGBA8 buffer produced by the Stage4 kernel (macOS in-kernel alpha=255,
  // Android fused planar→RGBA repack). No rgb_to_rgba pass needed.
  uint8_t *rgba = pipeline.rgba_ptr;
  if (!rgba) {
    // Defensive: should not happen with fuse_rgba_output=true, but guard anyway.
    result->error_code = kDngErrRgbaAllocFailed;
    return result;
  }

  result->rgba_data = rgba;
  result->width = static_cast<int32_t>(pipeline.width);
  result->height = static_cast<int32_t>(pipeline.height);
  result->error_code = 0;
  result->decode_ms = pipeline.decode_ms;
  result->process_ms = pipeline.process_ms;

  // [FFI] Success stderr removed (W6-5 / TD-23): timing fields on result
  // struct are sufficient; always-on stderr polluted Xcode console and
  // CI timing parsers. Re-enable via DiagnosticConfig in the future if
  // a dedicated debug channel is needed.
  return result;
}

FFI_EXPORT int32_t dng_decoder_warmup_for_size(int32_t width, int32_t height) {
  if (width <= 0 || height <= 0) {
    return -1;
  }
  return dng_pipeline_v2_warmup_for_size(width, height) ? 0 : -2;
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
  // Frees rgba_data when non-NULL, then frees the struct itself.
  // Zero-copy callers MUST clear result->rgba_data before calling this
  // (see _decodeZeroCopy in dng_decoder_service.dart) to avoid double-free.
  // W5 (H-2 FFI): the pool's release() now absorbs unknown pointers as a
  // logged no-op (W1 pool defense), so the delete[] fallback is removed to
  // prevent heap corruption if a pool-owned pointer is mistakenly released
  // twice. Dart already nulls rgba_data at dng_decoder_service.dart:293.
  if (result->rgba_data) {
    dng_rgba_output_release(result->rgba_data);
    result->rgba_data = nullptr;
  }
  std::free(result);
}

FFI_EXPORT void dng_free_rgba_buffer(void *ptr) {
  if (!ptr)
    return;
  // W5 (H-2 FFI): pool absorbs all pointers (known or unknown) — no
  // delete[] fallback. See dng_free_result comment above.
  uint8_t *p = static_cast<uint8_t *>(ptr);
  dng_rgba_output_release(p);
}

FFI_EXPORT void dng_free_halide_buffer(void *ptr) {
  dng_free_rgba_buffer(ptr);
}

FFI_EXPORT size_t dng_debug_pool_checked_out(void) {
  return dng_rgba_output_checked_out_count();
}

} // extern "C"
