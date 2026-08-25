#pragma once

#include <cstdint>
#include <vector>

#include <dng_host.h>
#include <dng_negative.h>
#include <dng_render.h>

#include "dng_pipeline_config.h"

enum class RenderHalideMode {
    SDK = 0,
    HALIDE_METAL = 1,
    HALIDE_GPU = 2,  // Platform-adaptive: Metal on Apple, Vulkan on Android
    AUTO = 3,
};

const char* renderHalideModeName(RenderHalideMode mode);

// W6-3 (TD-21): config parameter added so callers can pass a pre-loaded
// PipelineConfig instead of triggering per-call PipelineConfig::loadFromEnv().
bool render_stage4_halide(dng_host& host,
                          dng_negative& negative,
                          const dng_render& renderer,
                          RenderHalideMode mode,
                          const PipelineConfig& config,
                          std::vector<uint8_t>& out_rgb,
                          uint32_t& out_w,
                          uint32_t& out_h);

// Pool-backed overload: out_rgb_ptr must point to a pre-allocated buffer of
// at least out_rgb_size bytes (caller guarantees size >= W*H*3).
// Avoids vector resize / page-fault cost on the hot FFI path.
// W6-3 (TD-21): config parameter added.
bool render_stage4_halide(dng_host& host,
                          dng_negative& negative,
                          const dng_render& renderer,
                          RenderHalideMode mode,
                          const PipelineConfig& config,
                          uint8_t* out_rgb_ptr,
                          size_t out_rgb_size,
                          uint32_t& out_w,
                          uint32_t& out_h);

// R2 sized decode: the output extent the Stage4 render will produce for this
// negative + renderer (honours renderer.MaximumSize()). Lets the pipeline size
// its Stage4 output buffer by OUTPUT extent before the render runs, instead of
// by input extent.
void dng_render_stage4_output_size(const dng_negative& negative,
                                   const dng_render& renderer,
                                   uint32_t& out_w,
                                   uint32_t& out_h);

// Phase 8.2.2 — Stage3→Stage4 device-side handoff.
// stage3_device_buf must be device-dirty (from demosaic_warp dispatch).
// src_scale is typically 1.0f/65535.0f for uint16 Stage3 data.
// Returns false when not applicable (resample needed, params build failure,
// or renderHalideTryFull disabled) — caller must fall back to finish()+
// render_stage4_halide().
// W6-3 (TD-21): config parameter added (same as render_stage4_halide above).
struct halide_buffer_t;
bool render_stage4_halide_from_device_buffer(dng_host& host,
                                              dng_negative& negative,
                                              const dng_render& renderer,
                                              halide_buffer_t* stage3_device_buf,
                                              float src_scale,
                                              const PipelineConfig& config,
                                              std::vector<uint8_t>& out_rgb,
                                              uint32_t& out_w,
                                              uint32_t& out_h);

// Pool-backed overload for device handoff path.
// W6-3 (TD-21): config parameter added — caller passes pre-loaded PipelineConfig.
bool render_stage4_halide_from_device_buffer(dng_host& host,
                                              dng_negative& negative,
                                              const dng_render& renderer,
                                              halide_buffer_t* stage3_device_buf,
                                              float src_scale,
                                              const PipelineConfig& config,
                                              uint8_t* out_rgb_ptr,
                                              size_t out_rgb_size,
                                              uint32_t& out_w,
                                              uint32_t& out_h);

// W6-3 / TD-21 legacy overloads: kept for test_decode.cpp and other
// non-production callers that have no PipelineConfig handy.  Internally
// they call PipelineConfig::loadFromEnv() once and forward.  Production
// hot paths in dng_pipeline.cpp must NOT use these — pass the
// existing per-decode config explicitly to keep env reads at 1 per decode.
inline bool render_stage4_halide(dng_host& host,
                                  dng_negative& negative,
                                  const dng_render& renderer,
                                  RenderHalideMode mode,
                                  std::vector<uint8_t>& out_rgb,
                                  uint32_t& out_w,
                                  uint32_t& out_h) {
    const PipelineConfig config = PipelineConfig::loadFromEnv();
    return render_stage4_halide(host, negative, renderer, mode, config,
                                out_rgb, out_w, out_h);
}

// W7-E: idle-time prewarm of the Stage4 render AOT kernel
// (dng_render_stage4_split) at the actual image size, so the GPU pipeline
// state is built and cached before the first real decode of that resolution.
// Dispatches one pass on a zeroed dummy src with an identity RenderParams
// (built from the same toIdentity*/ensureSafeHueSatMap path buildRenderParams
// uses) and discards the result. Per-size cached; Android-only body (no-op on
// other platforms so macOS warmup behaviour is unchanged).
//
// G3: optional dst_rgba_ptr overload — when provided, the prewarm kernel
// writes to the caller's RGBA8 buffer (typically the pool RGBA buffer) instead
// of an internal dummy. This warms the GPU DMA path to the exact host pages
// production's copy_to_host will target, eliminating the ~63ms first-D2H
// page-fault / staging-buffer cold penalty.
void dng_render_stage4_prewarm_for_size(int width, int height);
void dng_render_stage4_prewarm_for_size(int width, int height,
                                         uint8_t* dst_rgba_ptr,
                                         size_t dst_rgba_size);

