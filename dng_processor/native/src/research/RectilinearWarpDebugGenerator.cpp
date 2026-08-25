// Phase 8.1.6 Stage E B2-V2 — Halide-side per-pixel intermediate dump.
//
// MIRROR FROM RectilinearWarpGenerator.cpp lines 76-138 (Phase 8.1.6 D-1, polynomial
// path only; precompute_coords / pre_* inputs are intentionally omitted).
// If production polynomial formula ever changes, sync this generator block
// for block and update the line range above.
//
// Differences vs production:
//   * No `precompute_coords` GeneratorParam, no `pre_base_*` / `pre_frac_*`
//     inputs (instrumentation only targets the polynomial code path).
//   * Adds 8 instrumentation Output buffers exposing src_x/src_y/base_x/
//     base_y/frac_x_idx/frac_y_idx/clipped_x/clipped_y per (x, y, c). Same
//     dimensions as `dst` (W, H, 3).
//   * dst still computed from the same Expr chain; debug outputs are wired
//     from the *same* Expr nodes so any divergence we observe in the dbg
//     buffers is also driving the dst pixel value.
//
// CMake (B3-A) will compile this generator twice: once with target=`host`
// emitting `rectilinear_warp_debug_cpu.{a,h}`, and once with the production AOT
// target (`host-metal-no_asserts-no_bounds_query`-no_runtime) emitting
// `rectilinear_warp_debug_metal.{a,h}`.
//
// See: docs/logs/2026-05-09/phase8_1_6_stageE/v2_probe_design.md

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

class RectilinearWarpDebug : public Halide::Generator<RectilinearWarpDebug> {
public:
    // Mirror production fast_codegen=true behaviour. Schedule equivalence
    // is critical (see v2_probe_design.md § 2.4 / § 6 R2): if dbg outputs
    // were on a different tile structure, Metal codegen could emit different
    // FMA contraction for the polynomial Expr in dst vs dbg, masking the
    // very drift we are trying to capture.
    GeneratorParam<bool> fast_codegen{"fast_codegen", true};

    // ===== Inputs (mirror production except precompute_coords path) =====
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

    // ===== Outputs: dst + 8 instrumentation buffers =====
    Output<Buffer<uint16_t>> dst{"dst", 3};
    Output<Buffer<float>>    dbg_src_x{"dbg_src_x", 3};
    Output<Buffer<float>>    dbg_src_y{"dbg_src_y", 3};
    Output<Buffer<int32_t>>  dbg_base_x{"dbg_base_x", 3};
    Output<Buffer<int32_t>>  dbg_base_y{"dbg_base_y", 3};
    Output<Buffer<int32_t>>  dbg_frac_x_idx{"dbg_frac_x_idx", 3};
    Output<Buffer<int32_t>>  dbg_frac_y_idx{"dbg_frac_y_idx", 3};
    Output<Buffer<uint8_t>>  dbg_clipped_x{"dbg_clipped_x", 3};
    Output<Buffer<uint8_t>>  dbg_clipped_y{"dbg_clipped_y", 3};

    Func src_clamped{"src_clamped"};

