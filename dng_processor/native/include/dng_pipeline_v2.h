#pragma once

#include <cstdint>
#include <vector>

class dng_host;
class dng_negative;

struct DngPipelineV2Result {
  std::vector<uint8_t> rgb;
  uint32_t width = 0;
  uint32_t height = 0;
  double decode_ms = 0.0;
  double process_ms = 0.0;
  int32_t error_code = 0;
};

struct DngPipelineStage3Timing {
  double extract_stage2_ms = 0.0;
  double make_image_ms = 0.0;
  double resize_ms = 0.0;
  double demosaic_ms = 0.0;
  double fused_demosaic_warp_ms = 0.0;
  double fast_warp_setup_ms = 0.0;
  double inject_put_ms = 0.0;
  double apply_opcode3_ms = 0.0;
  double sdk_build_ms = 0.0;
  double total_ms = 0.0;
};

bool dng_pipeline_v2_decode_to_rgb(const char *file_path,
                                   DngPipelineV2Result &result);

bool dng_pipeline_v2_run_stage3(dng_host &host,
                                dng_negative &negative,
                                bool use_halide_bayer,
                                DngPipelineStage3Timing *timing);
