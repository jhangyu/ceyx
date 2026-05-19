#pragma once

#include <cstdint>
#include <vector>

#include <dng_host.h>
#include <dng_negative.h>
#include <dng_render.h>

enum class RenderHalideMode {
    SDK = 0,
    HALIDE_METAL = 1,
    AUTO = 2,
};

const char* renderHalideModeName(RenderHalideMode mode);

bool render_stage4_halide(dng_host& host,
                          dng_negative& negative,
                          const dng_render& renderer,
                          RenderHalideMode mode,
                          std::vector<uint8_t>& out_rgb,
                          uint32_t& out_w,
                          uint32_t& out_h);

// Pool-backed overload: out_rgb_ptr must point to a pre-allocated buffer of
// at least out_rgb_size bytes (caller guarantees size >= W*H*3).
// Avoids vector resize / page-fault cost on the hot FFI path.
bool render_stage4_halide(dng_host& host,
                          dng_negative& negative,
                          const dng_render& renderer,
                          RenderHalideMode mode,
                          uint8_t* out_rgb_ptr,
                          size_t out_rgb_size,
                          uint32_t& out_w,
                          uint32_t& out_h);

// Phase 8.2.2 — Stage3→Stage4 device-side handoff.
// stage3_device_buf must be device-dirty (from demosaic_warp dispatch).
// src_scale is typically 1.0f/65535.0f for uint16 Stage3 data.
// Returns false when not applicable (resample needed, params build failure,
// or renderHalideTryFull disabled) — caller must fall back to finish()+
// render_stage4_halide().
struct halide_buffer_t;
bool render_stage4_halide_from_device_buffer(dng_host& host,
                                              dng_negative& negative,
                                              const dng_render& renderer,
                                              halide_buffer_t* stage3_device_buf,
                                              float src_scale,
                                              std::vector<uint8_t>& out_rgb,
                                              uint32_t& out_w,
                                              uint32_t& out_h);

// Pool-backed overload for device handoff path.
bool render_stage4_halide_from_device_buffer(dng_host& host,
                                              dng_negative& negative,
                                              const dng_render& renderer,
                                              halide_buffer_t* stage3_device_buf,
                                              float src_scale,
                                              uint8_t* out_rgb_ptr,
                                              size_t out_rgb_size,
                                              uint32_t& out_w,
                                              uint32_t& out_h);

