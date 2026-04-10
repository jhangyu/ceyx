#ifndef DNG_WARP_HALIDE_H_
#define DNG_WARP_HALIDE_H_

#include <cstdint>

#include <dng_auto_ptr.h>
#include <dng_image.h>

class dng_host;
class dng_negative;
class dng_opcode;
class dng_opcode_WarpRectilinear;

struct WarpRectilinearParams {
    uint32_t planes = 0;
    float rad_params[4][4] = {};
    float tan_params[4][2] = {};
    float center_h = 0.5f;
    float center_v = 0.5f;
    float pixel_aspect_ratio = 1.0f;
};

enum class WarpRectilinearMode {
    SDK,
    HALIDE_CPU,
    HALIDE_METAL,
    AUTO
};

bool extractWarpRectilinearParams(const dng_opcode_WarpRectilinear& opcode,
                                  float pixelAspectRatio,
                                  WarpRectilinearParams& params);

bool warp_rectilinear_halide(const uint16_t* src_interleaved_rgb,
                             int width,
                             int height,
                             int planes,
                             const WarpRectilinearParams& params,
                             WarpRectilinearMode mode,
                             uint16_t* dst_interleaved_rgb);

bool apply_warp_rectilinear_to_image(dng_host& host,
                                     dng_negative& negative,
                                     const dng_opcode_WarpRectilinear& opcode,
                                     AutoPtr<dng_image>& image,
                                     WarpRectilinearMode mode);

const char* warpRectilinearModeName(WarpRectilinearMode mode);

#endif  // DNG_WARP_HALIDE_H_
