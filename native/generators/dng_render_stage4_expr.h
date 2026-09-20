#ifndef CEYX_DNG_RENDER_STAGE4_EXPR_H
#define CEYX_DNG_RENDER_STAGE4_EXPR_H

// Stage-4 render arithmetic, shared by every generator that renders a
// camera-space RGB triple to display-referred 8-bit.
//
// WHY THIS HEADER EXISTS (mem8 v3, T20 step C)
// --------------------------------------------
// The fused Bayer-demosaic+render kernel needs byte-for-byte the same colour
// arithmetic as DngRenderStage4, differing in exactly one thing: where the
// camera-space RGB triple comes from. In the two-stage pipeline it is read
// from a materialised Stage-3 buffer; in the fused kernel it is produced
// on-chip by the demosaic, and never materialised at all.
//
// The alternative was to copy ~420 lines of colour arithmetic into a second
// generator. That is the duplicated path the project's standing rule forbids:
// a future colour-science fix would land in one copy and silently not the
// other, and nothing in this tree would detect the divergence -- there is no
// cross-generator equivalence test. So the arithmetic lives here once.
//
// HOW EQUIVALENCE WAS ESTABLISHED, AND HOW TO RE-ESTABLISH IT
// ------------------------------------------------------------
// This body was not retyped. It was sliced verbatim out of
// DngRenderGenerator.cpp by native/tests/tmp/t20-05c-extract.py, which applies
// exactly five substitutions (two source extents and three sample reads) and
// asserts each one's match count.
//
// The proof that the move changed nothing is that the emitted AOT archive is
// byte-identical: dng_render_stage4.a's SHA-256 was frozen BEFORE the
// extraction and compared after. Anyone modifying this header should expect
// that archive's SHA-256 to change and should be able to say why; if a change
// here is meant to be behaviour-preserving, that SHA is the check.
//
// THE SEAM, and the only thing a caller varies
// ---------------------------------------------
//   sample(sx, sy, channel) -> Expr    the camera-space value at an UNORIENTED
//                                      source coordinate. Returns the raw
//                                      stored value; this function applies
//                                      src_scale itself, so both callers scale
//                                      identically.
//   src_extent_0 / src_extent_1        the unoriented source extents used to
//                                      clamp the orientation-permuted read.
//
// Everything else is a straight pass-through of the generator's own Inputs.
//
// WHAT IS DELIBERATELY *NOT* IN HERE
// -----------------------------------
// Buffer layout (interleaved vs dense-planar), the `dst` assignment, the
// scheduling directives, and the backend predicate all stay in the generators.
// That is not an oversight: those ARE backend-dependent, while everything in
// this file is not -- the extracted range contains no get_target() call and no
// layout decision, which is precisely why one header can serve both backend
// families without a platform branch.

#include "Halide.h"

