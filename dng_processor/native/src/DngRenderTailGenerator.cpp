#include "Halide.h"

using namespace Halide;

class DngRenderTailStage4 : public Halide::Generator<DngRenderTailStage4> {
public:
    Input<Buffer<float>> src{"src", 3};               // x, y, c (tone-mapped linear RGB)
    Input<Buffer<float>> rgb_to_final{"rgb_to_final", 2};  // [col, row] 3x3
    Input<Buffer<float>> encode_gamma{"encode_gamma", 1};  // 4098
    Output<Buffer<uint8_t>> dst{"dst", 3};            // x, y, c interleaved RGB8

    void generate() {
        Var x("x"), y("y"), c("c");

        // Compile with explicit interleaved layout support for input/output RGB buffers.
        src.dim(0).set_stride(3);
        src.dim(2).set_bounds(0, 3);
        src.dim(2).set_stride(1);
        dst.dim(0).set_stride(3);
        dst.dim(2).set_bounds(0, 3);
        dst.dim(2).set_stride(1);

        Expr s_r = src(x, y, 0);
        Expr s_g = src(x, y, 1);
        Expr s_b = src(x, y, 2);

        Expr f_r_sum = s_r * rgb_to_final(0, 0) + s_g * rgb_to_final(1, 0) + s_b * rgb_to_final(2, 0);
        Expr f_g_sum = s_r * rgb_to_final(0, 1) + s_g * rgb_to_final(1, 1) + s_b * rgb_to_final(2, 1);
        Expr f_b_sum = s_r * rgb_to_final(0, 2) + s_g * rgb_to_final(1, 2) + s_b * rgb_to_final(2, 2);
#if DNG_RENDER_TAIL_STRICT_R
        f_r_sum = strict_float(f_r_sum);
#endif
#if DNG_RENDER_TAIL_STRICT_G
        f_g_sum = strict_float(f_g_sum);
#endif
#if DNG_RENDER_TAIL_STRICT_B
        f_b_sum = strict_float(f_b_sum);
#endif
        Expr f_r = clamp(f_r_sum, 0.0f, 1.0f);
        Expr f_g = clamp(f_g_sum, 0.0f, 1.0f);
        Expr f_b = clamp(f_b_sum, 0.0f, 1.0f);

        auto table_interp = [&](Expr v) {
            Expr xv = clamp(v, 0.0f, 1.0f);
            Expr max_idx = encode_gamma.dim(0).extent() - 2;
            Expr yv = xv * cast<float>(max_idx);
            Expr idx = clamp(cast<int>(floor(yv)), 0, max_idx);
            Expr frac = yv - cast<float>(idx);
            Expr a = encode_gamma(idx);
            Expr b = encode_gamma(idx + 1);
            return a * (1.0f - frac) + b * frac;
        };

        auto encode8 = [&](Expr v) {
            Expr g = table_interp(v);
            return clamp(g * 255.0f + 0.5f, 0.0f, 255.0f);
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

HALIDE_REGISTER_GENERATOR(DngRenderTailStage4, dng_render_tail_stage4)