    void generate() {
        Var x("x"), y("y"), c("c");

        // Mirror production interleaved layout for src/dst.
        src.dim(0).set_stride(3);
        src.dim(2).set_bounds(0, 3);
        src.dim(2).set_stride(1);
        tile_bounds.dim(0).set_bounds(0, 4);
        dst.dim(0).set_stride(3);
        dst.dim(2).set_bounds(0, 3);
        dst.dim(2).set_stride(1);

        // Debug buffers: planar (default) layout, c-extent = 3.
        auto bind_planar = [](OutputImageParam p) {
            p.dim(2).set_bounds(0, 3);
        };
        bind_planar(dbg_src_x);
        bind_planar(dbg_src_y);
        bind_planar(dbg_base_x);
        bind_planar(dbg_base_y);
        bind_planar(dbg_frac_x_idx);
        bind_planar(dbg_frac_y_idx);
        bind_planar(dbg_clipped_x);
        bind_planar(dbg_clipped_y);

        Expr plane = select(planes <= 1, 0, select(c < planes, c, 0));
        plane = clamp(plane, 0, rad.dim(1).extent() - 1);

        // === MIRROR FROM RectilinearWarpGenerator.cpp (else-branch polynomial) ===
        Expr diff_x = cast<float>(x) - center_x;
        Expr diff_y = cast<float>(y) - center_y;

        Expr diff_norm_x = diff_x * inv_norm_radius;
        Expr diff_norm_y = diff_y * inv_norm_radius;
        Expr diff_scaled_x = diff_norm_x;
        Expr diff_scaled_y = diff_norm_y * pixel_scale_v;

        Expr rr = min(diff_scaled_x * diff_scaled_x + diff_scaled_y * diff_scaled_y, 1.0f);

        Expr k0 = rad(0, plane);
        Expr k1 = rad(1, plane);
        Expr k2 = rad(2, plane);
        Expr k3 = rad(3, plane);
        Expr ratio = k0 + rr * (k1 + rr * (k2 + rr * k3));

        Expr kt0 = tan(0, plane);
        Expr kt1 = tan(1, plane);

        Expr tan_v = kt0 * (rr + 2.0f * diff_scaled_y * diff_scaled_y) +
                     (2.0f * kt1 * diff_scaled_x * diff_scaled_y);
        Expr tan_h = kt1 * (rr + 2.0f * diff_scaled_x * diff_scaled_x) +
                     (2.0f * kt0 * diff_scaled_x * diff_scaled_y);

        Expr src_x_rad = center_x + diff_x * ratio;
        Expr src_y_rad = center_y + diff_y * ratio;
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

        Expr frac_x_idx = clamp(cast<int>(floor((src_x - src_x_floor) * kResampleSubsampleCount2D)),
                                0,
                                kResampleSubsampleCount2D - 1);
        Expr frac_y_idx = clamp(cast<int>(floor((src_y - src_y_floor) * kResampleSubsampleCount2D)),
                                0,
                                kResampleSubsampleCount2D - 1);

        Expr base_x = cast<int>(src_x_floor) - 1;
        Expr base_y = cast<int>(src_y_floor) - 1;

        // === Tile clipping grid (mirror production) ===
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

        // === Instrumentation outputs: same Exprs, exposed as separate buffers ===
        dbg_src_x(x, y, c) = src_x;
        dbg_src_y(x, y, c) = src_y;
        dbg_base_x(x, y, c) = base_x;
        dbg_base_y(x, y, c) = base_y;
        dbg_frac_x_idx(x, y, c) = frac_x_idx;
        dbg_frac_y_idx(x, y, c) = frac_y_idx;
        dbg_clipped_x(x, y, c) = cast<uint8_t>(clipped_x);
        dbg_clipped_y(x, y, c) = cast<uint8_t>(clipped_y);
    }

    void schedule() {
        Var x("x"), y("y"), c("c");

        if (get_target().has_gpu_feature()) {
            // Metal: identical 8x8 gpu_tile to production fast_codegen path.
            // All 9 outputs share the same tile structure so the polynomial
            // Expr is codegen'd identically for dst and dbg consumers.
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            auto gpu_schedule = [&](Func f) {
                f.bound(c, 0, 3)
                 .reorder(c, x, y)
                 .gpu_tile(x, y, xo, yo, xi, yi, 8, 8);
            };
            gpu_schedule(dst);
            gpu_schedule(dbg_src_x);
            gpu_schedule(dbg_src_y);
            gpu_schedule(dbg_base_x);
            gpu_schedule(dbg_base_y);
            gpu_schedule(dbg_frac_x_idx);
            gpu_schedule(dbg_frac_y_idx);
            gpu_schedule(dbg_clipped_x);
            gpu_schedule(dbg_clipped_y);
        } else {
            Var yo("yo"), yi("yi");
            auto cpu_schedule = [&](Func f) {
                f.bound(c, 0, 3)
                 .reorder(c, x, y)
                 .split(y, yo, yi, 32)
                 .parallel(yo)
                 .vectorize(x, 4);
            };
            cpu_schedule(dst);
            cpu_schedule(dbg_src_x);
            cpu_schedule(dbg_src_y);
            cpu_schedule(dbg_base_x);
            cpu_schedule(dbg_base_y);
            cpu_schedule(dbg_frac_x_idx);
            cpu_schedule(dbg_frac_y_idx);
            cpu_schedule(dbg_clipped_x);
            cpu_schedule(dbg_clipped_y);
        }
    }
};

HALIDE_REGISTER_GENERATOR(RectilinearWarpDebug, rectilinear_warp_debug)
