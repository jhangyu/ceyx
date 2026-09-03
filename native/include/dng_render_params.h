#ifndef DNG_RENDER_PARAMS_H_
#define DNG_RENDER_PARAMS_H_

// Extracted from dng_render_halide.cpp's anonymous namespace so a second
// translation unit (the LibRaw parameter builder) can reach the SHARED Stage4
// core. Nothing here is new logic: the struct body and the function signatures
// are byte-for-byte what dng_render_halide.cpp already defined.
//
// This is one shared Stage4 core, not a per-decoder copy (spec section 13.1):
// exactly two run* declarations exist below, and both frontends call them.

#include <cstdint>
#include <vector>

#include "dng_1d_table.h"
#include "dng_auto_ptr.h"
#include "dng_host.h"
#include "dng_hue_sat_map.h"
#include "dng_matrix.h"
#include "dng_negative.h"
#include "dng_render.h"

#include "dng_pipeline_config.h"

struct halide_buffer_t;

// Transcribed unchanged from dng_render_halide.cpp:369-407.
struct RenderParams {
    dng_vector camera_white_vec;
    dng_matrix camera_to_rgb_mat;
    dng_matrix rgb_to_final_mat;
    dng_1d_table exp_table_ref;
    dng_1d_table tone_table_ref;
    dng_1d_table gamma_table_ref;
    AutoPtr<dng_hue_sat_map> huesat_map_ref;
    AutoPtr<dng_hue_sat_map> look_map_ref;
    AutoPtr<dng_1d_table> huesat_encode_ref;
    AutoPtr<dng_1d_table> huesat_decode_ref;
    AutoPtr<dng_1d_table> look_encode_ref;
    AutoPtr<dng_1d_table> look_decode_ref;

    float camera_white[3] = {1.0f, 1.0f, 1.0f};
    float camera_to_rgb[9] = {};
    float rgb_to_final[9] = {};
    std::vector<float> exp_ramp;
    std::vector<float> tone_curve;
    std::vector<float> encode_gamma;

    std::vector<float> huesat_table;
    std::vector<float> huesat_encode;
    std::vector<float> huesat_decode;
    int32_t huesat_hue_div = 0;
    int32_t huesat_sat_div = 0;
    int32_t huesat_val_div = 0;
    int32_t huesat_has_table = 0;
    int32_t huesat_has_encoding = 0;

    std::vector<float> look_table;
    std::vector<float> look_encode;
    std::vector<float> look_decode;
    int32_t look_hue_div = 0;
    int32_t look_sat_div = 0;
    int32_t look_val_div = 0;
    int32_t look_has_table = 0;
    int32_t look_has_encoding = 0;
};

// DNG-side parameter builder (unchanged behaviour).
// Signature transcribed from dng_render_halide.cpp:768-772.
bool buildRenderParams(dng_host& host,
                       dng_negative& negative,
                       const dng_render& renderer,
                       const PipelineConfig& config,
                       RenderParams& params);

// Mutex rework (plan Task 4): per-decode state container. Forward declaration
// only — the definition lives in src/pipeline/decode_context.h, which is not on
// every consumer's include path. A null ctx means "no decode frame" (the plain
// dng_host harness paths); see the two Stage-4 entry points below.
struct DecodeContext;

// THE shared Stage4 core. Plain buffers + RenderParams, no decoder state.
// Signature transcribed from dng_render_halide.cpp:932-944.
//
// ctx (mutex rework Task 4): supplies the per-decode bump arena used for the
// RGBA strip scratch on the !fuse_rgba path. Null is legal — the callers that
// pass fuse_rgba=true never need it, and the harness paths fall back to a
// per-call local allocation.
bool runRenderStage4HalideAot(const uint16_t* src,
                              int src_w,
                              int src_h,
                              int src_p,
                              int src_row_step,
                              int src_col_step,
                              int src_plane_step,
                              float src_scale,
                              int dst_w,
                              int dst_h,
                              const RenderParams& params,
                              uint8_t* dst,
                              bool fuse_rgba = false,
                              DecodeContext* ctx = nullptr);

// Device-handoff form. Signature transcribed from dng_render_halide.cpp:1182-1192
// (note the crop_l/crop_t/src_w/src_h parameters the plan placeholder omitted).
bool runRenderStage4HalideAotFromDevice(halide_buffer_t* stage3_device_buf,
                                        float src_scale,
                                        int crop_l,
                                        int crop_t,
                                        int src_w,
                                        int src_h,
                                        int dst_w,
                                        int dst_h,
                                        const RenderParams& params,
                                        uint8_t* dst,
                                        bool fuse_rgba = false,
                                        DecodeContext* ctx = nullptr);

// Needed by the LibRaw builder so "identity" is explicit, never uninitialised
// (spec section 7.1.4). Signatures transcribed from dng_render_halide.cpp:663-667
// and :731 — note all four ints are by reference; the helper itself sets the
// divisions to 2/2/2 and has_table to 0.
void toIdentityHueSatMap(std::vector<float>& table,
                         int32_t& hue_div,
                         int32_t& sat_div,
                         int32_t& val_div,
                         int32_t& has_table);
void toIdentityCurve(std::vector<float>& table);

#endif  // DNG_RENDER_PARAMS_H_