namespace ceyx {

// Builds `rendered_rgb(x, y)` as a Tuple of three uint8_t.
//
// Templated rather than taking concrete parameter types so that each caller
// instantiates it on its OWN generator's input types, producing the identical
// Halide IR the un-extracted code produced. A non-template signature would
// have required wrapping the Inputs (e.g. into Funcs), and that wrapping would
// itself perturb the emitted code -- which the SHA gate would then report as a
// failure indistinguishable from a real extraction defect.
template <typename SampleFn, typename BufferInput1D, typename BufferInput2D,
          typename ScalarInput>
void build_dng_render_stage4_rgb(
    Halide::Var x, Halide::Var y,
    SampleFn sample,
    Halide::Expr src_extent_0,
    Halide::Expr src_extent_1,
    Halide::Expr src_scale,
    Halide::Expr orient_a_x, Halide::Expr orient_b_x, Halide::Expr orient_c_x,
    Halide::Expr orient_a_y, Halide::Expr orient_b_y, Halide::Expr orient_c_y,
    BufferInput1D& exp_ramp,
    BufferInput1D& tone_curve,
    BufferInput1D& encode_gamma,
    BufferInput1D& camera_white,
    BufferInput2D& camera_to_rgb,
    BufferInput2D& rgb_to_final,
    BufferInput2D& huesat_table,
    BufferInput1D& huesat_encode,
    BufferInput1D& huesat_decode,
    ScalarInput& huesat_hue_div,
    ScalarInput& huesat_sat_div,
    ScalarInput& huesat_val_div,
    ScalarInput& huesat_has_table,
    ScalarInput& huesat_has_encoding,
    BufferInput2D& look_table,
    BufferInput1D& look_encode,
    BufferInput1D& look_decode,
    ScalarInput& look_hue_div,
    ScalarInput& look_sat_div,
    ScalarInput& look_val_div,
    ScalarInput& look_has_table,
    ScalarInput& look_has_encoding,
    Halide::Func& rendered_rgb) {
    using namespace Halide;

// ---- BEGIN verbatim slice of DngRenderGenerator.cpp:97-467 ----------------
// Sliced by native/tests/tmp/t20-05c-extract.py. Do not hand-edit: re-run the
// script instead, so the slice provenance stays true.
        // Fused EXIF orientation: permute the OUTPUT coordinate back to the
        // unoriented coordinate before any pixel math runs. Every arithmetic
        // expression below is untouched, so the fused result is a pure index
        // permutation of the unfused one -> byte-exact by construction.
        // Table mirrors native/tests/oracle/ceyx_orient_oracle.cpp exactly.
        //   1: (x, y)              5: (y, x)
        //   2: (W-1-x, y)          6: (y, H-1-x)
        //   3: (W-1-x, H-1-y)      7: (W-1-y, H-1-x)
        //   4: (x, H-1-y)          8: (W-1-y, x)
        // Cases 5-8 transpose, so the caller sizes dst as (H, W).
        //
        // AFFINE FORM (T7b). The kernel decides NOTHING about orientation. The
        // host computes six int32 coefficients with ceyx_orient_affine_coeffs()
        // (native/include/ceyx_orient.h) and the kernel evaluates one integer
        // multiply-add per axis. There is deliberately no select, no boolean,
        // no cast-from-bool and no comparison against an orientation value
        // anywhere below; the unoriented extents are folded into the c terms on
        // the host, so no extent scalar reaches the kernel either.
        // Two earlier IN-KERNEL formulations mis-lowered on Adreno 750 / Vulkan
        // while their CPU controls were 8/8 correct on the same text:
        //   F-T6-1 (Task_t6_android_device_gate.md) 8-way equality chain of
        //     selects -- asked 4,5,6 it produced the image for 3, asked 8 it
        //     produced 7, byte-exactly. Suspected CSE collapse across select
        //     arms sharing a value expression.
        //   F-T8-1 (Task_t8_android_device_gate.md) branch-free three-flag form
        //     -- 30/40 byte-compares mismatched; every orientation with at
        //     least one flag set behaved as if more flags were set, and only
        //     the all-clear and all-set cases survived. Consistent with the
        //     bool-to-int conversion lowering to selects internally.
        // DO NOT re-land either form on any backend, and do not reintroduce an
        // orientation scalar here: the branch belongs on the host.
        Expr ux = orient_a_x * x + orient_b_x * y + orient_c_x;
        Expr uy = orient_a_y * x + orient_b_y * y + orient_c_y;
        // ---- end fused orientation permutation ----
        Expr sx = clamp(ux, 0, src_extent_0 - 1);
        Expr sy = clamp(uy, 0, src_extent_1 - 1);
        Expr s_r = cast<float>(sample(sx, sy, 0)) * src_scale;
        Expr s_g = cast<float>(sample(sx, sy, 1)) * src_scale;
        Expr s_b = cast<float>(sample(sx, sy, 2)) * src_scale;

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
// ---- END verbatim slice -----------------------------------------------------
}

}  // namespace ceyx

#endif  // CEYX_DNG_RENDER_STAGE4_EXPR_H
