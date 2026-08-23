#include "Halide.h"

using namespace Halide;

// W7 (2026-08-21, Windows port): the dense-planar src/dst layout selected below
// is a workaround for the Halide v21 SPIR-V Tuple/dim(2) codegen bug, i.e. a
// property of the *Vulkan* backend rather than of Android. Selecting it by
// backend lets the Windows-Vulkan AOT target reuse the same workaround.
// Android is unaffected: its AOT target string always carries the vulkan
// feature (CMakeLists.txt AOT_TARGET), so this predicate is true exactly where
// `os == Target::Android` used to be.
static bool uses_vulkan_planar_layout(const Target &t) {
    return t.os == Target::Android || t.has_feature(Target::Vulkan);
}


class DngRenderStage4 : public Halide::Generator<DngRenderStage4> {
public:
    // Android Vulkan workaround: Android AOT uses dense planar RGB src.
    // Other targets keep the original interleaved RGB layout for performance.
    Input<Buffer<uint16_t>> src{"src", 3};          // x, y, c
    Input<float> src_scale{"src_scale"};            // usually 1 / 65535
    Input<Buffer<float>> exp_ramp{"exp_ramp", 1};   // 4098
    Input<Buffer<float>> tone_curve{"tone_curve", 1}; // 4098
    Input<Buffer<float>> encode_gamma{"encode_gamma", 1}; // 4098
    Input<Buffer<float>> camera_white{"camera_white", 1}; // 3
    Input<Buffer<float>> camera_to_rgb{"camera_to_rgb", 2}; // [col, row] 3x3
    Input<Buffer<float>> rgb_to_final{"rgb_to_final", 2};   // [col, row] 3x3
    Input<Buffer<float>> huesat_table{"huesat_table", 2};   // [entry, component(0..2)]
    Input<Buffer<float>> huesat_encode{"huesat_encode", 1}; // 4098
    Input<Buffer<float>> huesat_decode{"huesat_decode", 1}; // 4098
    Input<int32_t> huesat_hue_div{"huesat_hue_div"};
    Input<int32_t> huesat_sat_div{"huesat_sat_div"};
    Input<int32_t> huesat_val_div{"huesat_val_div"};
    Input<int32_t> huesat_has_table{"huesat_has_table"};
    Input<int32_t> huesat_has_encoding{"huesat_has_encoding"};
    Input<Buffer<float>> look_table{"look_table", 2};       // [entry, component(0..2)]
    Input<Buffer<float>> look_encode{"look_encode", 1};     // 4098
    Input<Buffer<float>> look_decode{"look_decode", 1};     // 4098
    Input<int32_t> look_hue_div{"look_hue_div"};
    Input<int32_t> look_sat_div{"look_sat_div"};
    Input<int32_t> look_val_div{"look_val_div"};
    Input<int32_t> look_has_table{"look_has_table"};
    Input<int32_t> look_has_encoding{"look_has_encoding"};
    Output<Buffer<uint8_t>> dst{"dst", 3};          // x, y, c
    Func rendered_rgb{"rendered_rgb"};

