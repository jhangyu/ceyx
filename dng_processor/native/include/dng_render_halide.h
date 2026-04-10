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

