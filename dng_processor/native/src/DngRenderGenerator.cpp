#include "Halide.h"

using namespace Halide;


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

        if (get_target().os == Target::Android) {
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
        if (get_target().os == Target::Android) {
            // Vulkan SPIR-V workaround: use dense planar dst so GPU code never
            // writes interleaved channel-stride-1 output on dim(2).
            dst.dim(0).set_stride(1);
            dst.dim(1).set_stride(dst.dim(0).extent());
            dst.dim(2).set_bounds(0, 3);
            dst.dim(2).set_stride(dst.dim(0).extent() * dst.dim(1).extent());
        } else {
            dst.dim(0).set_stride(3);
            dst.dim(2).set_bounds(0, 3);
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

        dst(x, y, c) = select(c == 0, rendered_rgb(x, y)[0],
                              c == 1, rendered_rgb(x, y)[1],
                                      rendered_rgb(x, y)[2]);
    }

    void schedule() {
        Var x("x"), y("y"), c("c");
        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            // P12-W1-03: fuse rendered_rgb into dst via compute_at to eliminate
            // the ~145.8 MB intermediate GPU global-memory roundtrip and collapse
            // two Metal kernel dispatches into one. dst now drives the 16x16 tile.
            dst.bound(c, 0, 3)
               .reorder(c, x, y)
               .gpu_tile(x, y, xo, yo, xi, yi, 16, 16)
               .unroll(c);
            rendered_rgb.compute_at(dst, xo)
                        .gpu_threads(x, y);
        } else {
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, 3)
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
// Android Vulkan workaround: separate generator with one-dimensional planar
// buffers per channel.  Avoids Tuple/2D/3D buffer stride codegen on Vulkan.
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
    Input<int32_t> dst_width{"dst_width"};
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

    // W4-3 (b): single 2D planar output dst(i, c) — dim0 = pixel index i
    // (stride 1, extent N), dim1 = channel c (stride N, extent 3). This is the
    // three former dst_r/dst_g/dst_b planes laid end-to-end in one buffer, so it
    // dispatches as ONE Vulkan kernel instead of three. With reorder(c,i) +
    // unroll(c), the c-index folds to a constant per unrolled copy so select()
    // prunes to one channel and the c-independent pipeline body is CSE-shared
    // inside one thread (computed once per pixel, not three times). 2D dense
    // buffers are verified safe on Vulkan (W3 §1); the host still repacks the
    // planar output to interleaved RGB8 (W4-1 MT repack_dst).
    //
    // Plan (a) — a single interleaved 1D dst_rgb(j) with j=i*3+c — was tried
    // first but produced incorrect output on Vulkan (~8 dB, border/coverage
    // corruption); the j/3, j%3 affine reconstruction under split+gpu_tile does
    // not lower correctly. (b) keeps the single-dispatch + compute-1x win without
    // the interleaved-index codegen hazard.
    Output<Buffer<uint8_t>> dst{"dst", 2};

    // Per-channel 8-bit results, set by emit_rgb8 (possibly from a diag stage),
    // consumed once at the end of generate() to build dst.
    Expr out8_r, out8_g, out8_b;

    void generate() {
        Var i("i");

        // W4-3 (b): dense planar dst layout — dim0=i (stride 1), dim1=c (stride
        // = N). Matches the host buffer wrap (three contiguous planes).
        dst.dim(0).set_stride(1);
        dst.dim(1).set_bounds(0, 3);
        dst.dim(1).set_stride(dst.dim(0).extent());

        Expr dst_x = i % dst_width;
        Expr dst_y = i / dst_width;
        Expr sx = clamp(dst_x + crop_l, 0, src_width - 1);
        Expr sy = clamp(dst_y + crop_t, 0, src_height - 1);

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
            finalize(i);
            return;
        }

        Expr wb_r = min(s_r, camera_white(0));
        Expr wb_g = min(s_g, camera_white(1));
        Expr wb_b = min(s_b, camera_white(2));

        if (diag_stage == 1) {
            emit_rgb8(linear8(wb_r), linear8(wb_g), linear8(wb_b));
            finalize(i);
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
            finalize(i);
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
            finalize(i);
            return;
        }

        Expr e_r = table_interp(exp_ramp, p_r1);
        Expr e_g = table_interp(exp_ramp, p_g1);
        Expr e_b = table_interp(exp_ramp, p_b1);

        if (diag_stage == 4) {
            emit_rgb8(linear8(e_r), linear8(e_g), linear8(e_b));
            finalize(i);
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
            finalize(i);
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
            finalize(i);
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
            finalize(i);
            return;
        }

        auto encode8 = [&](Expr v) {
            Expr g = table_interp(encode_gamma, v);
            return clamp(g * 255.0f + 0.5f, 0.0f, 255.0f);
        };

        emit_rgb8(cast<uint8_t>(encode8(f_r)),
                  cast<uint8_t>(encode8(f_g)),
                  cast<uint8_t>(encode8(f_b)));
        finalize(i);
    }

    // W4-3 (b): wire the three channel results into the 2D planar output.
    // out8_r/g/b are Exprs over the pixel index `i`; dst(i, c) selects a channel
    // by c. reorder(c,i) + unroll(c) fold c to a constant per copy so select()
    // prunes to one channel while the pipeline body (independent of c) is shared.
    void finalize(Var i) {
        Var c("c");
        dst(i, c) = select(c == 0, out8_r, c == 1, out8_g, out8_b);
    }

    void schedule() {
        Var i("i"), c("c");
        // W4-3 (b): single 2D planar output. reorder(c,i) + unroll(c) keeps the
        // three channels of one pixel in the same thread so c folds to a constant
        // per copy, select() prunes to one channel, and the c-independent
        // pipeline body is CSE-shared (computed once per pixel, one kernel).
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

HALIDE_REGISTER_GENERATOR(DngRenderStage4, dng_render_stage4)
HALIDE_REGISTER_GENERATOR(DngRenderStage4Android, dng_render_stage4_android)
HALIDE_REGISTER_GENERATOR(DngRenderStage4AndroidProbe, dng_render_stage4_android_probe)
