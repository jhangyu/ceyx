#include "dng_ffi_api.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_ifd.h>
#include <dng_info.h>
#include <dng_exceptions.h>
#include <iostream>
#include <vector>

#include "dng_pipeline_v2.h"

#if defined(_WIN32)
#define FFI_EXPORT __declspec(dllexport)
#else
#define FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

extern "C" {

DngResult *dng_decode_and_process(const char *file_path) {
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

  const size_t pixelCount =
      static_cast<size_t>(pipeline.width) * static_cast<size_t>(pipeline.height);
  uint8_t *rgba = new uint8_t[pixelCount * 4];
  for (size_t i = 0; i < pixelCount; ++i) {
    rgba[i * 4 + 0] = pipeline.rgb[i * 3 + 0];
    rgba[i * 4 + 1] = pipeline.rgb[i * 3 + 1];
    rgba[i * 4 + 2] = pipeline.rgb[i * 3 + 2];
    rgba[i * 4 + 3] = 255;
  }

  result->rgba_data = rgba;
  result->width = static_cast<int32_t>(pipeline.width);
  result->height = static_cast<int32_t>(pipeline.height);
  result->error_code = 0;
  result->decode_ms = pipeline.decode_ms;
  result->process_ms = pipeline.process_ms;

  std::cerr << "[FFI] Success: " << pipeline.width << "x" << pipeline.height
            << " decode=" << result->decode_ms
            << "ms process=" << result->process_ms << "ms\n";
  return result;
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

void dng_free_result(DngResult *result) {
  if (!result)
    return;
  // NOTE: If rgba_data is transferred to zero-copy in Dart, it might be null
  // here
  if (result->rgba_data) {
    delete[] result->rgba_data;
    result->rgba_data = nullptr;
  }
  std::free(result);
}

void dng_free_halide_buffer(void *ptr) {
  if (!ptr)
    return;
  delete[] static_cast<uint8_t *>(ptr);
}

} // extern "C"