    void generate() {
        Var x("x"), y("y"), c("c");

        if (uses_vulkan_planar_layout(get_target())) {
            // Vulkan SPIR-V workaround: use dense planar src so GPU code never
            // reads interleaved channel-stride-1 input on dim(2).
            src.dim(0).set_stride(1);
            src.dim(1).set_stride(src.dim(0).extent());
            src.dim(2).set_bounds(0, 3);
            src.dim(2).set_stride(src.dim(0).extent() * src.dim(1).extent());
        } else {
            // Original macOS/CPU layout: Stage3 produces interleaved RGB.
            src.dim(0).set_stride(3);
            src.dim(2).set_bounds(0, 3);
            src.dim(2).set_stride(1);
        }
        if (uses_vulkan_planar_layout(get_target())) {
            // Vulkan SPIR-V workaround: use dense planar dst so GPU code never
            // writes interleaved channel-stride-1 output on dim(2).
            dst.dim(0).set_stride(1);
            dst.dim(1).set_stride(dst.dim(0).extent());
            dst.dim(2).set_bounds(0, 3);
            dst.dim(2).set_stride(dst.dim(0).extent() * dst.dim(1).extent());
        } else {
            // W7 (M-11): macOS outputs RGBA8 (stride-4, alpha=255 in-kernel).
            // Eliminates the FFI rgb_to_rgba pass and one ~72MB RGB intermediate.
            dst.dim(0).set_stride(4);
            dst.dim(2).set_bounds(0, 4);
            dst.dim(2).set_stride(1);
        }

        Func src_f("src_f");
        src_f(x, y, c) = src(x, y, c);
        Expr sx = clamp(x, 0, src.dim(0).extent() - 1);
        Expr sy = clamp(y, 0, src.dim(1).extent() - 1);
        Expr s_r = cast<float>(src_f(sx, sy, 0)) * src_scale;
        Expr s_g = cast<float>(src_f(sx, sy, 1)) * src_scale;
        Expr s_b = cast<float>(src_f(sx, sy, 2)) * src_scale;

        Expr wb_r = min(s_r, camera_white(0));
        Expr wb_g = min(s_g, camera_white(1));
        Expr wb_b = min(s_b, camera_white(2));

        Expr p_r0_rg = wb_r * camera_to_rgb(0, 0) + wb_g * camera_to_rgb(1, 0);
        Expr p_g0_rg = wb_r * camera_to_rgb(0, 1) + wb_g * camera_to_rgb(1, 1);
        Expr p_b0_rg = wb_r * camera_to_rgb(0, 2) + wb_g * camera_to_rgb(1, 2);
        Expr p_r0_sum = p_r0_rg + wb_b * camera_to_rgb(2, 0);
        Expr p_g0_sum = p_g0_rg + wb_b * camera_to_rgb(2, 1);
        Expr p_b0_sum = p_b0_rg + wb_b * camera_to_rgb(2, 2);
        Expr p_r0 = clamp(p_r0_sum, 0.0f, 1.0f);
        Expr p_g0 = clamp(p_g0_sum, 0.0f, 1.0f);
        Expr p_b0 = clamp(p_b0_sum, 0.0f, 1.0f);

        Expr abc_r = p_r0;
        Expr abc_g = p_g0;
        Expr abc_b = p_b0;

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

        auto rgb_to_hsv = [&](Expr r, Expr g, Expr b, Expr& h, Expr& s, Expr& v) {
            v = max(r, max(g, b));
            Expr mn = min(r, min(g, b));
            Expr gap = v - mn;

            Expr gap_den = select(gap > 0.0f, gap, 1.0f);
            Expr h_r = (g - b) / gap_den;
            Expr h_r_fix = select(h_r < 0.0f, h_r + 6.0f, h_r);
            Expr h_g = 2.0f + (b - r) / gap_den;
            Expr h_b = 4.0f + (r - g) / gap_den;

            h = select(gap > 0.0f,
                       select(r == v, h_r_fix,
                              g == v, h_g,
                                      h_b),
                       0.0f);
            s = select(gap > 0.0f, gap / v, 0.0f);
        };

        auto hsv_to_rgb = [&](Expr h, Expr s, Expr v, Expr& r, Expr& g, Expr& b) {
            Expr use_sat = s > 0.0f;
            Expr hh = select(h < 0.0f, h + 6.0f, h);
            hh = select(hh >= 6.0f, hh - 6.0f, hh);
            Expr i = cast<int>(hh);
            Expr f = hh - cast<float>(i);
            Expr p = v * (1.0f - s);
            Expr q = v * (1.0f - s * f);
            Expr t = v * (1.0f - s * (1.0f - f));
            Expr cc = clamp(i, 0, 5);

            Expr r_hsv = select(cc == 0, v,
                                cc == 1, q,
                                cc == 2, p,
                                cc == 3, p,
                                cc == 4, t,
                                         v);
            Expr g_hsv = select(cc == 0, t,
                                cc == 1, v,
                                cc == 2, v,
                                cc == 3, q,
                                cc == 4, p,
                                         p);
            Expr b_hsv = select(cc == 0, p,
                                cc == 1, p,
                                cc == 2, t,
                                cc == 3, v,
                                cc == 4, v,
                                         q);

            // Match DNG SDK semantics: if saturation is non-positive, emit gray.
            r = select(use_sat, r_hsv, v);
            g = select(use_sat, g_hsv, v);
            b = select(use_sat, b_hsv, v);
        };

        auto sample_hsv_map = [&](const auto& table,
                                  const auto& encode_table,
                                  const auto& decode_table,
                                  Expr hue_div,
                                  Expr sat_div,
                                  Expr val_div,
                                  Expr has_table,
                                  Expr has_encoding,
                                  Expr r,
                                  Expr g,
                                  Expr b,
                                  Expr& out_r,
                                  Expr& out_g,
                                  Expr& out_b) {
            Expr h, s, v;
            rgb_to_hsv(r, g, b, h, s, v);

            // select() is not short-circuit in Halide; always keep dimensions valid.
            Expr hue_div_safe = max(hue_div, 2);
            Expr sat_div_safe = max(sat_div, 2);
            Expr val_div_safe = max(val_div, 1);

            Expr hue_scale = cast<float>(hue_div_safe) * (1.0f / 6.0f);
            Expr sat_scale = cast<float>(sat_div_safe - 1);
            Expr val_scale = cast<float>(val_div_safe - 1);
            Expr max_hue_index0 = hue_div_safe - 1;
            Expr max_sat_index0 = sat_div_safe - 2;
            Expr max_val_index0 = val_div_safe - 2;
            Expr hue_step = sat_div_safe;
            Expr val_step = hue_div_safe * sat_div_safe;

            Expr v_encoded0 = v;
            Expr use_encode = (has_encoding != 0) && (val_div_safe >= 2);
            Expr v_encoded = select(use_encode, table_interp(encode_table, clamp(v, 0.0f, 1.0f)), v_encoded0);

            Expr h_scaled = h * hue_scale;
            Expr s_scaled = s * sat_scale;
            Expr v_scaled = v_encoded * val_scale;

            Expr h_index0_raw = cast<int>(floor(h_scaled));
            Expr s_index0 = clamp(cast<int>(floor(s_scaled)), 0, cast<int>(max_sat_index0));
            Expr v_index0 = clamp(cast<int>(floor(v_scaled)), 0, cast<int>(max_val_index0));

            Expr h_index0 = clamp(h_index0_raw, 0, cast<int>(max_hue_index0));
            Expr h_index1 = select(h_index0_raw >= max_hue_index0, 0, h_index0 + 1);

            Expr h_fract1 = h_scaled - cast<float>(h_index0);
            Expr s_fract1 = s_scaled - cast<float>(s_index0);
            Expr v_fract1 = v_scaled - cast<float>(v_index0);
            Expr h_fract0 = 1.0f - h_fract1;
            Expr s_fract0 = 1.0f - s_fract1;
            Expr v_fract0 = 1.0f - v_fract1;

            auto tval = [&](Expr idx, int comp) {
                return table(clamp(cast<int>(idx), 0, table.dim(0).extent() - 1), comp);
            };

            Expr base2d0 = h_index0 * hue_step + s_index0;
            Expr base2d1 = h_index1 * hue_step + s_index0;

            Expr hs_hue0 = h_fract0 * tval(base2d0, 0) + h_fract1 * tval(base2d1, 0);
            Expr hs_sat0 = h_fract0 * tval(base2d0, 1) + h_fract1 * tval(base2d1, 1);
            Expr hs_val0 = h_fract0 * tval(base2d0, 2) + h_fract1 * tval(base2d1, 2);

            Expr hs_hue1 = h_fract0 * tval(base2d0 + 1, 0) + h_fract1 * tval(base2d1 + 1, 0);
            Expr hs_sat1 = h_fract0 * tval(base2d0 + 1, 1) + h_fract1 * tval(base2d1 + 1, 1);
            Expr hs_val1 = h_fract0 * tval(base2d0 + 1, 2) + h_fract1 * tval(base2d1 + 1, 2);

            Expr hue_shift_2d = s_fract0 * hs_hue0 + s_fract1 * hs_hue1;
            Expr sat_scale_2d = s_fract0 * hs_sat0 + s_fract1 * hs_sat1;
            Expr val_scale_2d = s_fract0 * hs_val0 + s_fract1 * hs_val1;

            Expr base3d00 = v_index0 * val_step + h_index0 * hue_step + s_index0;
            Expr base3d01 = v_index0 * val_step + h_index1 * hue_step + s_index0;
            Expr base3d10 = base3d00 + val_step;
            Expr base3d11 = base3d01 + val_step;

            auto lerp_hv = [&](int comp, Expr off) {
                return v_fract0 * (h_fract0 * tval(base3d00 + off, comp) + h_fract1 * tval(base3d01 + off, comp)) +
                       v_fract1 * (h_fract0 * tval(base3d10 + off, comp) + h_fract1 * tval(base3d11 + off, comp));
            };

            Expr hue_shift0_3d = lerp_hv(0, 0);
            Expr sat_scale0_3d = lerp_hv(1, 0);
            Expr val_scale0_3d = lerp_hv(2, 0);
            Expr hue_shift1_3d = lerp_hv(0, 1);
            Expr sat_scale1_3d = lerp_hv(1, 1);
            Expr val_scale1_3d = lerp_hv(2, 1);

            Expr hue_shift_3d = s_fract0 * hue_shift0_3d + s_fract1 * hue_shift1_3d;
            Expr sat_scale_3d = s_fract0 * sat_scale0_3d + s_fract1 * sat_scale1_3d;
            Expr val_scale_3d = s_fract0 * val_scale0_3d + s_fract1 * val_scale1_3d;

            Expr use_2d = val_div_safe < 2;
            Expr hue_shift = hue_shift_3d;
            Expr sat_mult = sat_scale_3d;
            Expr val_mult = val_scale_3d;

            Expr hh = h + hue_shift * (6.0f / 360.0f);
            Expr ss = min(s * sat_mult, 1.0f);
            Expr ve = clamp(v_encoded * val_mult, 0.0f, 1.0f);
            Expr vv = select(use_encode, table_interp(decode_table, ve), ve);

            Expr rr, gg, bb;
            hsv_to_rgb(hh, ss, vv, rr, gg, bb);

            out_r = select(has_table != 0, rr, r);
            out_g = select(has_table != 0, gg, g);
            out_b = select(has_table != 0, bb, b);
        };

        Expr p_r1, p_g1, p_b1;
        sample_hsv_map(huesat_table,
                       huesat_encode,
                       huesat_decode,
                       huesat_hue_div,
                       huesat_sat_div,
                       huesat_val_div,
                       huesat_has_table,
                       huesat_has_encoding,
                       abc_r,
                       abc_g,
                       abc_b,
                       p_r1,
                       p_g1,
                       p_b1);

        Expr e_r = table_interp(exp_ramp, p_r1);
        Expr e_g = table_interp(exp_ramp, p_g1);
        Expr e_b = table_interp(exp_ramp, p_b1);

        Expr p_r2, p_g2, p_b2;
        sample_hsv_map(look_table,
                       look_encode,
                       look_decode,
                       look_hue_div,
                       look_sat_div,
                       look_val_div,
                       look_has_table,
                       look_has_encoding,
                       e_r,
                       e_g,
                       e_b,
                       p_r2,
                       p_g2,
                       p_b2);

        auto rgb_tone = [&](Expr r, Expr g, Expr b, Expr& rr, Expr& gg, Expr& bb) {
            Expr tr = table_interp(tone_curve, r);
            Expr tg = table_interp(tone_curve, g);
            Expr tb = table_interp(tone_curve, b);

            Expr rr1 = tr;
            Expr den1 = select((r >= g) && (g > b), r - b, 1.0f);
            Expr gg1 = tb + ((tr - tb) * (g - b) / den1);
            Expr bb1 = tb;

            // Case 2: b > r >= g (RGBTone(b, r, g, bb, rr, gg))
            Expr bb2 = tb;
            Expr gg2 = tg;
            Expr den2 = select((r >= g) && !(g > b) && (b > r), b - g, 1.0f);
            Expr rr2 = gg2 + ((bb2 - gg2) * (r - g) / den2);

            // Case 3: r >= b > g (RGBTone(r, b, g, rr, bb, gg))
            Expr rr3 = tr;
            Expr gg3 = tg;
            Expr den3 = select((r >= g) && !(g > b) && !(b > r) && (b > g), r - g, 1.0f);
            Expr bb3 = gg3 + ((rr3 - gg3) * (b - g) / den3);

            Expr rr4 = tr;
            Expr gg4 = tg;
            Expr bb4 = tg;

            // Case 5: g > r >= b (RGBTone(g, r, b, gg, rr, bb))
            Expr gg5 = tg;
            Expr bb5 = tb;
            Expr den5 = select(!(r >= g) && (r >= b), g - b, 1.0f);
            Expr rr5 = bb5 + ((gg5 - bb5) * (r - b) / den5);

            // Case 6: b > g > r (RGBTone(b, g, r, bb, gg, rr))
            Expr bb6 = tb;
            Expr rr6 = tr;
            Expr den6 = select(!(r >= g) && !(r >= b) && (b > g), b - r, 1.0f);
            Expr gg6 = rr6 + ((bb6 - rr6) * (g - r) / den6);

            // Case 7: g >= b > r (RGBTone(g, b, r, gg, bb, rr))
            Expr gg7 = tg;
            Expr rr7 = tr;
            Expr den7 = select(!(r >= g) && !(r >= b) && !(b > g), g - r, 1.0f);
            Expr bb7 = rr7 + ((gg7 - rr7) * (b - r) / den7);

            Expr c1 = (r >= g) && (g > b);
            Expr c2 = (r >= g) && !(g > b) && (b > r);
            Expr c3 = (r >= g) && !(g > b) && !(b > r) && (b > g);
            Expr c4 = (r >= g) && !(g > b) && !(b > r) && !(b > g);
            Expr c5 = !(r >= g) && (r >= b);
            Expr c6 = !(r >= g) && !(r >= b) && (b > g);

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

        Expr t_r, t_g, t_b;
        rgb_tone(p_r2, p_g2, p_b2, t_r, t_g, t_b);

        // Keep accumulation order deterministic at the matrix stage.
        Expr tone_r = t_r;
        Expr tone_g = t_g;
        Expr tone_b = t_b;
        Expr f_r_rg = tone_r * rgb_to_final(0, 0) + tone_g * rgb_to_final(1, 0);
        Expr f_g_rg = tone_r * rgb_to_final(0, 1) + tone_g * rgb_to_final(1, 1);
        Expr f_b_rg = tone_r * rgb_to_final(0, 2) + tone_g * rgb_to_final(1, 2);
        Expr f_r_sum = f_r_rg + tone_b * rgb_to_final(2, 0);
        Expr f_g_sum = f_g_rg + tone_b * rgb_to_final(2, 1);
        Expr f_b_sum = f_b_rg + tone_b * rgb_to_final(2, 2);

        Expr f_r = clamp(f_r_sum, 0.0f, 1.0f);
        Expr f_g = clamp(f_g_sum, 0.0f, 1.0f);
        Expr f_b = clamp(f_b_sum, 0.0f, 1.0f);

        auto encode8 = [&](Expr v) {
            Expr g = table_interp(encode_gamma, v);
            return clamp(g * 255.0f + 0.5f, 0.0f, 255.0f);
        };

        rendered_rgb(x, y) = Tuple(cast<uint8_t>(encode8(f_r)),
                                   cast<uint8_t>(encode8(f_g)),
                                   cast<uint8_t>(encode8(f_b)));

        if (uses_vulkan_planar_layout(get_target())) {
            dst(x, y, c) = select(c == 0, rendered_rgb(x, y)[0],
                                  c == 1, rendered_rgb(x, y)[1],
                                          rendered_rgb(x, y)[2]);
        } else {
            // W7 (M-11): RGBA8 output with alpha=255 in-kernel.
            dst(x, y, c) = select(c == 0, rendered_rgb(x, y)[0],
                                  c == 1, rendered_rgb(x, y)[1],
                                  c == 2, rendered_rgb(x, y)[2],
                                          cast<uint8_t>(255));
        }
    }

    void schedule() {
        Var x("x"), y("y"), c("c");
        // W7 (M-11): macOS outputs 4 channels (RGBA8); Android (separate
        // DngRenderStage4Android generator) stays 3. This generator is only
        // compiled for non-Android targets.
        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            // P12-W1-03: fuse rendered_rgb into dst via compute_at to eliminate
            // the ~145.8 MB intermediate GPU global-memory roundtrip and collapse
            // two Metal kernel dispatches into one. dst now drives the 16x16 tile.
            dst.bound(c, 0, 4)
               .reorder(c, x, y)
               .gpu_tile(x, y, xo, yo, xi, yi, 16, 16)
               .unroll(c);
            rendered_rgb.compute_at(dst, xo)
                        .gpu_threads(x, y);
        } else {
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, 4)
               .reorder(c, x, y)
               .split(y, yo, yi, 32)
               .parallel(yo)
               .vectorize(x, 8)
               .unroll(c);
            rendered_rgb.compute_at(dst, yo)
                        .vectorize(x, 8);
        }
    }
};

