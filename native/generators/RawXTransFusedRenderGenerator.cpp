// Fused generic-RAW X-Trans pipeline: 6x6 CFA mosaic -> display-referred
// output in ONE kernel, with no full-frame RGB16 Stage-3 intermediate.
//
// Sibling of RawBayerFusedRenderGenerator.cpp; read that file's header for the
// full rationale. Summary of what is inherited unchanged:
//   * the colour arithmetic is NOT copied: ceyx::build_dng_render_stage4_rgb /
//     build_dng_render_stage4_rgb8_exprs (dng_render_stage4_expr.h);
//   * the demosaic is NOT copied: build_xtrans_demosaic_expr
//     (dng_halide_utils.h), the same body raw_xtrans_demosaic runs;
//   * no platform guard: the only target test is has_gpu_feature();
//   * nothing is staged on the GPU (runtime orientation affine => dynamic
//     staged extent => refused on Vulkan < 1.3; Adreno sub-32-bit Workgroup
//     defect). check_gpu_producer_width.py enforces this on the lowered stmt.
// The mosaic is passed WHOLE and the crop travels as scalars: cropping would
// shift the 6x6 phase and the repeat-6 boundary wrap.
#include "Halide.h"
#include "ceyx_yuv420_write.h"
#include "dng_halide_utils.h"
#include "dng_render_stage4_expr.h"

using namespace Halide;

class RawXTransFusedRender : public Halide::Generator<RawXTransFusedRender> {
public:
    GeneratorParam<bool> guard_tail{"guard_tail", true};

    // --- demosaic side, matching RawXTransDemosaicGenerator's inputs ---
    Input<Buffer<uint16_t>> src{"src", 2};      // x, y CFA mosaic (FULL plane)
    Input<Buffer<int32_t>> cfa{"cfa", 2};       // 6x6, values {0,1,2}
    Input<Buffer<float>> black{"black", 2};     // black repeat tile
    Input<float> inv_range{"inv_range"};        // 65535 / (white - black_max)
    Input<int32_t> crop_x{"crop_x"};
    Input<int32_t> crop_y{"crop_y"};
    Input<int32_t> src_w{"src_w"};              // CROPPED extents
    Input<int32_t> src_h{"src_h"};

    // --- render side: byte-for-byte the Bayer fused tail (binding, A1) ---
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
    Output<Buffer<uint8_t>> dst{"dst", 3};      // x, y, c (RGBA8 interleaved)

    Func demosaiced{"demosaiced"};   // INLINE, never scheduled
    Func rendered_rgb{"rendered_rgb"};

    void generate() {
        Var x("x"), y("y"), c("c");

        src.dim(0).set_stride(1);
        cfa.dim(0).set_bounds(0, 6);
        cfa.dim(1).set_bounds(0, 6);
        dst.dim(0).set_stride(4);
        dst.dim(2).set_bounds(0, 4);
        dst.dim(2).set_stride(1);

        Expr width = src.dim(0).extent();
        Expr height = src.dim(1).extent();
        Expr bw = max(black.dim(0).extent(), 1);
        Expr bh = max(black.dim(1).extent(), 1);

        demosaiced(x, y, c) = build_xtrans_demosaic_expr(
            x, y, c,
            [&](Expr sx, Expr sy) { return src(sx, sy); },
            [&](Expr i, Expr j) { return cfa(i, j); },
            [&](Expr i, Expr j) { return black(i, j); },
            width, height, bw, bh, inv_range);

        // THE SEAM: the render reads the demosaic directly.
        ceyx::build_dng_render_stage4_rgb(
            x, y,
            [&](Expr sample_x, Expr sample_y, int channel) {
                return demosaiced(sample_x + crop_x, sample_y + crop_y, channel);
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
            // Nothing staged: see the header and RawBayerFusedRenderGenerator.cpp.
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

// yuv420 OUTPUT VARIANT. Separate class because output arity differs (same
// reason as RawBayerFusedRenderYuv420). Type rules, both load-bearing:
//   (a) never birth a colour channel in an 8/16-bit type on the yuv420 path:
//       channels are born int32 below;
//   (b) no Tuple Func on this path (D2 Vulkan miscompile when a Tuple is
//       consumed by index at several coordinates): the EXPR colour body is used.
class RawXTransFusedRenderYuv420
    : public Halide::Generator<RawXTransFusedRenderYuv420> {
public:
    GeneratorParam<bool> guard_tail{"guard_tail", true};

    // Input list is RawXTransFusedRender's verbatim.
    Input<Buffer<uint16_t>> src{"src", 2};
    Input<Buffer<int32_t>> cfa{"cfa", 2};
    Input<Buffer<float>> black{"black", 2};
    Input<float> inv_range{"inv_range"};
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

    Func demosaiced{"demosaiced"};
    // Deliberately NO rendered_rgb member (no Tuple Func on this path).

    void generate() {
        Var x("x"), y("y"), c("c");

        src.dim(0).set_stride(1);
        cfa.dim(0).set_bounds(0, 6);
        cfa.dim(1).set_bounds(0, 6);
        y_plane.dim(0).set_stride(1);
        cb_plane.dim(0).set_stride(1);
        cr_plane.dim(0).set_stride(1);

        Expr width = src.dim(0).extent();
        Expr height = src.dim(1).extent();
        Expr bw = max(black.dim(0).extent(), 1);
        Expr bh = max(black.dim(1).extent(), 1);

        demosaiced(x, y, c) = build_xtrans_demosaic_expr(
            x, y, c,
            [&](Expr sx, Expr sy) { return src(sx, sy); },
            [&](Expr i, Expr j) { return cfa(i, j); },
            [&](Expr i, Expr j) { return black(i, j); },
            width, height, bw, bh, inv_range);

        Expr enc_r, enc_g, enc_b;
        ceyx::build_dng_render_stage4_rgb8_exprs(
            x, y,
            [&](Expr sample_x, Expr sample_y, int channel) {
                return demosaiced(sample_x + crop_x, sample_y + crop_y, channel);
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
            enc_r, enc_g, enc_b);

        // Channels born int32 (rule a), three single-value INLINE Funcs.
        Func rgb8_r("rgb8_r"), rgb8_g("rgb8_g"), rgb8_b("rgb8_b");
        rgb8_r(x, y) = cast<int32_t>(enc_r);
        rgb8_g(x, y) = cast<int32_t>(enc_g);
        rgb8_b(x, y) = cast<int32_t>(enc_b);

        ceyx::build_yuv420_planes(x, y, rgb8_r, rgb8_g, rgb8_b,
                                  y_plane.dim(0).extent(),
                                  y_plane.dim(1).extent(),
                                  y_plane, cb_plane, cr_plane);
    }

    void schedule() {
        Var x("x"), y("y");
        // NOTHING beyond the shared plane schedule (same as the Bayer sibling).
        ceyx::schedule_yuv420_planes(get_target(), guard_tail, x, y, y_plane,
                                     cb_plane, cr_plane);
    }
};

HALIDE_REGISTER_GENERATOR(RawXTransFusedRender, raw_xtrans_fused_render)
HALIDE_REGISTER_GENERATOR(RawXTransFusedRenderYuv420,
                          raw_xtrans_fused_render_yuv420)
