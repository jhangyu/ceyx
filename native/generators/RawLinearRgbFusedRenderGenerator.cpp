// Fused generic-RAW linear-RGB pipeline (Foveon X3F): interleaved U16 RGB ->
// display-referred RGBA8 / yuv420 planes in ONE kernel, with no Stage-3 RGB16
// intermediate allocated.
//
// WHAT THIS REPLACES: raw_linear_rgb_normalize (a normalize-only pre-pass into
// a per-lane Stage-3 buffer) followed by dng_render_stage4 reading it back. The
// two-stage pair is KEPT: it is the scaled-decode route (no fused _scaled
// archive exists), the DNG_RAW_FUSED_LINEAR_RGB_RENDER=0 control arm, and the
// CPU-oracle consumer.
//
// THE SEAM is the normalize arithmetic of RawLinearRgbNormalizeGenerator.cpp
// (black subtract, inv_range multiply, clamp, floor(v+0.5) half-up ROUNDING --
// not the Bayer kernels' truncation), evaluated inside the Stage-4 colour
// body's sample callback. Identical arithmetic on the same uint16 input is what
// makes the fused output byte-identical to the two-stage output (gates N2/G1).
//
// SOURCE SHAPE: flat 1-D uint16 with a manual (y*stride + x*3 + c) gather on
// EVERY backend. The 3-D interleaved channel-stride buffer is the Vulkan
// aliasing defect family (memory.md Gotcha #92); the flat gather is the
// verified-safe construct (Gotcha #95, dng_render_stage4_split_expr.h). One
// shape for all backends is the iron rule (2026-09-19). The stride is in
// ELEMENTS and is the decoder's own pitch; no %3 assumption.
//
// NOTHING IS STAGED: no neighbourhood taps, so no producer worth staging, and a
// staged producer under the runtime orientation affine has a dynamic extent
// that Vulkan refuses. check_gpu_producer_width.py enforces this on the
// lowered statement.
#include "Halide.h"
#include "ceyx_yuv420_write.h"
#include "dng_render_stage4_expr.h"

using namespace Halide;

namespace {

// The one normalize body both classes call. Arithmetic is
// RawLinearRgbNormalizeGenerator.cpp:52-55 verbatim; only the read differs
// (flat gather instead of a 3-D buffer access). `channel` is a compile-time int
// (the colour body calls sample(sx, sy, 0/1/2)), so black(channel) is a
// constant index -- no select, no runtime predicate.
template <typename SourceInput, typename BlackInput, typename FloatInput,
          typename IntInput>
Expr sample_linear_rgb_normalized(SourceInput &src, BlackInput &black,
                                  FloatInput &inv_range,
                                  IntInput &src_row_stride_elements,
                                  IntInput &crop_x, IntInput &crop_y,
                                  Expr sample_x, Expr sample_y, int channel) {
    Expr base = (sample_y + crop_y) * src_row_stride_elements +
                (sample_x + crop_x) * 3;
    Expr raw = src(clamp(base + channel, 0, src.dim(0).extent() - 1));
    Expr level = black(channel);
    Expr v = (cast<float>(raw) - level) * inv_range;
    Expr value = clamp(v, 0.0f, 65535.0f);
    return cast<uint16_t>(clamp(floor(value + 0.5f), 0.0f, 65535.0f));
}

}  // namespace