// =============================================================================
// Android Vulkan Stage4 generator.
// G2 (Round 2): outputs interleaved RGBA8 directly (same dst layout as the
// macOS kernel: dim0 stride 4, dim2 stride 1 bounds [0,4)), retiring the
// planar-output workaround + host repack. The interleaved dst construct was
// re-verified SAFE on Halide v21 Vulkan (Adreno 750) by the G2 pre-check
// Probe A — fully-inlined kernel, 0/24,000,000 per-channel mismatches at
// 6000x4000 (docs/logs/2026-07-04/Task_g2_vulkan_rg_bug_recheck.md).
// HARD CONSTRAINT: do NOT port the macOS `rendered_rgb.compute_at` schedule —
// any materialized compute_at producer still collapses G/B to the last select
// branch on Vulkan (pre-check Probe B, re-confirmed 2026-07-05 with
// align_bounds; TailStrategy::GuardWithIf is a compile error on this target:
// dynamic workgroup sizes need Vulkan v1.3). The kernel body must stay fully
// inlined; unroll(c) + select folds c per copy and CSE shares the
// c-independent pipeline body, so inlining costs no redundant compute.
// =============================================================================
class DngRenderStage4Android : public Halide::Generator<DngRenderStage4Android> {
public:
    GeneratorParam<int32_t> diag_stage{"diag_stage", -1};

    // W2: single flat-1D interleaved src (replaces three planar src_r/g/b).
    // Contents = SDK interleaved RGB buffer (row-major, channel stride 1) laid
    // out as one 1D plane of size src_row_stride_px * src_height * 3. The host
    // zero-copy wraps the SDK buffer (no repack_src). Verified bit-exact on
    // Vulkan by the W4-2 probe (Gotcha #95).
    Input<Buffer<uint16_t>> src_rgb{"src_rgb", 1};
    Input<int32_t> src_width{"src_width"};
    Input<int32_t> src_height{"src_height"};
    Input<int32_t> src_row_stride_px{"src_row_stride_px"};
    Input<int32_t> crop_l{"crop_l"};
    Input<int32_t> crop_t{"crop_t"};
    Input<float> src_scale{"src_scale"};
    Input<Buffer<float>> exp_ramp{"exp_ramp", 1};
    Input<Buffer<float>> tone_curve{"tone_curve", 1};
    Input<Buffer<float>> encode_gamma{"encode_gamma", 1};
    Input<Buffer<float>> camera_white{"camera_white", 1};
    Input<Buffer<float>> camera_to_rgb{"camera_to_rgb", 1};
    Input<Buffer<float>> rgb_to_final{"rgb_to_final", 1};
    Input<Buffer<float>> huesat_table{"huesat_table", 1};
    Input<Buffer<float>> huesat_encode{"huesat_encode", 1};
    Input<Buffer<float>> huesat_decode{"huesat_decode", 1};
    Input<int32_t> huesat_entry_count{"huesat_entry_count"};
    Input<int32_t> huesat_hue_div{"huesat_hue_div"};
    Input<int32_t> huesat_sat_div{"huesat_sat_div"};
    Input<int32_t> huesat_val_div{"huesat_val_div"};
    Input<int32_t> huesat_has_table{"huesat_has_table"};
    Input<int32_t> huesat_has_encoding{"huesat_has_encoding"};
    Input<Buffer<float>> look_table{"look_table", 1};
    Input<Buffer<float>> look_encode{"look_encode", 1};
    Input<Buffer<float>> look_decode{"look_decode", 1};
    Input<int32_t> look_entry_count{"look_entry_count"};
    Input<int32_t> look_hue_div{"look_hue_div"};
    Input<int32_t> look_sat_div{"look_sat_div"};
    Input<int32_t> look_val_div{"look_val_div"};
    Input<int32_t> look_has_table{"look_has_table"};
    Input<int32_t> look_has_encoding{"look_has_encoding"};

    // G2: interleaved RGBA8 output dst(x, y, c) — dim0 stride 4, dim2 stride 1
    // bounds [0,4), alpha=255 in-kernel. Exact macOS production dst layout;
    // construct verified 0-error on Vulkan by G2 pre-check Probe A. Replaces
    // the W4-3 (b) 2D planar dst + host MT repack (repackPlanarToRGBAMT /
    // repackPlanarToInterleavedMT + RepackThreadPool, all retired).
    //
    // Historical dead ends kept for the record:
    // - Plan (a), W4-1: single interleaved 1D dst_rgb(j), j=i*3+c — mis-lowered
    //   on Vulkan (~8 dB border/coverage corruption; Gotcha #93).
    // - W4-3 (b): 2D planar dst(i, c) — correct but needed a full-frame host
    //   repack pass per decode (~8-12 ms MT) plus a 3-plane D2H.
    Output<Buffer<uint8_t>> dst{"dst", 3};  // x, y, c

    // Per-channel 8-bit results, set by emit_rgb8 (possibly from a diag stage),
    // consumed once at the end of generate() to build dst.
    Expr out8_r, out8_g, out8_b;

