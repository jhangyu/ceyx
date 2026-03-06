#include "dng_ffi_api.h"
#include "DngDecoder.h"
#include "HalidePipeline.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>

extern "C" {

DngResult *dng_decode_and_process(const char *file_path) {
  DngResult *result =
      static_cast<DngResult *>(std::calloc(1, sizeof(DngResult)));
  if (!result)
    return nullptr;

  // --- Phase 2: DNG decode ---
  DngDecoder decoder;
  DngMetadata metadata = {};

  auto t0 = std::chrono::steady_clock::now();
  DngErrorCode code = decoder.decodeFile(file_path, metadata);
  auto t1 = std::chrono::steady_clock::now();
  result->decode_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

  if (code != DngErrorCode::SUCCESS) {
    result->error_code = static_cast<int32_t>(code);
    std::cerr << "[FFI] DNG decode failed: " << result->error_code << "\n";
    return result;
  }

  // --- Phase 3+5a: Halide pipeline (now includes HueSatMap/LookTable) ---
  int outW = 0, outH = 0;
  auto t2 = std::chrono::steady_clock::now();
  uint8_t *rgba = HalidePipeline::process(
      decoder.getRawBuffer(), static_cast<int>(metadata.width),
      static_cast<int>(metadata.height), metadata.blackLevel,
      metadata.whiteLevel, metadata.asShotNeutral, metadata.camToSrgb,
      metadata.baselineExposure, metadata, outW, outH);

  auto t3 = std::chrono::steady_clock::now();
  result->process_ms =
      std::chrono::duration<double, std::milli>(t3 - t2).count();

  if (!rgba) {
    result->error_code = -10;
    std::cerr << "[FFI] Halide pipeline failed\n";
    return result;
  }

  result->rgba_data = rgba;
  result->width = outW;
  result->height = outH;
  result->error_code = 0;

  std::cerr << "[FFI] Success: " << outW << "x" << outH
            << " decode=" << result->decode_ms
            << "ms halide=" << result->process_ms << "ms\n";
  return result;
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
  // The buffer was allocated via `new uint8_t[]` in HalidePipeline::process
  delete[] static_cast<uint8_t *>(ptr);
}

} // extern "C"