class RawLinearRgbFusedRender
    : public Halide::Generator<RawLinearRgbFusedRender> {
public:
    GeneratorParam<bool> guard_tail{"guard_tail", true};

    // --- normalize side ---
    Input<Buffer<uint16_t>> src{"src", 1};          // flat interleaved RGB
    Input<Buffer<float>> black{"black", 1};         // 3 per-component entries
    Input<float> inv_range{"inv_range"};            // 65535 / (white - max black)
    Input<int32_t> src_row_stride_elements{"src_row_stride_elements"};
    Input<int32_t> crop_x{"crop_x"};
    Input<int32_t> crop_y{"crop_y"};
    Input<int32_t> src_w{"src_w"};                  // CROPPED extents
    Input<int32_t> src_h{"src_h"};

    // --- render side: RawBayerFusedRenderGenerator.cpp:108-141 verbatim ---
    Input<float> src_scale{"src_scale"};
    Input<int32_t> orient_a_x{"orient_a_x"};
    Input<int32_t> orient_b_x{"orient_b_x"};
    Input<int32_t> orient_c_x{"orient_c_x"};
    Input<int32_t> orient_a_y{"orient_a_y"};
    Input<int32_t> orient_b_y{"orient_b_y"};
    Input<int32_t> orient_c_y{"orient_c_y"};
    Input<Buffer<float>> exp_ramp{"exp_ramp", 1};
    Input<Buffer<float>> tone_curve{"tone_curve", 1};
    Input<Buffer<float>> encode_gamma{"encode_gamma", 1};
    Input<Buffer<float>> camera_white{"camera_white", 1};
    Input<Buffer<float>> camera_to_rgb{"camera_to_rgb", 2};
    Input<Buffer<float>> rgb_to_final{"rgb_to_final", 2};
    Input<Buffer<float>> huesat_table{"huesat_table", 2};
    Input<Buffer<float>> huesat_encode{"huesat_encode", 1};
    Input<Buffer<float>> huesat_decode{"huesat_decode", 1};
    Input<int32_t> huesat_hue_div{"huesat_hue_div"};
    Input<int32_t> huesat_sat_div{"huesat_sat_div"};
    Input<int32_t> huesat_val_div{"huesat_val_div"};
    Input<int32_t> huesat_has_table{"huesat_has_table"};
    Input<int32_t> huesat_has_encoding{"huesat_has_encoding"};
    Input<Buffer<float>> look_table{"look_table", 2};
    Input<Buffer<float>> look_encode{"look_encode", 1};
    Input<Buffer<float>> look_decode{"look_decode", 1};
    Input<int32_t> look_hue_div{"look_hue_div"};
    Input<int32_t> look_sat_div{"look_sat_div"};
    Input<int32_t> look_val_div{"look_val_div"};
    Input<int32_t> look_has_table{"look_has_table"};
    Input<int32_t> look_has_encoding{"look_has_encoding"};
    Output<Buffer<uint8_t>> dst{"dst", 3};          // x, y, c (RGBA8 interleaved)

    Func rendered_rgb{"rendered_rgb"};

    void generate() {
        Var x("x"), y("y"), c("c");

        src.dim(0).set_stride(1);
        black.dim(0).set_bounds(0, 3);
        dst.dim(0).set_stride(4);
        dst.dim(2).set_bounds(0, 4);
        dst.dim(2).set_stride(1);

        ceyx::build_dng_render_stage4_rgb(
            x, y,
            [&](Expr sample_x, Expr sample_y, int channel) {
                return sample_linear_rgb_normalized(
                    src, black, inv_range, src_row_stride_elements, crop_x,
                    crop_y, sample_x, sample_y, channel);
            },
            src_w, src_h, src_scale,
            orient_a_x, orient_b_x, orient_c_x,
            orient_a_y, orient_b_y, orient_c_y,
            exp_ramp, tone_curve, encode_gamma, camera_white,
            camera_to_rgb, rgb_to_final,
            huesat_table, huesat_encode, huesat_decode,
            huesat_hue_div, huesat_sat_div, huesat_val_div,
            huesat_has_table, huesat_has_encoding,
            look_table, look_encode, look_decode,
            look_hue_div, look_sat_div, look_val_div,
            look_has_table, look_has_encoding,
            rendered_rgb);

        dst(x, y, c) = select(c == 0, rendered_rgb(x, y)[0],
                              c == 1, rendered_rgb(x, y)[1],
                              c == 2, rendered_rgb(x, y)[2],
                                      cast<uint8_t>(255));
    }

    void schedule() {
        Var x("x"), y("y"), c("c");
        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            dst.bound(c, 0, 4)
               .reorder(c, x, y);
            if (guard_tail) {
                dst.gpu_tile(x, y, xo, yo, xi, yi, 16, 16,
                             TailStrategy::GuardWithIf);
            } else {
                dst.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            }
            dst.unroll(c);
        } else {
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, 4)
               .reorder(c, x, y)
               .split(y, yo, yi, 32)
               .parallel(yo)
               .vectorize(x, 8)
               .unroll(c);
        }
    }
};