    void generate() {
        Var x("x"), y("y");

        // G2: interleaved RGBA8 dst layout (macOS layout; probe-A verified).
        dst.dim(0).set_stride(4);
        dst.dim(2).set_bounds(0, 4);
        dst.dim(2).set_stride(1);

        Expr sx = clamp(x + crop_l, 0, src_width - 1);
        Expr sy = clamp(y + crop_t, 0, src_height - 1);

        // W2: input-side interleaved gather (replaces three planar reads).
        Expr base = (sy * src_row_stride_px + sx) * 3;
        Expr max_idx = src_rgb.dim(0).extent() - 1;
        Expr s_r = cast<float>(src_rgb(clamp(base + 0, 0, max_idx))) * src_scale;
        Expr s_g = cast<float>(src_rgb(clamp(base + 1, 0, max_idx))) * src_scale;
        Expr s_b = cast<float>(src_rgb(clamp(base + 2, 0, max_idx))) * src_scale;

        auto linear8 = [&](Expr v) {
            return cast<uint8_t>(clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
        };

        // Stash the three channel results; finalize() wires them into dst_rgb.
        auto emit_rgb8 = [&](Expr r8, Expr g8, Expr b8) {
            out8_r = r8;
            out8_g = g8;
            out8_b = b8;
        };

        if (diag_stage == 0) {
            emit_rgb8(linear8(s_r), linear8(s_g), linear8(s_b));
            finalize(x, y);
            return;
        }

        Expr wb_r = min(s_r, camera_white(0));
        Expr wb_g = min(s_g, camera_white(1));
        Expr wb_b = min(s_b, camera_white(2));

        if (diag_stage == 1) {
            emit_rgb8(linear8(wb_r), linear8(wb_g), linear8(wb_b));
            finalize(x, y);
            return;
        }

        auto matrix3 = [&](const auto& matrix, int col, int row) {
            return matrix(row * 3 + col);
        };

        Expr p_r0_rg = wb_r * matrix3(camera_to_rgb, 0, 0) + wb_g * matrix3(camera_to_rgb, 1, 0);
        Expr p_g0_rg = wb_r * matrix3(camera_to_rgb, 0, 1) + wb_g * matrix3(camera_to_rgb, 1, 1);
        Expr p_b0_rg = wb_r * matrix3(camera_to_rgb, 0, 2) + wb_g * matrix3(camera_to_rgb, 1, 2);
        Expr p_r0_sum = p_r0_rg + wb_b * matrix3(camera_to_rgb, 2, 0);
        Expr p_g0_sum = p_g0_rg + wb_b * matrix3(camera_to_rgb, 2, 1);
        Expr p_b0_sum = p_b0_rg + wb_b * matrix3(camera_to_rgb, 2, 2);
        Expr p_r0 = clamp(p_r0_sum, 0.0f, 1.0f);
        Expr p_g0 = clamp(p_g0_sum, 0.0f, 1.0f);
        Expr p_b0 = clamp(p_b0_sum, 0.0f, 1.0f);

        if (diag_stage == 2) {
            emit_rgb8(linear8(p_r0), linear8(p_g0), linear8(p_b0));
            finalize(x, y);
            return;
        }

        Expr abc_r = p_r0;
        Expr abc_g = p_g0;
        Expr abc_b = p_b0;

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

        auto rgb_to_hsv = [&](Expr r, Expr g, Expr b, Expr& h, Expr& s, Expr& v) {
            v = max(r, max(g, b));
            Expr mn = min(r, min(g, b));
            Expr gap = v - mn;
            Expr gap_den = select(gap > 0.0f, gap, 1.0f);
            Expr h_r = (g - b) / gap_den;
            Expr h_r_fix = select(h_r < 0.0f, h_r + 6.0f, h_r);
            Expr h_g = 2.0f + (b - r) / gap_den;
            Expr h_b = 4.0f + (r - g) / gap_den;
            h = select(gap > 0.0f,
                       select(r == v, h_r_fix,
                              g == v, h_g,
                                      h_b),
                       0.0f);
            s = select(gap > 0.0f, gap / v, 0.0f);
        };

        auto hsv_to_rgb = [&](Expr h, Expr s, Expr v, Expr& r, Expr& g, Expr& b) {
            Expr use_sat = s > 0.0f;
            Expr hh = select(h < 0.0f, h + 6.0f, h);
            hh = select(hh >= 6.0f, hh - 6.0f, hh);
            Expr i = cast<int>(hh);
            Expr f = hh - cast<float>(i);
            Expr p = v * (1.0f - s);
            Expr q = v * (1.0f - s * f);
            Expr t = v * (1.0f - s * (1.0f - f));
            Expr cc = clamp(i, 0, 5);
            Expr r_hsv = select(cc == 0, v, cc == 1, q, cc == 2, p, cc == 3, p, cc == 4, t, v);
            Expr g_hsv = select(cc == 0, t, cc == 1, v, cc == 2, v, cc == 3, q, cc == 4, p, p);
            Expr b_hsv = select(cc == 0, p, cc == 1, p, cc == 2, t, cc == 3, v, cc == 4, v, q);
            r = select(use_sat, r_hsv, v);
            g = select(use_sat, g_hsv, v);
            b = select(use_sat, b_hsv, v);
        };

        auto sample_hsv_map = [&](const auto& table,
                                  const auto& encode_table,
                                  const auto& decode_table,
                                  Expr entry_count,
                                  Expr hue_div, Expr sat_div, Expr val_div,
                                  Expr has_table, Expr has_encoding,
                                  Expr r, Expr g, Expr b,
                                  Expr& out_r, Expr& out_g, Expr& out_b) {
            Expr h, s, v;
            rgb_to_hsv(r, g, b, h, s, v);
            Expr hue_div_safe = max(hue_div, 2);
            Expr sat_div_safe = max(sat_div, 2);
            Expr val_div_safe = max(val_div, 1);
            Expr hue_scale = cast<float>(hue_div_safe) * (1.0f / 6.0f);
            Expr sat_scale = cast<float>(sat_div_safe - 1);
            Expr val_scale = cast<float>(val_div_safe - 1);
            Expr max_hue_index0 = hue_div_safe - 1;
            Expr max_sat_index0 = sat_div_safe - 2;
            Expr max_val_index0 = val_div_safe - 2;
            Expr hue_step = sat_div_safe;
            Expr val_step = hue_div_safe * sat_div_safe;
            Expr v_encoded0 = v;
            Expr use_encode = (has_encoding != 0) && (val_div_safe >= 2);
            Expr v_encoded = select(use_encode, table_interp(encode_table, clamp(v, 0.0f, 1.0f)), v_encoded0);
            Expr h_scaled = h * hue_scale;
            Expr s_scaled = s * sat_scale;
            Expr v_scaled = v_encoded * val_scale;
            Expr h_index0_raw = cast<int>(floor(h_scaled));
            Expr s_index0 = clamp(cast<int>(floor(s_scaled)), 0, cast<int>(max_sat_index0));
            Expr v_index0 = clamp(cast<int>(floor(v_scaled)), 0, cast<int>(max_val_index0));
            Expr h_index0 = clamp(h_index0_raw, 0, cast<int>(max_hue_index0));
            Expr h_index1 = select(h_index0_raw >= max_hue_index0, 0, h_index0 + 1);
            Expr h_fract1 = h_scaled - cast<float>(h_index0);
            Expr s_fract1 = s_scaled - cast<float>(s_index0);
            Expr v_fract1 = v_scaled - cast<float>(v_index0);
            Expr h_fract0 = 1.0f - h_fract1;
            Expr s_fract0 = 1.0f - s_fract1;
            Expr v_fract0 = 1.0f - v_fract1;
            auto tval = [&](Expr idx, int comp) {
                Expr entry = clamp(cast<int>(idx), 0, entry_count - 1);
                return table(comp * entry_count + entry);
            };
            Expr base3d00 = v_index0 * val_step + h_index0 * hue_step + s_index0;
            Expr base3d01 = v_index0 * val_step + h_index1 * hue_step + s_index0;
            Expr base3d10 = base3d00 + val_step;
            Expr base3d11 = base3d01 + val_step;
            auto lerp_hv = [&](int comp, Expr off) {
                return v_fract0 * (h_fract0 * tval(base3d00 + off, comp) + h_fract1 * tval(base3d01 + off, comp)) +
                       v_fract1 * (h_fract0 * tval(base3d10 + off, comp) + h_fract1 * tval(base3d11 + off, comp));
            };
            Expr hue_shift = s_fract0 * lerp_hv(0, 0) + s_fract1 * lerp_hv(0, 1);
            Expr sat_mult = s_fract0 * lerp_hv(1, 0) + s_fract1 * lerp_hv(1, 1);
            Expr val_mult = s_fract0 * lerp_hv(2, 0) + s_fract1 * lerp_hv(2, 1);
            Expr hh = h + hue_shift * (6.0f / 360.0f);
            Expr ss = min(s * sat_mult, 1.0f);
            Expr ve = clamp(v_encoded * val_mult, 0.0f, 1.0f);
            Expr vv = select(use_encode, table_interp(decode_table, ve), ve);
            Expr rr, gg, bb;
            hsv_to_rgb(hh, ss, vv, rr, gg, bb);
            out_r = rr;
            out_g = gg;
            out_b = bb;
        };

        Expr p_r1, p_g1, p_b1;
        sample_hsv_map(huesat_table, huesat_encode, huesat_decode,
                       huesat_entry_count,
                       huesat_hue_div, huesat_sat_div, huesat_val_div,
                       huesat_has_table, huesat_has_encoding,
                       abc_r, abc_g, abc_b, p_r1, p_g1, p_b1);

        if (diag_stage == 3) {
            emit_rgb8(linear8(p_r1), linear8(p_g1), linear8(p_b1));
            finalize(x, y);
            return;
        }

        Expr e_r = table_interp(exp_ramp, p_r1);
        Expr e_g = table_interp(exp_ramp, p_g1);
        Expr e_b = table_interp(exp_ramp, p_b1);

        if (diag_stage == 4) {
            emit_rgb8(linear8(e_r), linear8(e_g), linear8(e_b));
            finalize(x, y);
            return;
        }

        Expr p_r2, p_g2, p_b2;
        sample_hsv_map(look_table, look_encode, look_decode,
                       look_entry_count,
                       look_hue_div, look_sat_div, look_val_div,
                       look_has_table, look_has_encoding,
                       e_r, e_g, e_b, p_r2, p_g2, p_b2);

        if (diag_stage == 5) {
            emit_rgb8(linear8(p_r2), linear8(p_g2), linear8(p_b2));
            finalize(x, y);
            return;
        }

        auto rgb_tone = [&](Expr r, Expr g, Expr b, Expr& rr, Expr& gg, Expr& bb) {
            Expr tr = table_interp(tone_curve, r);
            Expr tg = table_interp(tone_curve, g);
            Expr tb = table_interp(tone_curve, b);
            Expr rr1 = tr;
            Expr den1 = select((r >= g) && (g > b), r - b, 1.0f);
            Expr gg1 = tb + ((tr - tb) * (g - b) / den1);
            Expr bb1 = tb;
            Expr bb2 = tb;
            Expr gg2 = tg;
            Expr den2 = select((r >= g) && !(g > b) && (b > r), b - g, 1.0f);
            Expr rr2 = gg2 + ((bb2 - gg2) * (r - g) / den2);
            Expr rr3 = tr;
            Expr gg3 = tg;
            Expr den3 = select((r >= g) && !(g > b) && !(b > r) && (b > g), r - g, 1.0f);
            Expr bb3 = gg3 + ((rr3 - gg3) * (b - g) / den3);
            Expr rr4 = tr;
            Expr gg4 = tg;
            Expr bb4 = tg;
            Expr gg5 = tg;
            Expr bb5 = tb;
            Expr den5 = select(!(r >= g) && (r >= b), g - b, 1.0f);
            Expr rr5 = bb5 + ((gg5 - bb5) * (r - b) / den5);
            Expr bb6 = tb;
            Expr rr6 = tr;
            Expr den6 = select(!(r >= g) && !(r >= b) && (b > g), b - r, 1.0f);
            Expr gg6 = rr6 + ((bb6 - rr6) * (g - r) / den6);
            Expr gg7 = tg;
            Expr rr7 = tr;
            Expr den7 = select(!(r >= g) && !(r >= b) && !(b > g), g - r, 1.0f);
            Expr bb7 = rr7 + ((gg7 - rr7) * (b - r) / den7);
            Expr c1 = (r >= g) && (g > b);
            Expr c2 = (r >= g) && !(g > b) && (b > r);
            Expr c3 = (r >= g) && !(g > b) && !(b > r) && (b > g);
            Expr c4 = (r >= g) && !(g > b) && !(b > r) && !(b > g);
            Expr c5 = !(r >= g) && (r >= b);
            Expr c6 = !(r >= g) && !(r >= b) && (b > g);
            rr = select(c1, rr1, c2, rr2, c3, rr3, c4, rr4, c5, rr5, c6, rr6, rr7);
            gg = select(c1, gg1, c2, gg2, c3, gg3, c4, gg4, c5, gg5, c6, gg6, gg7);
            bb = select(c1, bb1, c2, bb2, c3, bb3, c4, bb4, c5, bb5, c6, bb6, bb7);
        };

        Expr t_r, t_g, t_b;
        rgb_tone(p_r2, p_g2, p_b2, t_r, t_g, t_b);

        if (diag_stage == 6) {
            emit_rgb8(linear8(t_r), linear8(t_g), linear8(t_b));
            finalize(x, y);
            return;
        }

        Expr tone_r = t_r;
        Expr tone_g = t_g;
        Expr tone_b = t_b;
        Expr f_r_rg = tone_r * matrix3(rgb_to_final, 0, 0) + tone_g * matrix3(rgb_to_final, 1, 0);
        Expr f_g_rg = tone_r * matrix3(rgb_to_final, 0, 1) + tone_g * matrix3(rgb_to_final, 1, 1);
        Expr f_b_rg = tone_r * matrix3(rgb_to_final, 0, 2) + tone_g * matrix3(rgb_to_final, 1, 2);
        Expr f_r_sum = f_r_rg + tone_b * matrix3(rgb_to_final, 2, 0);
        Expr f_g_sum = f_g_rg + tone_b * matrix3(rgb_to_final, 2, 1);
        Expr f_b_sum = f_b_rg + tone_b * matrix3(rgb_to_final, 2, 2);

        Expr f_r = clamp(f_r_sum, 0.0f, 1.0f);
        Expr f_g = clamp(f_g_sum, 0.0f, 1.0f);
        Expr f_b = clamp(f_b_sum, 0.0f, 1.0f);

        if (diag_stage == 7) {
            emit_rgb8(linear8(f_r), linear8(f_g), linear8(f_b));
            finalize(x, y);
            return;
        }

        auto encode8 = [&](Expr v) {
            Expr g = table_interp(encode_gamma, v);
            return clamp(g * 255.0f + 0.5f, 0.0f, 255.0f);
        };

        emit_rgb8(cast<uint8_t>(encode8(f_r)),
                  cast<uint8_t>(encode8(f_g)),
                  cast<uint8_t>(encode8(f_b)));
        finalize(x, y);
    }

    // G2: wire the three channel results into the interleaved RGBA8 output.
    // out8_r/g/b are Exprs over (x, y); dst(x, y, c) selects a channel by c
    // (alpha hard 255). reorder(c,x,y) + unroll(c) folds c to a constant per
    // copy so select() prunes to one channel while the pipeline body
    // (independent of c) is CSE-shared — same compute-1x property the planar
    // kernel had, now with coalesced 4-byte interleaved stores.
    void finalize(Var x, Var y) {
        Var c("c");
        dst(x, y, c) = select(c == 0, out8_r,
                              c == 1, out8_g,
                              c == 2, out8_b,
                                      cast<uint8_t>(255));
    }

    void schedule() {
        Var x("x"), y("y"), c("c");
        // G2: exact Probe-A schedule (the verified-safe configuration). Fully
        // inlined — NO compute_at producer (see class comment; Probe B fails).
        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            dst.bound(c, 0, 4)
               .reorder(c, x, y)
               .gpu_tile(x, y, xo, yo, xi, yi, 16, 16)
               .unroll(c);
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

// =============================================================================
// P14-W4-4 GO/NO-GO PROBE — isolated, reversible experiment.
//
// Sole purpose: decide whether an INPUT-side interleaved flat-1D gather
// `src_rgb(base + c)` (base = (sy*row_stride + sx)*3) lowers correctly on
// Vulkan, i.e. whether the Stage4 generator can read the Stage3 interleaved
// device buffer directly instead of the current host planar repack.
//
// This is a SEPARATE generator (separate AOT, separate signature) so it does
// NOT touch the production three-planar DngRenderStage4Android at all — maximal
// reversibility. It only does a linear8 passthrough (diag_stage 0 equivalent)
// through the SAME verified 2D-planar dst(i,c) output, isolating the src-read
// construct as the single variable under test.
//
// Gotcha context: #92 = interleaved 3D-buffer channel-stride aliasing (R==G);
// #93 = OUTPUT-side `idx*3+c` mis-lowers under split+gpu_tile. The INPUT-side
// `idx*3+c` gather is the untested construct this probe settles.
// =============================================================================
class DngRenderStage4AndroidProbe
    : public Halide::Generator<DngRenderStage4AndroidProbe> {
public:
    // Single flat interleaved src: contents = SDK interleaved RGB buffer
    // (row-major, channel stride 1) laid out as one 1D plane of size
    // src_row_stride_px * src_height * 3.
    Input<Buffer<uint16_t>> src_rgb{"src_rgb", 1};
    Input<int32_t> src_width{"src_width"};
    Input<int32_t> src_height{"src_height"};
    Input<int32_t> dst_width{"dst_width"};
    Input<int32_t> src_row_stride_px{"src_row_stride_px"};
    Input<int32_t> crop_l{"crop_l"};
    Input<int32_t> crop_t{"crop_t"};
    Input<float> src_scale{"src_scale"};

    // Same verified 2D-planar output as W4-3 (b): dim0=i (stride1), dim1=c
    // (stride N). The dst construct is held constant vs the production kernel so
    // the only variable under test is the interleaved src read.
    Output<Buffer<uint8_t>> dst{"dst", 2};

    Expr out8_r, out8_g, out8_b;

    void generate() {
        Var i("i");

        dst.dim(0).set_stride(1);
        dst.dim(1).set_bounds(0, 3);
        dst.dim(1).set_stride(dst.dim(0).extent());

        Expr dst_x = i % dst_width;
        Expr dst_y = i / dst_width;
        Expr sx = clamp(dst_x + crop_l, 0, src_width - 1);
        Expr sy = clamp(dst_y + crop_t, 0, src_height - 1);

        // INPUT-side interleaved gather under test:
        Expr base = (sy * src_row_stride_px + sx) * 3;
        Expr max_idx = src_rgb.dim(0).extent() - 1;
        Expr s_r = cast<float>(src_rgb(clamp(base + 0, 0, max_idx))) * src_scale;
        Expr s_g = cast<float>(src_rgb(clamp(base + 1, 0, max_idx))) * src_scale;
        Expr s_b = cast<float>(src_rgb(clamp(base + 2, 0, max_idx))) * src_scale;

        auto linear8 = [&](Expr v) {
            return cast<uint8_t>(clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
        };

        out8_r = linear8(s_r);
        out8_g = linear8(s_g);
        out8_b = linear8(s_b);

        Var c("c");
        dst(i, c) = select(c == 0, out8_r, c == 1, out8_g, out8_b);
    }

    void schedule() {
        Var i("i"), c("c");
        if (get_target().has_gpu_feature()) {
            Var io("io"), ii("ii");
            dst.bound(c, 0, 3)
               .reorder(c, i)
               .gpu_tile(i, io, ii, 256)
               .unroll(c);
        } else {
            Var io("io"), ii("ii");
            dst.bound(c, 0, 3)
               .reorder(c, i)
               .split(i, io, ii, 1024)
               .parallel(io)
               .vectorize(ii, 8)
               .unroll(c);
        }
    }
};

// =============================================================================
// Sized (box-filter downscaling) Stage4 generator — macOS/Metal round 1.
//
// Purpose: `targetWidth` / sized decode (docs/logs/2026-08-23/
// targetwidth-sized-decode-handover.md §4 row 6, §5.2). The production
// `dng_render_stage4` AOT must stay bit-identical because its output SHAs are
// pinned gate artifacts (Gotcha #99), so this is a *separate* generator +
// separate AOT rather than a scale path bolted into the existing kernel.
//
// Semantics: identical to DngRenderStage4 (same inputs, same RGBA8 dst layout,
// same render math) except that the rendered result is box-averaged down to
// the requested output size. The averaging happens AFTER the full colour
// math, in float, and BEFORE the single uint8 quantisation — so the kernel
// converges on "box downscale of the full-resolution 8-bit output", which is
// the acceptance reference (contract AC7). Averaging on the source side
// instead would NOT converge on that reference, because the tone curve and
// encode gamma between the two points are non-linear.
//
// Consequence to be aware of: the colour math still evaluates at sensor
// resolution, so this variant shrinks the output buffer but does not shrink
// the render workload.
//
// Cell convention: output pixel x covers source columns
// [x*src_w/dst_width, (x+1)*src_w/dst_width) — an exact integer-ratio box, so
// the cells tile the source exactly and neighbouring cells differ in size by
// at most one pixel. The RDom is sized to the worst-case cell (its bounds
// depend only on the inputs, never on x/y, as Halide requires) and the surplus
// taps are masked out by `in_cell`; the divisor is the true cell area.
//
// The accumulation is a single Tuple-valued update rather than three separate
// sum() reductions, so CSE shares the (expensive, channel-independent) render
// pipeline body across R/G/B instead of evaluating it three times per source
// pixel — the same reasoning as the `unroll(c) + select` note above.
//
// NOTE (round 1 scope): macOS/Metal only. The Vulkan branches below are
// carried over verbatim from DngRenderStage4 so the class stays a faithful
// base, but this generator is NOT wired into the Android AOT and has not been
// checked against Gotcha #93 / #96 on a real Adreno device. Do not claim
// Android coverage from a macOS green.
// =============================================================================
class DngRenderStage4Scaled : public Halide::Generator<DngRenderStage4Scaled> {
public:
    Input<Buffer<uint16_t>> src{"src", 3};          // x, y, c (Stage3 output)
    Input<float> src_scale{"src_scale"};            // usually 1 / 65535
    // Requested output size. Must match the extents of `dst`; passed
    // explicitly so the box geometry never depends on output bounds
    // inference.
    Input<int32_t> dst_width{"dst_width"};
    Input<int32_t> dst_height{"dst_height"};
    Input<Buffer<float>> exp_ramp{"exp_ramp", 1};   // 4098
    Input<Buffer<float>> tone_curve{"tone_curve", 1}; // 4098
    Input<Buffer<float>> encode_gamma{"encode_gamma", 1}; // 4098
    Input<Buffer<float>> camera_white{"camera_white", 1}; // 3
    Input<Buffer<float>> camera_to_rgb{"camera_to_rgb", 2}; // [col, row] 3x3
    Input<Buffer<float>> rgb_to_final{"rgb_to_final", 2};   // [col, row] 3x3
    Input<Buffer<float>> huesat_table{"huesat_table", 2};   // [entry, component(0..2)]
    Input<Buffer<float>> huesat_encode{"huesat_encode", 1}; // 4098
    Input<Buffer<float>> huesat_decode{"huesat_decode", 1}; // 4098
    Input<int32_t> huesat_hue_div{"huesat_hue_div"};
    Input<int32_t> huesat_sat_div{"huesat_sat_div"};
    Input<int32_t> huesat_val_div{"huesat_val_div"};
    Input<int32_t> huesat_has_table{"huesat_has_table"};
    Input<int32_t> huesat_has_encoding{"huesat_has_encoding"};
    Input<Buffer<float>> look_table{"look_table", 2};       // [entry, component(0..2)]
    Input<Buffer<float>> look_encode{"look_encode", 1};     // 4098
    Input<Buffer<float>> look_decode{"look_decode", 1};     // 4098
    Input<int32_t> look_hue_div{"look_hue_div"};
    Input<int32_t> look_sat_div{"look_sat_div"};
    Input<int32_t> look_val_div{"look_val_div"};
    Input<int32_t> look_has_table{"look_has_table"};
    Input<int32_t> look_has_encoding{"look_has_encoding"};
    Output<Buffer<uint8_t>> dst{"dst", 3};          // x, y, c
    Func rendered_rgb{"rendered_rgb"};   // full-res, float, pre-quantisation
    Func box_acc{"box_acc"};             // per-output-pixel cell accumulator

    void generate() {
        Var x("x"), y("y"), c("c");

        if (uses_vulkan_planar_layout(get_target())) {
            src.dim(0).set_stride(1);
            src.dim(1).set_stride(src.dim(0).extent());
            src.dim(2).set_bounds(0, 3);
            src.dim(2).set_stride(src.dim(0).extent() * src.dim(1).extent());
        } else {
            src.dim(0).set_stride(3);
            src.dim(2).set_bounds(0, 3);
            src.dim(2).set_stride(1);
        }
        if (uses_vulkan_planar_layout(get_target())) {
            dst.dim(0).set_stride(1);
            dst.dim(1).set_stride(dst.dim(0).extent());
            dst.dim(2).set_bounds(0, 3);
            dst.dim(2).set_stride(dst.dim(0).extent() * dst.dim(1).extent());
        } else {
            dst.dim(0).set_stride(4);
            dst.dim(2).set_bounds(0, 4);
            dst.dim(2).set_stride(1);
        }

        Func src_f("src_f");
        src_f(x, y, c) = src(x, y, c);

        // Source reads are at FULL resolution and identical to
        // DngRenderStage4 — the downscale happens after the colour math.
        Expr sx = clamp(x, 0, src.dim(0).extent() - 1);
        Expr sy = clamp(y, 0, src.dim(1).extent() - 1);
        Expr s_r = cast<float>(src_f(sx, sy, 0)) * src_scale;
        Expr s_g = cast<float>(src_f(sx, sy, 1)) * src_scale;
        Expr s_b = cast<float>(src_f(sx, sy, 2)) * src_scale;

        Expr wb_r = min(s_r, camera_white(0));
        Expr wb_g = min(s_g, camera_white(1));
        Expr wb_b = min(s_b, camera_white(2));

        Expr p_r0_rg = wb_r * camera_to_rgb(0, 0) + wb_g * camera_to_rgb(1, 0);
        Expr p_g0_rg = wb_r * camera_to_rgb(0, 1) + wb_g * camera_to_rgb(1, 1);
        Expr p_b0_rg = wb_r * camera_to_rgb(0, 2) + wb_g * camera_to_rgb(1, 2);
        Expr p_r0_sum = p_r0_rg + wb_b * camera_to_rgb(2, 0);
        Expr p_g0_sum = p_g0_rg + wb_b * camera_to_rgb(2, 1);
        Expr p_b0_sum = p_b0_rg + wb_b * camera_to_rgb(2, 2);
        Expr p_r0 = clamp(p_r0_sum, 0.0f, 1.0f);
        Expr p_g0 = clamp(p_g0_sum, 0.0f, 1.0f);
        Expr p_b0 = clamp(p_b0_sum, 0.0f, 1.0f);

        Expr abc_r = p_r0;
        Expr abc_g = p_g0;
        Expr abc_b = p_b0;

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

        auto rgb_to_hsv = [&](Expr r_, Expr g_, Expr b_, Expr& h, Expr& s, Expr& v) {
            v = max(r_, max(g_, b_));
            Expr mn = min(r_, min(g_, b_));
            Expr gap = v - mn;

            Expr gap_den = select(gap > 0.0f, gap, 1.0f);
            Expr h_r = (g_ - b_) / gap_den;
            Expr h_r_fix = select(h_r < 0.0f, h_r + 6.0f, h_r);
            Expr h_g = 2.0f + (b_ - r_) / gap_den;
            Expr h_b = 4.0f + (r_ - g_) / gap_den;

            h = select(gap > 0.0f,
                       select(r_ == v, h_r_fix,
                              g_ == v, h_g,
                                       h_b),
                       0.0f);
            s = select(gap > 0.0f, gap / v, 0.0f);
        };

        auto hsv_to_rgb = [&](Expr h, Expr s, Expr v, Expr& r_, Expr& g_, Expr& b_) {
            Expr use_sat = s > 0.0f;
            Expr hh = select(h < 0.0f, h + 6.0f, h);
            hh = select(hh >= 6.0f, hh - 6.0f, hh);
            Expr i = cast<int>(hh);
            Expr f = hh - cast<float>(i);
            Expr p = v * (1.0f - s);
            Expr q = v * (1.0f - s * f);
            Expr t = v * (1.0f - s * (1.0f - f));
            Expr cc = clamp(i, 0, 5);

            Expr r_hsv = select(cc == 0, v,
                                cc == 1, q,
                                cc == 2, p,
                                cc == 3, p,
                                cc == 4, t,
                                         v);
            Expr g_hsv = select(cc == 0, t,
                                cc == 1, v,
                                cc == 2, v,
                                cc == 3, q,
                                cc == 4, p,
                                         p);
            Expr b_hsv = select(cc == 0, p,
                                cc == 1, p,
                                cc == 2, t,
                                cc == 3, v,
                                cc == 4, v,
                                         q);

            // Match DNG SDK semantics: if saturation is non-positive, emit gray.
            r_ = select(use_sat, r_hsv, v);
            g_ = select(use_sat, g_hsv, v);
            b_ = select(use_sat, b_hsv, v);
        };

        auto sample_hsv_map = [&](const auto& table,
                                  const auto& encode_table,
                                  const auto& decode_table,
                                  Expr hue_div,
                                  Expr sat_div,
                                  Expr val_div,
                                  Expr has_table,
                                  Expr has_encoding,
                                  Expr r_,
                                  Expr g_,
                                  Expr b_,
                                  Expr& out_r,
                                  Expr& out_g,
                                  Expr& out_b) {
            Expr h, s, v;
            rgb_to_hsv(r_, g_, b_, h, s, v);

            // select() is not short-circuit in Halide; always keep dimensions valid.
            Expr hue_div_safe = max(hue_div, 2);
            Expr sat_div_safe = max(sat_div, 2);
            Expr val_div_safe = max(val_div, 1);

            Expr hue_scale = cast<float>(hue_div_safe) * (1.0f / 6.0f);
            Expr sat_scale = cast<float>(sat_div_safe - 1);
            Expr val_scale = cast<float>(val_div_safe - 1);
            Expr max_hue_index0 = hue_div_safe - 1;
            Expr max_sat_index0 = sat_div_safe - 2;
            Expr max_val_index0 = val_div_safe - 2;
            Expr hue_step = sat_div_safe;
            Expr val_step = hue_div_safe * sat_div_safe;

            Expr v_encoded0 = v;
            Expr use_encode = (has_encoding != 0) && (val_div_safe >= 2);
            Expr v_encoded = select(use_encode, table_interp(encode_table, clamp(v, 0.0f, 1.0f)), v_encoded0);

            Expr h_scaled = h * hue_scale;
            Expr s_scaled = s * sat_scale;
            Expr v_scaled = v_encoded * val_scale;

            Expr h_index0_raw = cast<int>(floor(h_scaled));
            Expr s_index0 = clamp(cast<int>(floor(s_scaled)), 0, cast<int>(max_sat_index0));
            Expr v_index0 = clamp(cast<int>(floor(v_scaled)), 0, cast<int>(max_val_index0));

            Expr h_index0 = clamp(h_index0_raw, 0, cast<int>(max_hue_index0));
            Expr h_index1 = select(h_index0_raw >= max_hue_index0, 0, h_index0 + 1);

            Expr h_fract1 = h_scaled - cast<float>(h_index0);
            Expr s_fract1 = s_scaled - cast<float>(s_index0);
            Expr v_fract1 = v_scaled - cast<float>(v_index0);
            Expr h_fract0 = 1.0f - h_fract1;
            Expr s_fract0 = 1.0f - s_fract1;
            Expr v_fract0 = 1.0f - v_fract1;

            auto tval = [&](Expr idx, int comp) {
                return table(clamp(cast<int>(idx), 0, table.dim(0).extent() - 1), comp);
            };

            Expr base2d0 = h_index0 * hue_step + s_index0;
            Expr base2d1 = h_index1 * hue_step + s_index0;

            Expr hs_hue0 = h_fract0 * tval(base2d0, 0) + h_fract1 * tval(base2d1, 0);
            Expr hs_sat0 = h_fract0 * tval(base2d0, 1) + h_fract1 * tval(base2d1, 1);
            Expr hs_val0 = h_fract0 * tval(base2d0, 2) + h_fract1 * tval(base2d1, 2);

            Expr hs_hue1 = h_fract0 * tval(base2d0 + 1, 0) + h_fract1 * tval(base2d1 + 1, 0);
            Expr hs_sat1 = h_fract0 * tval(base2d0 + 1, 1) + h_fract1 * tval(base2d1 + 1, 1);
            Expr hs_val1 = h_fract0 * tval(base2d0 + 1, 2) + h_fract1 * tval(base2d1 + 1, 2);

            Expr hue_shift_2d = s_fract0 * hs_hue0 + s_fract1 * hs_hue1;
            Expr sat_scale_2d = s_fract0 * hs_sat0 + s_fract1 * hs_sat1;
            Expr val_scale_2d = s_fract0 * hs_val0 + s_fract1 * hs_val1;

            Expr base3d00 = v_index0 * val_step + h_index0 * hue_step + s_index0;
            Expr base3d01 = v_index0 * val_step + h_index1 * hue_step + s_index0;
            Expr base3d10 = base3d00 + val_step;
            Expr base3d11 = base3d01 + val_step;

            auto lerp_hv = [&](int comp, Expr off) {
                return v_fract0 * (h_fract0 * tval(base3d00 + off, comp) + h_fract1 * tval(base3d01 + off, comp)) +
                       v_fract1 * (h_fract0 * tval(base3d10 + off, comp) + h_fract1 * tval(base3d11 + off, comp));
            };

            Expr hue_shift0_3d = lerp_hv(0, 0);
            Expr sat_scale0_3d = lerp_hv(1, 0);
            Expr val_scale0_3d = lerp_hv(2, 0);
            Expr hue_shift1_3d = lerp_hv(0, 1);
            Expr sat_scale1_3d = lerp_hv(1, 1);
            Expr val_scale1_3d = lerp_hv(2, 1);

            Expr hue_shift_3d = s_fract0 * hue_shift0_3d + s_fract1 * hue_shift1_3d;
            Expr sat_scale_3d = s_fract0 * sat_scale0_3d + s_fract1 * sat_scale1_3d;
            Expr val_scale_3d = s_fract0 * val_scale0_3d + s_fract1 * val_scale1_3d;

            Expr use_2d = val_div_safe < 2;
            Expr hue_shift = hue_shift_3d;
            Expr sat_mult = sat_scale_3d;
            Expr val_mult = val_scale_3d;

            Expr hh = h + hue_shift * (6.0f / 360.0f);
            Expr ss = min(s * sat_mult, 1.0f);
            Expr ve = clamp(v_encoded * val_mult, 0.0f, 1.0f);
            Expr vv = select(use_encode, table_interp(decode_table, ve), ve);

            Expr rr, gg, bb;
            hsv_to_rgb(hh, ss, vv, rr, gg, bb);

            out_r = select(has_table != 0, rr, r_);
            out_g = select(has_table != 0, gg, g_);
            out_b = select(has_table != 0, bb, b_);
        };

        Expr p_r1, p_g1, p_b1;
        sample_hsv_map(huesat_table,
                       huesat_encode,
                       huesat_decode,
                       huesat_hue_div,
                       huesat_sat_div,
                       huesat_val_div,
                       huesat_has_table,
                       huesat_has_encoding,
                       abc_r,
                       abc_g,
                       abc_b,
                       p_r1,
                       p_g1,
                       p_b1);

        Expr e_r = table_interp(exp_ramp, p_r1);
        Expr e_g = table_interp(exp_ramp, p_g1);
        Expr e_b = table_interp(exp_ramp, p_b1);

        Expr p_r2, p_g2, p_b2;
        sample_hsv_map(look_table,
                       look_encode,
                       look_decode,
                       look_hue_div,
                       look_sat_div,
                       look_val_div,
                       look_has_table,
                       look_has_encoding,
                       e_r,
                       e_g,
                       e_b,
                       p_r2,
                       p_g2,
                       p_b2);

        auto rgb_tone = [&](Expr r_, Expr g_, Expr b_, Expr& rr, Expr& gg, Expr& bb) {
            Expr tr = table_interp(tone_curve, r_);
            Expr tg = table_interp(tone_curve, g_);
            Expr tb = table_interp(tone_curve, b_);

            Expr rr1 = tr;
            Expr den1 = select((r_ >= g_) && (g_ > b_), r_ - b_, 1.0f);
            Expr gg1 = tb + ((tr - tb) * (g_ - b_) / den1);
            Expr bb1 = tb;

            // Case 2: b > r >= g (RGBTone(b, r, g, bb, rr, gg))
            Expr bb2 = tb;
            Expr gg2 = tg;
            Expr den2 = select((r_ >= g_) && !(g_ > b_) && (b_ > r_), b_ - g_, 1.0f);
            Expr rr2 = gg2 + ((bb2 - gg2) * (r_ - g_) / den2);

            // Case 3: r >= b > g (RGBTone(r, b, g, rr, bb, gg))
            Expr rr3 = tr;
            Expr gg3 = tg;
            Expr den3 = select((r_ >= g_) && !(g_ > b_) && !(b_ > r_) && (b_ > g_), r_ - g_, 1.0f);
            Expr bb3 = gg3 + ((rr3 - gg3) * (b_ - g_) / den3);

            Expr rr4 = tr;
            Expr gg4 = tg;
            Expr bb4 = tg;

            // Case 5: g > r >= b (RGBTone(g, r, b, gg, rr, bb))
            Expr gg5 = tg;
            Expr bb5 = tb;
            Expr den5 = select(!(r_ >= g_) && (r_ >= b_), g_ - b_, 1.0f);
            Expr rr5 = bb5 + ((gg5 - bb5) * (r_ - b_) / den5);

            // Case 6: b > g > r (RGBTone(b, g, r, bb, gg, rr))
            Expr bb6 = tb;
            Expr rr6 = tr;
            Expr den6 = select(!(r_ >= g_) && !(r_ >= b_) && (b_ > g_), b_ - r_, 1.0f);
            Expr gg6 = rr6 + ((bb6 - rr6) * (g_ - r_) / den6);

            // Case 7: g >= b > r (RGBTone(g, b, r, gg, bb, rr))
            Expr gg7 = tg;
            Expr rr7 = tr;
            Expr den7 = select(!(r_ >= g_) && !(r_ >= b_) && !(b_ > g_), g_ - r_, 1.0f);
            Expr bb7 = rr7 + ((gg7 - rr7) * (b_ - r_) / den7);

            Expr c1 = (r_ >= g_) && (g_ > b_);
            Expr c2 = (r_ >= g_) && !(g_ > b_) && (b_ > r_);
            Expr c3 = (r_ >= g_) && !(g_ > b_) && !(b_ > r_) && (b_ > g_);
            Expr c4 = (r_ >= g_) && !(g_ > b_) && !(b_ > r_) && !(b_ > g_);
            Expr c5 = !(r_ >= g_) && (r_ >= b_);
            Expr c6 = !(r_ >= g_) && !(r_ >= b_) && (b_ > g_);

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

        Expr t_r, t_g, t_b;
        rgb_tone(p_r2, p_g2, p_b2, t_r, t_g, t_b);

        // Keep accumulation order deterministic at the matrix stage.
        Expr tone_r = t_r;
        Expr tone_g = t_g;
        Expr tone_b = t_b;
        Expr f_r_rg = tone_r * rgb_to_final(0, 0) + tone_g * rgb_to_final(1, 0);
        Expr f_g_rg = tone_r * rgb_to_final(0, 1) + tone_g * rgb_to_final(1, 1);
        Expr f_b_rg = tone_r * rgb_to_final(0, 2) + tone_g * rgb_to_final(1, 2);
        Expr f_r_sum = f_r_rg + tone_b * rgb_to_final(2, 0);
        Expr f_g_sum = f_g_rg + tone_b * rgb_to_final(2, 1);
        Expr f_b_sum = f_b_rg + tone_b * rgb_to_final(2, 2);

        Expr f_r = clamp(f_r_sum, 0.0f, 1.0f);
        Expr f_g = clamp(f_g_sum, 0.0f, 1.0f);
        Expr f_b = clamp(f_b_sum, 0.0f, 1.0f);

        auto encode8 = [&](Expr v) {
            Expr g_ = table_interp(encode_gamma, v);
            return clamp(g_ * 255.0f + 0.5f, 0.0f, 255.0f);
        };

        // Same value as DngRenderStage4's output, but kept in float and NOT
        // yet quantised. The `+ 0.5f` rounding bias from encode8 survives the
        // averaging, so the single truncating cast below is a round-to-nearest.
        rendered_rgb(x, y) = Tuple(encode8(f_r), encode8(f_g), encode8(f_b));

        // ---- box-filter downscale of the rendered result ----------------
        Expr src_w = src.dim(0).extent();
        Expr src_h = src.dim(1).extent();
        Expr dw = max(dst_width, 1);
        Expr dh = max(dst_height, 1);
        Expr xx = clamp(x, 0, dw - 1);
        Expr yy = clamp(y, 0, dh - 1);

        // Exact integer-ratio cell. When the requested size is >= the source
        // size this degenerates to a single tap per output pixel, i.e. the
        // kernel is a plain passthrough render at 1:1.
        Expr x0 = (xx * src_w) / dw;
        Expr x1 = ((xx + 1) * src_w) / dw;
        Expr y0 = (yy * src_h) / dh;
        Expr y1 = ((yy + 1) * src_h) / dh;
        Expr cnt_x = max(x1 - x0, 1);
        Expr cnt_y = max(y1 - y0, 1);

        // RDom bounds depend only on the inputs, never on x/y — required.
        Expr max_cw = (src_w + dw - 1) / dw + 1;
        Expr max_ch = (src_h + dh - 1) / dh + 1;
        RDom r(0, max_cw, 0, max_ch, "r");
        Expr in_cell = (r.x < cnt_x) && (r.y < cnt_y);
        Expr rsx = clamp(x0 + r.x, 0, src_w - 1);
        Expr rsy = clamp(y0 + r.y, 0, src_h - 1);

        // One Tuple-valued update, so CSE evaluates the render pipeline once
        // per source pixel and shares it across R/G/B.
        box_acc(x, y) = Tuple(0.0f, 0.0f, 0.0f);
        box_acc(x, y) = Tuple(
            box_acc(x, y)[0] + select(in_cell, rendered_rgb(rsx, rsy)[0], 0.0f),
            box_acc(x, y)[1] + select(in_cell, rendered_rgb(rsx, rsy)[1], 0.0f),
            box_acc(x, y)[2] + select(in_cell, rendered_rgb(rsx, rsy)[2], 0.0f));

        Expr inv_area = 1.0f / cast<float>(cnt_x * cnt_y);
        Expr o_r = cast<uint8_t>(clamp(box_acc(x, y)[0] * inv_area, 0.0f, 255.0f));
        Expr o_g = cast<uint8_t>(clamp(box_acc(x, y)[1] * inv_area, 0.0f, 255.0f));
        Expr o_b = cast<uint8_t>(clamp(box_acc(x, y)[2] * inv_area, 0.0f, 255.0f));

        if (uses_vulkan_planar_layout(get_target())) {
            dst(x, y, c) = select(c == 0, o_r,
                                  c == 1, o_g,
                                          o_b);
        } else {
            dst(x, y, c) = select(c == 0, o_r,
                                  c == 1, o_g,
                                  c == 2, o_b,
                                          cast<uint8_t>(255));
        }
    }

    void schedule() {
        Var x("x"), y("y"), c("c");
        // `rendered_rgb` stays inline inside box_acc's update so that no
        // full-resolution float intermediate is ever materialised (that would
        // be ~3x the size of the RGBA output we are trying to shrink).
        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            dst.bound(c, 0, 4)
               .reorder(c, x, y)
               .gpu_tile(x, y, xo, yo, xi, yi, 16, 16)
               .unroll(c);
            box_acc.compute_at(dst, xo).gpu_threads(x, y);
            box_acc.update(0).gpu_threads(x, y);
        } else {
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, 4)
               .reorder(c, x, y)
               .split(y, yo, yi, 32)
               .parallel(yo)
               .unroll(c);
            box_acc.compute_at(dst, yi);
        }
    }
};

HALIDE_REGISTER_GENERATOR(DngRenderStage4, dng_render_stage4)
HALIDE_REGISTER_GENERATOR(DngRenderStage4Scaled, dng_render_stage4_scaled)
HALIDE_REGISTER_GENERATOR(DngRenderStage4Android, dng_render_stage4_android)
HALIDE_REGISTER_GENERATOR(DngRenderStage4AndroidProbe, dng_render_stage4_android_probe)
