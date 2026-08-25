#ifndef RAW_RENDER_PARAMS_BUILDER_H_
#define RAW_RENDER_PARAMS_BUILDER_H_

// The LibRaw-side RenderParams builder (spec section 7.1.3). Fills the SAME
// RenderParams the DNG path fills, so both frontends feed one Stage4 core.
//
// No dng_negative and no LibRaw type appears in this interface: input is the
// plain-C contract.

#include "dng_render_params.h"
#include "raw_pipeline_contract.h"

// Returns false when input.camera_to_pcs.valid == 0 (no invented matrix,
// spec section 4.1.9) or when develop.output_space is not sRGB.
bool raw_build_render_params(const RawGpuInput& input,
                              const RawDevelopParams& develop,
                              RenderParams& params);

// Test affordance: build the DNG-side params for a file, so the generic
// builder can be cross-checked against production behaviour.
bool dng_render_params_for_test(const char* dng_path, RenderParams& out);

#endif  // RAW_RENDER_PARAMS_BUILDER_H_
