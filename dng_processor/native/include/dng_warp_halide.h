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
    double rad_params64[4][4] = {};
    double tan_params64[4][2] = {};
    double center_h64 = 0.5;
    double center_v64 = 0.5;
    double pixel_aspect_ratio64 = 1.0;
    bool is_rad_nop_all = false;
    bool is_tan_nop_all = false;
};

enum class WarpRectilinearMode {
    SDK,
    HALIDE_CPU,
    HALIDE_METAL,
    AUTO
};

bool extractWarpRectilinearParams(const dng_opcode_WarpRectilinear& opcode,
                                  double pixelAspectRatio,
                                  WarpRectilinearParams& params);

bool warp_rectilinear_halide(const uint16_t* src_interleaved_rgb,
                             int width,
                             int height,
                             int planes,
                             const WarpRectilinearParams& params,
                             WarpRectilinearMode mode,
                             uint16_t* dst_interleaved_rgb);

bool demosaic_warp_rectilinear_halide(const uint16_t* src_bayer,
                                      int width,
                                      int height,
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
