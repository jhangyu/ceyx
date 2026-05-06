#include "Halide.h"

using namespace Halide;

class DngRenderNoMapStage4 : public Halide::Generator<DngRenderNoMapStage4> {
public:
    Input<Buffer<uint16_t>> src{"src", 3};          // x, y, c (Stage3 interleaved RGB uint16)
    Input<float> src_scale{"src_scale"};            // usually 1 / 65535
    Input<Buffer<float>> exp_ramp{"exp_ramp", 1};   // 4098
    Input<Buffer<float>> tone_curve{"tone_curve", 1}; // 4098
    Input<Buffer<float>> encode_gamma{"encode_gamma", 1}; // 4098
    Input<Buffer<float>> camera_white{"camera_white", 1}; // 3
    Input<Buffer<float>> camera_to_rgb{"camera_to_rgb", 2}; // [col, row] 3x3
    Input<Buffer<float>> rgb_to_final{"rgb_to_final", 2};   // [col, row] 3x3
    Output<Buffer<uint8_t>> dst{"dst", 3};          // x, y, c

    void generate() {
        Var x("x"), y("y"), c("c");

        src.dim(0).set_stride(3);
        src.dim(2).set_bounds(0, 3);
        src.dim(2).set_stride(1);
        dst.dim(0).set_stride(3);
        dst.dim(2).set_bounds(0, 3);
        dst.dim(2).set_stride(1);

        Expr r = cast<float>(src(x, y, 0)) * src_scale;
        Expr g = cast<float>(src(x, y, 1)) * src_scale;
        Expr b = cast<float>(src(x, y, 2)) * src_scale;

        Expr wb_r = min(r, camera_white(0));
        Expr wb_g = min(g, camera_white(1));
        Expr wb_b = min(b, camera_white(2));

        Expr p_r0 = clamp(
            wb_r * camera_to_rgb(0, 0) + wb_g * camera_to_rgb(1, 0) + wb_b * camera_to_rgb(2, 0),
            0.0f, 1.0f);
        Expr p_g0 = clamp(
            wb_r * camera_to_rgb(0, 1) + wb_g * camera_to_rgb(1, 1) + wb_b * camera_to_rgb(2, 1),
            0.0f, 1.0f);
        Expr p_b0 = clamp(
            wb_r * camera_to_rgb(0, 2) + wb_g * camera_to_rgb(1, 2) + wb_b * camera_to_rgb(2, 2),
            0.0f, 1.0f);

        auto table_interp = [&](const auto& table, Expr v) {
            Expr xv = clamp(v, 0.0f, 1.0f);
            Expr max_idx = table.dim(0).extent() - 2;
            Expr yv = xv * cast<float>(max_idx);
            Expr idx = clamp(cast<int>(floor(yv)), 0, max_idx);
            Expr frac = yv - cast<float>(idx);
            Expr a = table(idx);
            Expr b = table(idx + 1);
            return a * (1.0f - frac) + b * frac;
        };

        Expr e_r = table_interp(exp_ramp, p_r0);
        Expr e_g = table_interp(exp_ramp, p_g0);
        Expr e_b = table_interp(exp_ramp, p_b0);

        auto rgb_tone = [&](Expr r0, Expr g0, Expr b0, Expr& rr, Expr& gg, Expr& bb) {
            Expr tr = table_interp(tone_curve, r0);
            Expr tg = table_interp(tone_curve, g0);
            Expr tb = table_interp(tone_curve, b0);

            Expr rr1 = tr;
            Expr den1 = select((r0 >= g0) && (g0 > b0), r0 - b0, 1.0f);
            Expr gg1 = tb + ((tr - tb) * (g0 - b0) / den1);
            Expr bb1 = tb;

            Expr bb2 = tb;
            Expr gg2 = tg;
            Expr den2 = select((r0 >= g0) && !(g0 > b0) && (b0 > r0), b0 - g0, 1.0f);
            Expr rr2 = gg2 + ((bb2 - gg2) * (r0 - g0) / den2);

            Expr rr3 = tr;
            Expr gg3 = tg;
            Expr den3 = select((r0 >= g0) && !(g0 > b0) && !(b0 > r0) && (b0 > g0), r0 - g0, 1.0f);
            Expr bb3 = gg3 + ((rr3 - gg3) * (b0 - g0) / den3);

            Expr rr4 = tr;
            Expr gg4 = tg;
            Expr bb4 = tg;

            Expr gg5 = tg;
            Expr bb5 = tb;
            Expr den5 = select(!(r0 >= g0) && (r0 >= b0), g0 - b0, 1.0f);
            Expr rr5 = bb5 + ((gg5 - bb5) * (r0 - b0) / den5);

            Expr bb6 = tb;
            Expr rr6 = tr;
            Expr den6 = select(!(r0 >= g0) && !(r0 >= b0) && (b0 > g0), b0 - r0, 1.0f);
            Expr gg6 = rr6 + ((bb6 - rr6) * (g0 - r0) / den6);

            Expr gg7 = tg;
            Expr rr7 = tr;
            Expr den7 = select(!(r0 >= g0) && !(r0 >= b0) && !(b0 > g0), g0 - r0, 1.0f);
            Expr bb7 = rr7 + ((gg7 - rr7) * (b0 - r0) / den7);

            Expr c1 = (r0 >= g0) && (g0 > b0);
            Expr c2 = (r0 >= g0) && !(g0 > b0) && (b0 > r0);
            Expr c3 = (r0 >= g0) && !(g0 > b0) && !(b0 > r0) && (b0 > g0);
            Expr c4 = (r0 >= g0) && !(g0 > b0) && !(b0 > r0) && !(b0 > g0);
            Expr c5 = !(r0 >= g0) && (r0 >= b0);
            Expr c6 = !(r0 >= g0) && !(r0 >= b0) && (b0 > g0);

            rr = select(c1, rr1, c2, rr2, c3, rr3, c4, rr4, c5, rr5, c6, rr6, rr7);
            gg = select(c1, gg1, c2, gg2, c3, gg3, c4, gg4, c5, gg5, c6, gg6, gg7);
            bb = select(c1, bb1, c2, bb2, c3, bb3, c4, bb4, c5, bb5, c6, bb6, bb7);
        };

        Expr t_r, t_g, t_b;
        rgb_tone(e_r, e_g, e_b, t_r, t_g, t_b);

        Expr f_r = clamp(
            t_r * rgb_to_final(0, 0) + t_g * rgb_to_final(1, 0) + t_b * rgb_to_final(2, 0),
            0.0f, 1.0f);
        Expr f_g = clamp(
            t_r * rgb_to_final(0, 1) + t_g * rgb_to_final(1, 1) + t_b * rgb_to_final(2, 1),
            0.0f, 1.0f);
        Expr f_b = clamp(
            t_r * rgb_to_final(0, 2) + t_g * rgb_to_final(1, 2) + t_b * rgb_to_final(2, 2),
            0.0f, 1.0f);

        auto encode8 = [&](Expr v) {
            Expr g8 = table_interp(encode_gamma, v);
            return clamp(g8 * 255.0f + 0.5f, 0.0f, 255.0f);
        };

        dst(x, y, c) = cast<uint8_t>(
            select(c == 0, encode8(f_r),
                   c == 1, encode8(f_g),
                            encode8(f_b)));
    }

    void schedule() {
        Var x("x"), y("y"), c("c");
        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            dst.bound(c, 0, 3)
               .reorder(c, x, y)
               .gpu_tile(x, y, xo, yo, xi, yi, 32, 8)
               .unroll(c);
        } else {
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, 3)
               .reorder(c, x, y)
               .split(y, yo, yi, 32)
               .parallel(yo)
               .vectorize(x, 8)
               .unroll(c);
        }
    }
};

HALIDE_REGISTER_GENERATOR(DngRenderNoMapStage4, dng_render_nomap_stage4)
