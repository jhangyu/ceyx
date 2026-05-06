#include "Halide.h"

using namespace Halide;

class DngRenderMapsNoEncodingStage4 : public Halide::Generator<DngRenderMapsNoEncodingStage4> {
public:
    Input<Buffer<uint16_t>> src{"src", 3};          // x, y, c (Stage3 interleaved RGB uint16)
    Input<float> src_scale{"src_scale"};            // usually 1 / 65535
    Input<Buffer<float>> exp_ramp{"exp_ramp", 1};   // 4098
    Input<Buffer<float>> tone_curve{"tone_curve", 1}; // 4098
    Input<Buffer<float>> encode_gamma{"encode_gamma", 1}; // 4098
    Input<Buffer<float>> camera_white{"camera_white", 1}; // 3
    Input<Buffer<float>> camera_to_rgb{"camera_to_rgb", 2}; // [col, row] 3x3
    Input<Buffer<float>> rgb_to_final{"rgb_to_final", 2};   // [col, row] 3x3
    Input<Buffer<float>> huesat_table{"huesat_table", 2};   // [entry, component]
    Input<int32_t> huesat_hue_div{"huesat_hue_div"};
    Input<int32_t> huesat_sat_div{"huesat_sat_div"};
    Input<int32_t> huesat_val_div{"huesat_val_div"};
    Input<Buffer<float>> look_table{"look_table", 2};       // [entry, component]
    Input<int32_t> look_hue_div{"look_hue_div"};
    Input<int32_t> look_sat_div{"look_sat_div"};
    Input<int32_t> look_val_div{"look_val_div"};
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

        auto rgb_to_hsv = [&](Expr rr, Expr gg, Expr bb, Expr& h, Expr& s, Expr& v) {
            v = max(rr, max(gg, bb));
            Expr mn = min(rr, min(gg, bb));
            Expr gap = v - mn;

            Expr gap_den = select(gap > 0.0f, gap, 1.0f);
            Expr h_r = (gg - bb) / gap_den;
            Expr h_r_fix = select(h_r < 0.0f, h_r + 6.0f, h_r);
            Expr h_g = 2.0f + (bb - rr) / gap_den;
            Expr h_b = 4.0f + (rr - gg) / gap_den;

            h = select(gap > 0.0f,
                       select(rr == v, h_r_fix,
                              gg == v, h_g,
                                       h_b),
                       0.0f);
            s = select(gap > 0.0f, gap / v, 0.0f);
        };

        auto hsv_to_rgb = [&](Expr h, Expr s, Expr v, Expr& rr, Expr& gg, Expr& bb) {
            Expr hh = select(h < 0.0f, h + 6.0f, h);
            hh = select(hh >= 6.0f, hh - 6.0f, hh);
            Expr i = cast<int>(hh);
            Expr f = hh - cast<float>(i);
            Expr p = v * (1.0f - s);
            Expr q = v * (1.0f - s * f);
            Expr t = v * (1.0f - s * (1.0f - f));
            Expr cc = clamp(i, 0, 5);

            rr = select(cc == 0, v,
                        cc == 1, q,
                        cc == 2, p,
                        cc == 3, p,
                        cc == 4, t,
                                 v);
            gg = select(cc == 0, t,
                        cc == 1, v,
                        cc == 2, v,
                        cc == 3, q,
                        cc == 4, p,
                                 p);
            bb = select(cc == 0, p,
                        cc == 1, p,
                        cc == 2, t,
                        cc == 3, v,
                        cc == 4, v,
                                 q);
        };

        auto sample_hsv_map_noencode = [&](const auto& table,
                                           Expr hue_div,
                                           Expr sat_div,
                                           Expr val_div,
                                           Expr rr,
                                           Expr gg,
                                           Expr bb,
                                           Expr& out_r,
                                           Expr& out_g,
                                           Expr& out_b) {
            Expr h, s, v;
            rgb_to_hsv(rr, gg, bb, h, s, v);

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

            Expr h_scaled = h * hue_scale;
            Expr s_scaled = s * sat_scale;
            Expr v_scaled = v * val_scale;

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
            Expr hue_shift = select(use_2d, hue_shift_2d, hue_shift_3d);
            Expr sat_mult = select(use_2d, sat_scale_2d, sat_scale_3d);
            Expr val_mult = select(use_2d, val_scale_2d, val_scale_3d);

            Expr hh = h + hue_shift * (6.0f / 360.0f);
            Expr ss = min(s * sat_mult, 1.0f);
            Expr vv = clamp(v * val_mult, 0.0f, 1.0f);

            hsv_to_rgb(hh, ss, vv, out_r, out_g, out_b);
        };

        Expr p_r1, p_g1, p_b1;
        sample_hsv_map_noencode(huesat_table,
                                huesat_hue_div,
                                huesat_sat_div,
                                huesat_val_div,
                                p_r0,
                                p_g0,
                                p_b0,
                                p_r1,
                                p_g1,
                                p_b1);

        Expr e_r = table_interp(exp_ramp, p_r1);
        Expr e_g = table_interp(exp_ramp, p_g1);
        Expr e_b = table_interp(exp_ramp, p_b1);

        Expr p_r2, p_g2, p_b2;
        sample_hsv_map_noencode(look_table,
                                look_hue_div,
                                look_sat_div,
                                look_val_div,
                                e_r,
                                e_g,
                                e_b,
                                p_r2,
                                p_g2,
                                p_b2);

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
        rgb_tone(p_r2, p_g2, p_b2, t_r, t_g, t_b);

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

HALIDE_REGISTER_GENERATOR(DngRenderMapsNoEncodingStage4, dng_render_maps_noencode_stage4)
