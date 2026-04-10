#include "Halide.h"

using namespace Halide;

class DngRenderToneTailStage4 : public Halide::Generator<DngRenderToneTailStage4> {
public:
    Input<Buffer<float>> src{"src", 3};               // x, y, c (pre-tone RGB)
    Input<Buffer<float>> tone_curve{"tone_curve", 1}; // 4098
    Input<Buffer<float>> rgb_to_final{"rgb_to_final", 2};  // [col, row] 3x3
    Input<Buffer<float>> encode_gamma{"encode_gamma", 1};  // 4098
    Output<Buffer<uint8_t>> dst{"dst", 3};            // x, y, c interleaved RGB8

    void generate() {
        Var x("x"), y("y"), c("c");

        src.dim(0).set_stride(3);
        src.dim(2).set_bounds(0, 3);
        src.dim(2).set_stride(1);
        dst.dim(0).set_stride(3);
        dst.dim(2).set_bounds(0, 3);
        dst.dim(2).set_stride(1);

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

        Expr r = src(x, y, 0);
        Expr g = src(x, y, 1);
        Expr b = src(x, y, 2);

        auto rgb_tone = [&](Expr r0, Expr g0, Expr b0, Expr& rr, Expr& gg, Expr& bb) {
            Expr tr = table_interp(tone_curve, r0);
            Expr tg = table_interp(tone_curve, g0);
            Expr tb = table_interp(tone_curve, b0);

            Expr rr1 = tr;
            Expr gg1 = tb + ((tr - tb) * (g0 - b0) / max(r0 - b0, 1e-8f));
            Expr bb1 = tb;

            // Case 2: b > r >= g (RGBTone(b, r, g, bb, rr, gg))
            Expr bb2 = tb;
            Expr gg2 = tg;
            Expr rr2 = gg2 + ((bb2 - gg2) * (r0 - g0) / max(b0 - g0, 1e-8f));

            // Case 3: r >= b > g (RGBTone(r, b, g, rr, bb, gg))
            Expr rr3 = tr;
            Expr gg3 = tg;
            Expr bb3 = gg3 + ((rr3 - gg3) * (b0 - g0) / max(r0 - g0, 1e-8f));

            Expr rr4 = tr;
            Expr gg4 = tg;
            Expr bb4 = tg;

            // Case 5: g > r >= b (RGBTone(g, r, b, gg, rr, bb))
            Expr gg5 = tg;
            Expr bb5 = tb;
            Expr rr5 = bb5 + ((gg5 - bb5) * (r0 - b0) / max(g0 - b0, 1e-8f));

            // Case 6: b > g > r (RGBTone(b, g, r, bb, gg, rr))
            Expr bb6 = tb;
            Expr rr6 = tr;
            Expr gg6 = rr6 + ((bb6 - rr6) * (g0 - r0) / max(b0 - r0, 1e-8f));

            // Case 7: g >= b > r (RGBTone(g, b, r, gg, bb, rr))
            Expr gg7 = tg;
            Expr rr7 = tr;
            Expr bb7 = rr7 + ((gg7 - rr7) * (b0 - r0) / max(g0 - r0, 1e-8f));

            Expr c1 = (r0 >= g0) && (g0 > b0);
            Expr c2 = (r0 >= g0) && !(g0 > b0) && (b0 > r0);
            Expr c3 = (r0 >= g0) && !(g0 > b0) && !(b0 > r0) && (b0 > g0);
            Expr c4 = (r0 >= g0) && !(g0 > b0) && !(b0 > r0) && !(b0 > g0);
            Expr c5 = !(r0 >= g0) && (r0 >= b0);
            Expr c6 = !(r0 >= g0) && !(r0 >= b0) && (b0 > g0);

            rr = select(c1, rr1,
                        c2, rr2,
                        c3, rr3,
                        c4, rr4,
                        c5, rr5,
                        c6, rr6,
                            rr7);
            gg = select(c1, gg1,
                        c2, gg2,
                        c3, gg3,
                        c4, gg4,
                        c5, gg5,
                        c6, gg6,
                            gg7);
            bb = select(c1, bb1,
                        c2, bb2,
                        c3, bb3,
                        c4, bb4,
                        c5, bb5,
                        c6, bb6,
                            bb7);
        };

        Expr tr, tg, tb;
        rgb_tone(r, g, b, tr, tg, tb);

        Expr fr = clamp(
            tr * rgb_to_final(0, 0) + tg * rgb_to_final(1, 0) + tb * rgb_to_final(2, 0),
            0.0f, 1.0f);
        Expr fg = clamp(
            tr * rgb_to_final(0, 1) + tg * rgb_to_final(1, 1) + tb * rgb_to_final(2, 1),
            0.0f, 1.0f);
        Expr fb = clamp(
            tr * rgb_to_final(0, 2) + tg * rgb_to_final(1, 2) + tb * rgb_to_final(2, 2),
            0.0f, 1.0f);

        auto encode8 = [&](Expr v) {
            Expr g8 = table_interp(encode_gamma, v);
            return clamp(g8 * 255.0f + 0.5f, 0.0f, 255.0f);
        };

        dst(x, y, c) = cast<uint8_t>(
            select(c == 0, encode8(fr),
                   c == 1, encode8(fg),
                            encode8(fb)));
    }

    void schedule() {
        Var x("x"), y("y"), c("c");
        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            dst.bound(c, 0, 3)
               .reorder(c, x, y)
               .gpu_tile(x, y, xo, yo, xi, yi, 16, 16)
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

HALIDE_REGISTER_GENERATOR(DngRenderToneTailStage4, dng_render_tonetail_stage4)
