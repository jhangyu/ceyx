#include "Halide.h"

using namespace Halide;

namespace {

Expr cubic_weight(Expr x) {
    const float a = -0.75f;
    x = abs(x);
    return select(x >= 2.0f, 0.0f,
                  x >= 1.0f, (((a * x - 5.0f * a) * x + 8.0f * a) * x - 4.0f * a),
                  (((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f));
}

constexpr int kResampleSubsampleBits2D = 5;
constexpr int kResampleSubsampleCount2D = 1 << kResampleSubsampleBits2D;

}  // namespace

// Phase 8.1.6 Stage E A1 — strict_float variant of RectilinearWarp.
//
// Identical to DngWarpGenerator.cpp except for the R/B plane radial polynomial
// computation, which is wrapped in Halide::strict_float() to forbid Metal MSL
// FMA contraction (chromatic aberration drift root cause; see
// docs/logs/2026-05-09/phase8_1_6_stageE/a1_strict_float_design.md §3).
//
// Scope (R/B plane only; G plane is bit-exact identity warp and remains
// untouched):
//   - rr   (diff_scaled² sum, polynomial input)
//   - ratio = k0 + rr*(k1 + rr*(k2 + rr*k3))
//   - src_x_rad = center_x + diff_x * ratio
//   - src_y_rad = center_y + diff_y * ratio
//
// G plane (plane == 1) keeps the original FMA-friendly expressions. tan
// distortion, bicubic weights, and downstream floor/cast paths are NOT wrapped.
class RectilinearWarpStrictFloat : public Halide::Generator<RectilinearWarpStrictFloat> {
public:
    // Keep algorithm identical; reduce codegen pressure via simpler scheduling.
    GeneratorParam<bool> fast_codegen{"fast_codegen", true};
    // Phase 8.1.6 D-1: when true, skip polynomial src_x/src_y derivation and
    // read pre-computed base_x/base_y/frac_x_idx/frac_y_idx from input buffers
    // (host CPU computes them in double precision). This eliminates Metal
    // fast-math/FMA contraction on R/B plane chromatic aberration polynomial.
    GeneratorParam<bool> precompute_coords{"precompute_coords", false};

    Input<Buffer<uint16_t>> src{"src", 3};      // x, y, c
    Input<Buffer<float>> rad{"rad", 2};         // coeff, plane
    Input<Buffer<float>> tan{"tan", 2};         // coeff, plane
    Input<Buffer<int32_t>> tile_bounds{"tile_bounds", 3};  // bound(4), tile_x, tile_y
    Input<int32_t> planes{"planes"};
    Input<float> center_x{"center_x"};
    Input<float> center_y{"center_y"};
    Input<float> norm_radius{"norm_radius"};
    Input<float> inv_norm_radius{"inv_norm_radius"};
    Input<float> pixel_scale_v{"pixel_scale_v"};
    Input<float> pixel_scale_v_inv{"pixel_scale_v_inv"};
    Input<int32_t> is_rad_nop_all{"is_rad_nop_all"};
    Input<int32_t> is_tan_nop_all{"is_tan_nop_all"};
    Input<int32_t> tile_width{"tile_width"};
    Input<int32_t> tile_height{"tile_height"};
    // Phase 8.1.6 D-1: precomputed coord inputs (only consumed when
    // precompute_coords=true; ignored otherwise but always part of ABI to keep
    // host wiring identical between two AOT variants).
    Input<Buffer<int32_t>> pre_base_x{"pre_base_x", 3};       // x, y, c
    Input<Buffer<int32_t>> pre_base_y{"pre_base_y", 3};       // x, y, c
    Input<Buffer<int32_t>> pre_frac_x_idx{"pre_frac_x_idx", 3};
    Input<Buffer<int32_t>> pre_frac_y_idx{"pre_frac_y_idx", 3};

    Output<Buffer<uint16_t>> dst{"dst", 3};
    Func src_clamped{"src_clamped"};

    void generate() {
        Var x("x"), y("y"), c("c");

        // Optimize for Stage3 RGB interleaved buffers used by test/custom pipeline.
        src.dim(0).set_stride(3);
        src.dim(2).set_bounds(0, 3);
        src.dim(2).set_stride(1);
        tile_bounds.dim(0).set_bounds(0, 4);
        dst.dim(0).set_stride(3);
        dst.dim(2).set_bounds(0, 3);
        dst.dim(2).set_stride(1);
        // Phase 8.1.6 D-1: precomputed coord buffers are width × height × c.
        pre_base_x.dim(2).set_bounds(0, 3);
        pre_base_y.dim(2).set_bounds(0, 3);
        pre_frac_x_idx.dim(2).set_bounds(0, 3);
        pre_frac_y_idx.dim(2).set_bounds(0, 3);

        Expr plane = select(planes <= 1, 0, select(c < planes, c, 0));
        plane = clamp(plane, 0, rad.dim(1).extent() - 1);

        // Phase 8.1.6 Stage E A1: G plane (RGB c==1) is identity warp; only R/B
        // need strict_float to forbid Metal FMA contraction in the polynomial.
        Expr is_g_plane = (plane == 1);

        Expr base_x, base_y, frac_x_idx, frac_y_idx;
        if (precompute_coords) {
            // Pre-computed path: read host-CPU double-precision coords directly.
            // Skips polynomial / floor / bin to avoid Metal fast-math drift.
            base_x = pre_base_x(x, y, c);
            base_y = pre_base_y(x, y, c);
            frac_x_idx = clamp(pre_frac_x_idx(x, y, c), 0, kResampleSubsampleCount2D - 1);
            frac_y_idx = clamp(pre_frac_y_idx(x, y, c), 0, kResampleSubsampleCount2D - 1);
        } else {
            Expr diff_x = cast<float>(x) - center_x;
            Expr diff_y = cast<float>(y) - center_y;

            Expr diff_norm_x = diff_x * inv_norm_radius;
            Expr diff_norm_y = diff_y * inv_norm_radius;
            Expr diff_scaled_x = diff_norm_x;
            Expr diff_scaled_y = diff_norm_y * pixel_scale_v;

            // ====== Phase 8.1.6 Stage E A1: strict_float wrap (R/B only) ======
            // Original (G-plane / baseline) expression chain:
            Expr rr_default = min(diff_scaled_x * diff_scaled_x + diff_scaled_y * diff_scaled_y, 1.0f);

            Expr k0 = rad(0, plane);
            Expr k1 = rad(1, plane);
            Expr k2 = rad(2, plane);
            Expr k3 = rad(3, plane);

            // R/B-only strict_float chain: forbid Metal MSL FMA contraction on
            // each sub-expression. G plane (plane == 1) keeps rr_default /
            // ratio_default below, preserving FMA fusion for identity warp.
            Expr rr_strict = min(
                strict_float(strict_float(diff_scaled_x * diff_scaled_x) +
                             strict_float(diff_scaled_y * diff_scaled_y)),
                1.0f);
            Expr rr = select(is_g_plane, rr_default, rr_strict);

            Expr ratio_default = k0 + rr * (k1 + rr * (k2 + rr * k3));
            Expr ratio_strict = strict_float(
                k0 + strict_float(rr * strict_float(
                    k1 + strict_float(rr * strict_float(
                        k2 + strict_float(rr * k3))))));
            Expr ratio = select(is_g_plane, ratio_default, ratio_strict);
            // ===================================================================

            Expr kt0 = tan(0, plane);
            Expr kt1 = tan(1, plane);

            Expr tan_v = kt0 * (rr + 2.0f * diff_scaled_y * diff_scaled_y) +
                         (2.0f * kt1 * diff_scaled_x * diff_scaled_y);
            Expr tan_h = kt1 * (rr + 2.0f * diff_scaled_x * diff_scaled_x) +
                         (2.0f * kt0 * diff_scaled_x * diff_scaled_y);

            // Stage E A1: src_x_rad / src_y_rad ratio-application also wrapped
            // for R/B (design §3.2 L109–L110); G plane stays default.
            Expr src_x_rad_default = center_x + diff_x * ratio;
            Expr src_y_rad_default = center_y + diff_y * ratio;
            Expr src_x_rad_strict = center_x + strict_float(diff_x * ratio);
            Expr src_y_rad_strict = center_y + strict_float(diff_y * ratio);
            Expr src_x_rad = select(is_g_plane, src_x_rad_default, src_x_rad_strict);
            Expr src_y_rad = select(is_g_plane, src_y_rad_default, src_y_rad_strict);

            Expr src_x_tan = cast<float>(x) + norm_radius * tan_h;
            Expr src_y_tan = cast<float>(y) + norm_radius * tan_v * pixel_scale_v_inv;
            Expr src_x_both = center_x + norm_radius * (diff_norm_x * ratio + tan_h);
            Expr src_y_both = center_y + norm_radius * (diff_norm_y * ratio + tan_v * pixel_scale_v_inv);

            Expr src_x = select(is_tan_nop_all != 0,
                                src_x_rad,
                                is_rad_nop_all != 0,
                                src_x_tan,
                                src_x_both);
            Expr src_y = select(is_tan_nop_all != 0,
                                src_y_rad,
                                is_rad_nop_all != 0,
                                src_y_tan,
                                src_y_both);

            Expr src_x_floor = floor(src_x);
            Expr src_y_floor = floor(src_y);

            frac_x_idx = clamp(cast<int>(floor((src_x - src_x_floor) * kResampleSubsampleCount2D)),
                               0,
                               kResampleSubsampleCount2D - 1);
            frac_y_idx = clamp(cast<int>(floor((src_y - src_y_floor) * kResampleSubsampleCount2D)),
                               0,
                               kResampleSubsampleCount2D - 1);

            base_x = cast<int>(src_x_floor) - 1;
            base_y = cast<int>(src_y_floor) - 1;
        }

        Expr safe_tile_w = max(tile_width, 1);
        Expr safe_tile_h = max(tile_height, 1);
        Expr tile_x = clamp(x / safe_tile_w, 0, tile_bounds.dim(1).extent() - 1);
        Expr tile_y = clamp(y / safe_tile_h, 0, tile_bounds.dim(2).extent() - 1);

        Expr min_base_x = tile_bounds(0, tile_x, tile_y);
        Expr max_base_x = tile_bounds(1, tile_x, tile_y);
        Expr min_base_y = tile_bounds(2, tile_x, tile_y);
        Expr max_base_y = tile_bounds(3, tile_x, tile_y);

        Expr clipped_x = base_x < min_base_x || base_x > max_base_x;
        Expr clipped_y = base_y < min_base_y || base_y > max_base_y;

        Expr base_x_clamped = clamp(base_x, min_base_x, max_base_x);
        Expr base_y_clamped = clamp(base_y, min_base_y, max_base_y);

        Expr frac_x = select(clipped_x,
                             0.0f,
                             cast<float>(frac_x_idx) / kResampleSubsampleCount2D);
        Expr frac_y = select(clipped_y,
                             0.0f,
                             cast<float>(frac_y_idx) / kResampleSubsampleCount2D);

        Expr w0x = cubic_weight((-1.0f) - frac_x);
        Expr w1x = cubic_weight(0.0f - frac_x);
        Expr w2x = cubic_weight(1.0f - frac_x);
        Expr w3x = cubic_weight(2.0f - frac_x);
        Expr sumx = w0x + w1x + w2x + w3x;
        w0x /= sumx;
        w1x /= sumx;
        w2x /= sumx;
        w3x /= sumx;

        Expr w0y = cubic_weight((-1.0f) - frac_y);
        Expr w1y = cubic_weight(0.0f - frac_y);
        Expr w2y = cubic_weight(1.0f - frac_y);
        Expr w3y = cubic_weight(2.0f - frac_y);
        Expr sumy = w0y + w1y + w2y + w3y;
        w0y /= sumy;
        w1y /= sumy;
        w2y /= sumy;
        w3y /= sumy;

        src_clamped(x, y, c) = cast<float>(src(clamp(x, 0, src.dim(0).extent() - 1),
                                             clamp(y, 0, src.dim(1).extent() - 1),
                                             clamp(c, 0, src.dim(2).extent() - 1)));

        auto sample_row = [&](Expr yy, Expr wx0, Expr wx1, Expr wx2, Expr wx3) {
            return wx0 * src_clamped(base_x_clamped + 0, yy, c) +
                   wx1 * src_clamped(base_x_clamped + 1, yy, c) +
                   wx2 * src_clamped(base_x_clamped + 2, yy, c) +
                   wx3 * src_clamped(base_x_clamped + 3, yy, c);
        };

        Expr row0 = sample_row(base_y_clamped + 0, w0x, w1x, w2x, w3x);
        Expr row1 = sample_row(base_y_clamped + 1, w0x, w1x, w2x, w3x);
        Expr row2 = sample_row(base_y_clamped + 2, w0x, w1x, w2x, w3x);
        Expr row3 = sample_row(base_y_clamped + 3, w0x, w1x, w2x, w3x);

        Expr value = w0y * row0 + w1y * row1 + w2y * row2 + w3y * row3;
        dst(x, y, c) = cast<uint16_t>(clamp(value + 0.5f, 0.0f, 65535.0f));
    }

    void schedule() {
        Var x("x"), y("y"), c("c");

        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            if (fast_codegen) {
                // Simpler schedule reduces compile-time pressure on Metal codegen.
                dst.bound(c, 0, 3)
                   .reorder(c, x, y)
                   .gpu_tile(x, y, xo, yo, xi, yi, 8, 8);
            } else {
                dst.bound(c, 0, 3)
                   .reorder(c, x, y)
                   .gpu_tile(x, y, xo, yo, xi, yi, 16, 16)
                   .unroll(c);
            }
        } else {
            Var yo("yo"), yi("yi");
            if (fast_codegen) {
                dst.bound(c, 0, 3)
                   .reorder(c, x, y)
                   .split(y, yo, yi, 32)
                   .parallel(yo)
                   .vectorize(x, 4);
            } else {
                dst.bound(c, 0, 3)
                   .reorder(c, x, y)
                   .split(y, yo, yi, 32)
                   .parallel(yo)
                   .vectorize(x, 8)
                   .unroll(c);
            }
        }
    }
};

HALIDE_REGISTER_GENERATOR(RectilinearWarpStrictFloat, rectilinear_warp_strict_float)
