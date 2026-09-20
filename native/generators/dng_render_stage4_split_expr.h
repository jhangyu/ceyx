#ifndef CEYX_DNG_RENDER_STAGE4_SPLIT_EXPR_H
#define CEYX_DNG_RENDER_STAGE4_SPLIT_EXPR_H

// Stage-4 render arithmetic for the SPLIT (Vulkan) generator family, shared by
// every generator that renders a flat-1D interleaved camera-space RGB source to
// display-referred 8-bit on that family's input signature.
//
// WHY THIS HEADER EXISTS (mem8 v3, T12-pre)
// ------------------------------------------
// dng_render_stage4_split_expr.h is the split-family twin of
// dng_render_stage4_expr.h. T20 step C extracted the NON-split (Metal)
// generator's colour body so the fused Bayer kernel could share it;
// DngRenderStage4Android was left with its own copy, because its inputs differ
// (flat-1D interleaved src, 1-D matrices/tables plus explicit entry counts,
// where the non-split generator has a 3-D src and 2-D matrices).
//
// T12 adds yuv420 output variants. Done naively that would put FOUR copies of
// the Stage-4 colour math in this tree. The project's standing rule forbids
// maintaining a second path for the same behaviour, and nothing here would
// detect the drift: there is no cross-generator equivalence test.
//
// HOW EQUIVALENCE WAS ESTABLISHED, AND HOW TO RE-ESTABLISH IT
// ------------------------------------------------------------
// This body was not retyped. It was sliced verbatim out of
// DngRenderGenerator.cpp by native/tests/tmp/t12-02-extract.py, which applies
// ZERO substitutions and asserts every anchor it cuts on.
//
// The proof that the move changed nothing is that the emitted AOT archive is
// byte-identical: dng_render_stage4_split.a's SHA-256 was frozen BEFORE the
// extraction and compared after, with the gate proven live by perturbing one
// constant. See native/tests/tmp/t12-02-extraction-prereg.txt. Anyone modifying
// this header should expect that SHA-256 to change and should be able to say
// why; if a change here is meant to be behaviour-preserving, that SHA is the
// check.
//
// THE SEAM
// --------
// The caller supplies the generator's Inputs and three out-Exprs; the helper
// produces the three per-pixel 8-bit channel values and nothing else. Writing
// them to an output buffer is the CALLER's job -- DngRenderStage4Android's
// finalize() writes interleaved RGBA8, and the yuv420 variants write planes.
// That is the whole point of the split: the colour math is identical across
// output formats, and the output format is never a branch inside the kernel.
//
// The diag_stage ladder travels WITH the body as a plain `int` parameter: it is
// a GeneratorParam (compile-time), never an in-kernel branch.
//
// WHAT IS DELIBERATELY *NOT* IN HERE
// -----------------------------------
// Buffer layout, the `dst` assignment, the scheduling directives and the
// backend predicate stay in the generators. The extracted range contains no
// get_target() call and no layout decision.

#include "Halide.h"

namespace ceyx {

// Templated rather than taking concrete parameter types so that each caller
// instantiates it on its OWN generator's input types, producing the identical
// Halide IR the un-extracted code produced. A non-template signature would have
// required wrapping the Inputs, and that wrapping would itself perturb the
// emitted code -- which the SHA gate would then report as a failure
// indistinguishable from a real extraction defect.
template <typename SrcInput1D, typename BufferInput1D, typename IntScalar,
          typename FloatScalar>
void build_dng_render_stage4_split_rgb8(
    Halide::Var x, Halide::Var y,
    int diag_stage,
    SrcInput1D& src_rgb,
    IntScalar& src_width,
    IntScalar& src_height,
    IntScalar& src_row_stride_px,
    IntScalar& crop_l,
    IntScalar& crop_t,
    FloatScalar& src_scale,
    IntScalar& orient_a_x, IntScalar& orient_b_x, IntScalar& orient_c_x,
    IntScalar& orient_a_y, IntScalar& orient_b_y, IntScalar& orient_c_y,
    BufferInput1D& exp_ramp,
    BufferInput1D& tone_curve,
    BufferInput1D& encode_gamma,
    BufferInput1D& camera_white,
    BufferInput1D& camera_to_rgb,
    BufferInput1D& rgb_to_final,
    BufferInput1D& huesat_table,
    BufferInput1D& huesat_encode,
    BufferInput1D& huesat_decode,
    IntScalar& huesat_entry_count,
    IntScalar& huesat_hue_div,
    IntScalar& huesat_sat_div,
    IntScalar& huesat_val_div,
    IntScalar& huesat_has_table,
    IntScalar& huesat_has_encoding,
    BufferInput1D& look_table,
    BufferInput1D& look_encode,
    BufferInput1D& look_decode,
    IntScalar& look_entry_count,
    IntScalar& look_hue_div,
    IntScalar& look_sat_div,
    IntScalar& look_val_div,
    IntScalar& look_has_table,
    IntScalar& look_has_encoding,
    Halide::Expr& out8_r, Halide::Expr& out8_g, Halide::Expr& out8_b) {
    using namespace Halide;

// ---- BEGIN verbatim slice of DngRenderGenerator.cpp:311-632 ----------------
// Sliced by native/tests/tmp/t12-02-extract.py. Do not hand-edit: re-run the
// script instead, so the slice provenance stays true. The 8 `finalize(x, y);`
// calls of the diag ladder are the only lines dropped; the caller makes that
// call once, after this function returns.
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
        // Per-class clamp: this kernel gathers from a cropped 1D interleaved
        // source, so the permuted coordinate carries the crop offset.
        Expr sx = clamp(ux + crop_l, 0, src_width - 1);
        Expr sy = clamp(uy + crop_t, 0, src_height - 1);

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
            return;
        }

        Expr wb_r = min(s_r, camera_white(0));
        Expr wb_g = min(s_g, camera_white(1));
        Expr wb_b = min(s_b, camera_white(2));

        if (diag_stage == 1) {
            emit_rgb8(linear8(wb_r), linear8(wb_g), linear8(wb_b));
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
            return;
        }

        Expr e_r = table_interp(exp_ramp, p_r1);
        Expr e_g = table_interp(exp_ramp, p_g1);
        Expr e_b = table_interp(exp_ramp, p_b1);

        if (diag_stage == 4) {
            emit_rgb8(linear8(e_r), linear8(e_g), linear8(e_b));
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
            return;
        }

        auto encode8 = [&](Expr v) {
            Expr g = table_interp(encode_gamma, v);
            return clamp(g * 255.0f + 0.5f, 0.0f, 255.0f);
        };

        emit_rgb8(cast<uint8_t>(encode8(f_r)),
                  cast<uint8_t>(encode8(f_g)),
                  cast<uint8_t>(encode8(f_b)));
// ---- END verbatim slice -----------------------------------------------------
}

}  // namespace ceyx

#endif  // CEYX_DNG_RENDER_STAGE4_SPLIT_EXPR_H