// yuv420 output variant: a second class because a generator cannot switch
// output arity on a GeneratorParam. Same input list, same seam, the un-Tupled
// colour body (a Tuple consumed at 5 coordinates per pixel is miscompiled on
// the project's Vulkan driver, dng_render_stage4_expr.h:59-82), channels born
// int32 (type rule 304e3abd), and the shared plane write + schedule.
class RawLinearRgbFusedRenderYuv420
    : public Halide::Generator<RawLinearRgbFusedRenderYuv420> {
public:
    GeneratorParam<bool> guard_tail{"guard_tail", true};

    Input<Buffer<uint16_t>> src{"src", 1};
    Input<Buffer<float>> black{"black", 1};
    Input<float> inv_range{"inv_range"};
    Input<int32_t> src_row_stride_elements{"src_row_stride_elements"};
    Input<int32_t> crop_x{"crop_x"};
    Input<int32_t> crop_y{"crop_y"};
    Input<int32_t> src_w{"src_w"};
    Input<int32_t> src_h{"src_h"};
    Input<float> src_scale{"src_scale"};
    Input<int32_t> orient_a_x{"orient_a_x"};
    Input<int32_t> orient_b_x{"orient_b_x"};
    Input<int32_t> orient_c_x{"orient_c_x"};
    Input<int32_t> orient_a_y{"orient_a_y"};
    Input<int32_t> orient_b_y{"orient_b_y"};
    Input<int32_t> orient_c_y{"orient_c_y"};
    Input<Buffer<float>> exp_ramp{"exp_ramp", 1};
    Input<Buffer<float>> tone_curve{"tone_curve", 1};
    Input<Buffer<float>> encode_gamma{"encode_gamma", 1};
    Input<Buffer<float>> camera_white{"camera_white", 1};
    Input<Buffer<float>> camera_to_rgb{"camera_to_rgb", 2};
    Input<Buffer<float>> rgb_to_final{"rgb_to_final", 2};
    Input<Buffer<float>> huesat_table{"huesat_table", 2};
    Input<Buffer<float>> huesat_encode{"huesat_encode", 1};
    Input<Buffer<float>> huesat_decode{"huesat_decode", 1};
    Input<int32_t> huesat_hue_div{"huesat_hue_div"};
    Input<int32_t> huesat_sat_div{"huesat_sat_div"};
    Input<int32_t> huesat_val_div{"huesat_val_div"};
    Input<int32_t> huesat_has_table{"huesat_has_table"};
    Input<int32_t> huesat_has_encoding{"huesat_has_encoding"};
    Input<Buffer<float>> look_table{"look_table", 2};
    Input<Buffer<float>> look_encode{"look_encode", 1};
    Input<Buffer<float>> look_decode{"look_decode", 1};
    Input<int32_t> look_hue_div{"look_hue_div"};
    Input<int32_t> look_sat_div{"look_sat_div"};
    Input<int32_t> look_val_div{"look_val_div"};
    Input<int32_t> look_has_table{"look_has_table"};
    Input<int32_t> look_has_encoding{"look_has_encoding"};

    Output<Buffer<uint8_t>> y_plane{"y_plane", 2};
    Output<Buffer<uint8_t>> cb_plane{"cb_plane", 2};
    Output<Buffer<uint8_t>> cr_plane{"cr_plane", 2};

    void generate() {
        Var x("x"), y("y");

        src.dim(0).set_stride(1);
        black.dim(0).set_bounds(0, 3);
        y_plane.dim(0).set_stride(1);
        cb_plane.dim(0).set_stride(1);
        cr_plane.dim(0).set_stride(1);

        Expr encoded_red, encoded_green, encoded_blue;
        ceyx::build_dng_render_stage4_rgb8_exprs(
            x, y,
            [&](Expr sample_x, Expr sample_y, int channel) {
                return sample_linear_rgb_normalized(
                    src, black, inv_range, src_row_stride_elements, crop_x,
                    crop_y, sample_x, sample_y, channel);
            },
            src_w, src_h, src_scale,
            orient_a_x, orient_b_x, orient_c_x,
            orient_a_y, orient_b_y, orient_c_y,
            exp_ramp, tone_curve, encode_gamma, camera_white,
            camera_to_rgb, rgb_to_final,
            huesat_table, huesat_encode, huesat_decode,
            huesat_hue_div, huesat_sat_div, huesat_val_div,
            huesat_has_table, huesat_has_encoding,
            look_table, look_encode, look_decode,
            look_hue_div, look_sat_div, look_val_div,
            look_has_table, look_has_encoding,
            encoded_red, encoded_green, encoded_blue);

        Func rgb8_r("rgb8_r"), rgb8_g("rgb8_g"), rgb8_b("rgb8_b");
        rgb8_r(x, y) = cast<int32_t>(encoded_red);
        rgb8_g(x, y) = cast<int32_t>(encoded_green);
        rgb8_b(x, y) = cast<int32_t>(encoded_blue);

        ceyx::build_yuv420_planes(x, y, rgb8_r, rgb8_g, rgb8_b,
                                  y_plane.dim(0).extent(),
                                  y_plane.dim(1).extent(),
                                  y_plane, cb_plane, cr_plane);
    }

    void schedule() {
        Var x("x"), y("y");
        ceyx::schedule_yuv420_planes(get_target(), guard_tail, x, y, y_plane,
                                     cb_plane, cr_plane);
    }
};

HALIDE_REGISTER_GENERATOR(RawLinearRgbFusedRender, raw_linear_rgb_fused_render)
HALIDE_REGISTER_GENERATOR(RawLinearRgbFusedRenderYuv420,
                          raw_linear_rgb_fused_render_yuv420)
