#include "Halide.h"
#include "dng_halide_utils.h"

using namespace Halide;

class RectilinearWarp : public Halide::Generator<RectilinearWarp> {
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

        // Warp coordinates: either from precomputed host-CPU buffers or from
        // the polynomial distortion shared with DngDemosaicWarp.
        WarpCoords coords;
        if (precompute_coords) {
            // Pre-computed path: read host-CPU double-precision coords directly.
            // Skips polynomial / floor / bin to avoid Metal fast-math drift.
            coords.base_x     = pre_base_x(x, y, c);
            coords.base_y     = pre_base_y(x, y, c);
            coords.frac_x_idx = clamp(pre_frac_x_idx(x, y, c), 0,
                                      kResampleSubsampleCount2D - 1);
            coords.frac_y_idx = clamp(pre_frac_y_idx(x, y, c), 0,
                                      kResampleSubsampleCount2D - 1);
        } else {
            coords = compute_warp_polynomial(
                x, y, plane,
                center_x, center_y, norm_radius, inv_norm_radius,
                pixel_scale_v, pixel_scale_v_inv,
                is_rad_nop_all, is_tan_nop_all,
                [&](int i, Expr p) { return rad(i, p); },
                [&](int i, Expr p) { return tan(i, p); });
        }

        // Clamped float view of src for bicubic sampling.
        src_clamped(x, y, c) = cast<float>(src(clamp(x, 0, src.dim(0).extent() - 1),
                                               clamp(y, 0, src.dim(1).extent() - 1),
                                               clamp(c, 0, src.dim(2).extent() - 1)));

        // Bicubic 4×4 interpolation (shared with DngDemosaicWarp).
        Expr value = build_warp_bicubic_expr(
            x, y, c, coords,
            tile_width, tile_height,
            [&](int i, Expr tx, Expr ty) { return tile_bounds(i, tx, ty); },
            tile_bounds.dim(1).extent(), tile_bounds.dim(2).extent(),
            [&](Expr sx, Expr sy, Expr sc) { return src_clamped(sx, sy, sc); });

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

HALIDE_REGISTER_GENERATOR(RectilinearWarp, rectilinear_warp)
