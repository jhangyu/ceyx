#include "Halide.h"
#include "dng_halide_utils.h"

using namespace Halide;

class DngDemosaicWarp : public Halide::Generator<DngDemosaicWarp> {
public:
    GeneratorParam<bool> fast_codegen{"fast_codegen", true};

    Input<Buffer<uint16_t>> src{"src", 2};      // x, y Bayer CFA
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

    Output<Buffer<uint16_t>> dst{"dst", 3};

    Func demosaic{"demosaic"};
    Func rgb_clamped{"rgb_clamped"};

    void generate() {
        Var x("x"), y("y"), c("c");

        src.dim(0).set_stride(1);
        tile_bounds.dim(0).set_bounds(0, 4);
        dst.dim(0).set_stride(3);
        dst.dim(2).set_bounds(0, 3);
        dst.dim(2).set_stride(1);

        Expr width = src.dim(0).extent();
        Expr height = src.dim(1).extent();

        // Bilinear demosaic (shared with DngDemosaicGenerator).
        auto cfa_sample = [&](Expr sx, Expr sy) {
            Expr mx = map_repeat_coord(sx, width);
            Expr my = map_repeat_coord(sy, height);
            return src(mx, my);
        };
        demosaic(x, y, c) = build_demosaic_expr(x, y, c, cfa_sample);

        // Warp polynomial distortion (shared with RectilinearWarp).
        Expr plane = select(planes <= 1, 0, select(c < planes, c, 0));
        plane = clamp(plane, 0, rad.dim(1).extent() - 1);

        WarpCoords coords = compute_warp_polynomial(
            x, y, plane,
            center_x, center_y, norm_radius, inv_norm_radius,
            pixel_scale_v, pixel_scale_v_inv,
            is_rad_nop_all, is_tan_nop_all,
            [&](int i, Expr p) { return rad(i, p); },
            [&](int i, Expr p) { return tan(i, p); });

        // Clamped float view of demosaic output for bicubic sampling.
        rgb_clamped(x, y, c) = cast<float>(demosaic(clamp(x, 0, src.dim(0).extent() - 1),
                                                    clamp(y, 0, src.dim(1).extent() - 1),
                                                    clamp(c, 0, 2)));

        // Bicubic 4×4 interpolation (shared with RectilinearWarp).
        Expr value = build_warp_bicubic_expr(
            x, y, c, coords,
            tile_width, tile_height,
            [&](int i, Expr tx, Expr ty) { return tile_bounds(i, tx, ty); },
            tile_bounds.dim(1).extent(), tile_bounds.dim(2).extent(),
            [&](Expr sx, Expr sy, Expr sc) { return rgb_clamped(sx, sy, sc); });

        dst(x, y, c) = cast<uint16_t>(clamp(value + 0.5f, 0.0f, 65535.0f));
    }

    void schedule() {
        Var x("x"), y("y"), c("c");
        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            // [W1 materialization] compute_root demosaic into a 3D device buffer so
            // the bicubic warp samples a precomputed plane instead of re-running the
            // 144 gather/px demosaic per tap. macOS Metal: Stage3 fused -17% bit-exact.
            // [W6 H6 fix] align_bounds(x/y, 8, 0) forces this materialized allocation's
            // buffer min to literal 0 (no halo-derived min expression). The default
            // TailStrategy otherwise emits a non-trivial min (max(min(src.ext,8),1)+-8
            // form) that Vulkan/Adreno mishandles as a buffer-head device base offset,
            // corrupting the first rows of the R plane (W1b Stage3 96.81 dB regression).
            // W6 X1a/X3a confirmed the buffer-head mechanism; this restores Android
            // Vulkan to 102.72 dB, bit-exact with macOS Metal on both platforms.
            Var dxo("dxo"), dyo("dyo"), dxi("dxi"), dyi("dyi");
            demosaic.compute_root()
                    .bound(c, 0, 3)
                    .reorder(c, x, y)
                    .align_bounds(x, 8, 0)
                    .align_bounds(y, 8, 0)
                    .gpu_tile(x, y, dxo, dyo, dxi, dyi, 8, 8)
                    .unroll(c);
            if (fast_codegen) {
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
            demosaic.compute_root()
                    .bound(c, 0, 3)
                    .reorder(c, x, y)
                    .split(y, Var("dyo"), Var("dyi"), 32)
                    .parallel(Var("dyo"))
                    .vectorize(x, 8);
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, 3)
               .reorder(c, x, y)
               .split(y, yo, yi, 32)
               .parallel(yo)
               .vectorize(x, 4);
        }
    }
};

HALIDE_REGISTER_GENERATOR(DngDemosaicWarp, dng_demosaic_warp)
