#pragma once

// ---------------------------------------------------------------------------
// W6-4 / TD-22: shared Halide IR helpers used by Stage3 demosaic / warp /
// fused-demosaic-warp AOT generators.
//
// Prior to W6 these helpers were duplicated verbatim (or nearly so) at the
// top of:
//   - dng_processor/native/src/DngDemosaicGenerator.cpp
//   - dng_processor/native/src/DngDemosaicWarpGenerator.cpp
//   - dng_processor/native/src/DngWarpGenerator.cpp
//
// `map_repeat_coord` had already drifted between copies:
//   * DngDemosaicGenerator.cpp           used `Expr(kCfaRepeat)` (= 2)
//   * DngDemosaicWarpGenerator.cpp       used a bare `Expr(2)` literal
//   * (DngWarpGenerator.cpp did not need it)
// The unified helper here keeps the named `kCfaRepeat` constant so the
// intent (Bayer CFA 2x2 tile repeat at the buffer edge) is explicit.
//
// All helpers are `inline` and live in an anonymous namespace so each
// translation unit gets its own internal copy at -O2; no extra symbols
// leak into the `dng_decoder_native` dylib.
//
// Pure refactor — AOT outputs must be bit-exact vs pre-W6 (matrix PSNR
// numbers must not move; `nm -gU` must show no new/missing exports).
//
// M-3 dedup (W3): `build_demosaic_expr`, `compute_warp_polynomial`, and
// `build_warp_bicubic_expr` extract the 43-line demosaic and 100-line
// warp+bicubic blocks that were duplicated across the fused and fallback
// generators.  The warp helper parameterizes the sample source via a
// std::function so the fused generator passes its demosaic Func while
// the standalone warp passes its src Func.
// ---------------------------------------------------------------------------

#include "Halide.h"
#include <functional>

namespace {

// Bayer CFA repeats every 2 pixels in each axis; used by the edge-repeat
// boundary logic in `map_repeat_coord` so that out-of-range sample
// coordinates wrap onto a same-color CFA site instead of the nearest pixel.
constexpr int kCfaRepeat = 2;

// Resample subsample table size used by bicubic phase indexing (32 phases).
constexpr int kResampleSubsampleBits2D = 5;
constexpr int kResampleSubsampleCount2D = 1 << kResampleSubsampleBits2D;

inline Halide::Expr positive_modulo(Halide::Expr v, Halide::Expr m) {
    return ((v % m) + m) % m;
}

// CFA-aware edge clamp.  When `coord` is outside [0, size), wrap by
// `kCfaRepeat` so the returned coordinate lands on the same CFA color
// as a naive boundary pixel would.  Matches the SDK's edge behaviour
// closely enough that fused demosaic + warp stays bit-exact vs the SDK
// reference at full image extent.
inline Halide::Expr map_repeat_coord(Halide::Expr coord, Halide::Expr size) {
    Halide::Expr repeat = Halide::min(Halide::Expr(kCfaRepeat), size);
    Halide::Expr start = size - repeat;
    return Halide::select(coord < 0,
                          positive_modulo(coord, repeat),
                          coord >= size,
                          start + positive_modulo(coord - start, repeat),
                          coord);
}

// Generalised CFA-aware edge clamp for repeats other than 2 (X-Trans is 6).
// Deliberately a SEPARATE function: `map_repeat_coord` above must keep
// emitting byte-identical IR, because the DNG demosaic/warp AOT outputs are
// pinned regression artifacts (Gotcha #99).  Re-expressing the old helper in
// terms of this one is therefore NOT acceptable, even though the bodies are
// structurally identical with repeat_n == kCfaRepeat.
inline Halide::Expr map_repeat_coord_n(Halide::Expr coord, Halide::Expr size,
                                       int repeat_n) {
    Halide::Expr repeat = Halide::min(Halide::Expr(repeat_n), size);
    Halide::Expr start = size - repeat;
    return Halide::select(coord < 0,
                          positive_modulo(coord, repeat),
                          coord >= size,
                          start + positive_modulo(coord - start, repeat),
                          coord);
}

inline Halide::Expr avg2_u16(Halide::Expr a, Halide::Expr b) {
    return Halide::cast<uint16_t>(
        (Halide::cast<uint32_t>(a) + Halide::cast<uint32_t>(b) +
         Halide::cast<uint32_t>(1)) >>
        1);
}

inline Halide::Expr avg4_u16(Halide::Expr a, Halide::Expr b, Halide::Expr c,
                             Halide::Expr d) {
    Halide::Expr total = Halide::cast<uint32_t>(a) + Halide::cast<uint32_t>(b) +
                         Halide::cast<uint32_t>(c) + Halide::cast<uint32_t>(d);
    return Halide::cast<uint16_t>((total + Halide::cast<uint32_t>(2)) >> 2);
}

// Cubic resampler weight (Mitchell-style 4-tap with a = -0.75).
inline Halide::Expr cubic_weight(Halide::Expr x) {
    const float a = -0.75f;
    x = Halide::abs(x);
    return Halide::select(
        x >= 2.0f, 0.0f,
        x >= 1.0f, (((a * x - 5.0f * a) * x + 8.0f * a) * x - 4.0f * a),
        (((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f));
}

// ---------------------------------------------------------------------------
// M-3: Shared builder helpers for demosaic and warp IR dedup.
// ---------------------------------------------------------------------------

// Bilinear demosaic: 9-neighbor Bayer interpolation, phase-parameterized.
// `sample(sx, sy)` returns a uint16_t Halide::Expr for the CFA pixel at
// (sx, sy), with caller-defined boundary handling (typically
// map_repeat_coord).  Returns the select(c==0, R, c==1, G, B) expression.
//
// CFA phase (2026-08-16): `red_x` / `red_y` are the column / row parity of the
// red CFA site (0 or 1), i.e. red lives wherever `x % 2 == red_x` and
// `y % 2 == red_y`.  All four standard Bayer phases are covered by one
// expression:
//   RGGB -> (0, 0)   GRBG -> (1, 0)   GBRG -> (0, 1)   BGGR -> (1, 1)
// They are runtime Exprs (Input<int32_t> on the generators), not compile-time
// GeneratorParams: the AOT kernels are compiled once but the phase is a
// per-file property read from the DNG CFAPattern tag.  Passing (0, 0)
// reproduces the pre-2026-08-16 hard-coded RGGB behaviour bit-exactly.
inline Halide::Expr build_demosaic_expr(
    Halide::Var x, Halide::Var y, Halide::Var c,
    std::function<Halide::Expr(Halide::Expr, Halide::Expr)> sample,
    Halide::Expr red_x, Halide::Expr red_y)
{
    Halide::Expr center = sample(x, y);
    Halide::Expr n  = sample(x, y - 1);
    Halide::Expr s  = sample(x, y + 1);
    Halide::Expr w  = sample(x - 1, y);
    Halide::Expr e  = sample(x + 1, y);
    Halide::Expr nw = sample(x - 1, y - 1);
    Halide::Expr ne = sample(x + 1, y - 1);
    Halide::Expr sw = sample(x - 1, y + 1);
    Halide::Expr se = sample(x + 1, y + 1);

    Halide::Expr red_row = (y % 2) == red_y;
    Halide::Expr red_col = (x % 2) == red_x;
    Halide::Expr red_site        = red_row && red_col;
    Halide::Expr blue_site       = !red_row && !red_col;
    Halide::Expr green_on_red_row = red_row && !red_col;

    Halide::Expr r = Halide::select(red_site,
                        center,
                        blue_site,
                        avg4_u16(nw, ne, sw, se),
                        green_on_red_row,
                        avg2_u16(w, e),
                        avg2_u16(n, s));

    Halide::Expr g = Halide::select(red_site || blue_site,
                        avg4_u16(n, s, w, e),
                        center);

    Halide::Expr b = Halide::select(red_site,
                        avg4_u16(nw, ne, sw, se),
                        blue_site,
                        center,
                        green_on_red_row,
                        avg2_u16(n, s),
                        avg2_u16(w, e));

    return Halide::select(c == 0, r, c == 1, g, b);
}

// Intermediate warp coordinate result.
struct WarpCoords {
    Halide::Expr base_x, base_y;
    Halide::Expr frac_x_idx, frac_y_idx;
};

// Radial + tangential lens distortion polynomial → bicubic base/frac coords.
// `rad_access(coeff, plane)` and `tan_access(coeff, plane)` provide buffer
// lookups for the 4-coeff radial and 2-coeff tangential polynomials.
// The caller computes `plane` (clamped per-channel index) before calling.
inline WarpCoords compute_warp_polynomial(
    Halide::Var x, Halide::Var y,
    Halide::Expr plane,
    Halide::Expr center_x, Halide::Expr center_y,
    Halide::Expr norm_radius, Halide::Expr inv_norm_radius,
    Halide::Expr pixel_scale_v, Halide::Expr pixel_scale_v_inv,
    Halide::Expr is_rad_nop_all, Halide::Expr is_tan_nop_all,
    std::function<Halide::Expr(int, Halide::Expr)> rad_access,
    std::function<Halide::Expr(int, Halide::Expr)> tan_access)
{
    Halide::Expr diff_x = Halide::cast<float>(x) - center_x;
    Halide::Expr diff_y = Halide::cast<float>(y) - center_y;
    Halide::Expr diff_norm_x = diff_x * inv_norm_radius;
    Halide::Expr diff_norm_y = diff_y * inv_norm_radius;
    Halide::Expr diff_scaled_x = diff_norm_x;
    Halide::Expr diff_scaled_y = diff_norm_y * pixel_scale_v;
    Halide::Expr rr = Halide::min(diff_scaled_x * diff_scaled_x +
                                  diff_scaled_y * diff_scaled_y, 1.0f);

    Halide::Expr ratio = rad_access(0, plane) +
        rr * (rad_access(1, plane) +
              rr * (rad_access(2, plane) +
                    rr * rad_access(3, plane)));

    Halide::Expr tan_v = tan_access(0, plane) *
                            (rr + 2.0f * diff_scaled_y * diff_scaled_y) +
                         (2.0f * tan_access(1, plane) *
                            diff_scaled_x * diff_scaled_y);
    Halide::Expr tan_h = tan_access(1, plane) *
                            (rr + 2.0f * diff_scaled_x * diff_scaled_x) +
                         (2.0f * tan_access(0, plane) *
                            diff_scaled_x * diff_scaled_y);

    Halide::Expr src_x_rad  = center_x + diff_x * ratio;
    Halide::Expr src_y_rad  = center_y + diff_y * ratio;
    Halide::Expr src_x_tan  = Halide::cast<float>(x) + norm_radius * tan_h;
    Halide::Expr src_y_tan  = Halide::cast<float>(y) +
                              norm_radius * tan_v * pixel_scale_v_inv;
    Halide::Expr src_x_both = center_x +
        norm_radius * (diff_norm_x * ratio + tan_h);
    Halide::Expr src_y_both = center_y +
        norm_radius * (diff_norm_y * ratio + tan_v * pixel_scale_v_inv);

    Halide::Expr src_x = Halide::select(is_tan_nop_all != 0,
                            src_x_rad,
                            is_rad_nop_all != 0,
                            src_x_tan,
                            src_x_both);
    Halide::Expr src_y = Halide::select(is_tan_nop_all != 0,
                            src_y_rad,
                            is_rad_nop_all != 0,
                            src_y_tan,
                            src_y_both);

    Halide::Expr src_x_floor = Halide::floor(src_x);
    Halide::Expr src_y_floor = Halide::floor(src_y);

    Halide::Expr frac_x_idx = Halide::clamp(
        Halide::cast<int>(Halide::floor(
            (src_x - src_x_floor) * kResampleSubsampleCount2D)),
        0, kResampleSubsampleCount2D - 1);
    Halide::Expr frac_y_idx = Halide::clamp(
        Halide::cast<int>(Halide::floor(
            (src_y - src_y_floor) * kResampleSubsampleCount2D)),
        0, kResampleSubsampleCount2D - 1);

    Halide::Expr base_x = Halide::cast<int>(src_x_floor) - 1;
    Halide::Expr base_y = Halide::cast<int>(src_y_floor) - 1;

    return {base_x, base_y, frac_x_idx, frac_y_idx};
}

// Bicubic 4×4 interpolation with tile-bounds clamping.
// Returns the interpolated float value; caller casts to output type and
// assigns to dst.  `sample_source(x, y, c)` returns a float Expr from
// the source domain (clamped demosaic Func for fused, clamped src Func
// for standalone warp).
inline Halide::Expr build_warp_bicubic_expr(
    Halide::Var x, Halide::Var y, Halide::Var c,
    const WarpCoords& coords,
    Halide::Expr tile_width, Halide::Expr tile_height,
    std::function<Halide::Expr(int, Halide::Expr, Halide::Expr)> tile_bounds_access,
    Halide::Expr tile_bounds_x_extent, Halide::Expr tile_bounds_y_extent,
    std::function<Halide::Expr(Halide::Expr, Halide::Expr, Halide::Expr)> sample_source)
{
    Halide::Expr safe_tile_w = Halide::max(tile_width, 1);
    Halide::Expr safe_tile_h = Halide::max(tile_height, 1);
    Halide::Expr tile_x = Halide::clamp(x / safe_tile_w, 0,
                                        tile_bounds_x_extent - 1);
    Halide::Expr tile_y = Halide::clamp(y / safe_tile_h, 0,
                                        tile_bounds_y_extent - 1);

    Halide::Expr min_base_x = tile_bounds_access(0, tile_x, tile_y);
    Halide::Expr max_base_x = tile_bounds_access(1, tile_x, tile_y);
    Halide::Expr min_base_y = tile_bounds_access(2, tile_x, tile_y);
    Halide::Expr max_base_y = tile_bounds_access(3, tile_x, tile_y);

    Halide::Expr clipped_x = coords.base_x < min_base_x ||
                             coords.base_x > max_base_x;
    Halide::Expr clipped_y = coords.base_y < min_base_y ||
                             coords.base_y > max_base_y;

    Halide::Expr base_x_clamped = Halide::clamp(coords.base_x,
                                                min_base_x, max_base_x);
    Halide::Expr base_y_clamped = Halide::clamp(coords.base_y,
                                                min_base_y, max_base_y);

    Halide::Expr frac_x = Halide::select(clipped_x,
                    0.0f,
                    Halide::cast<float>(coords.frac_x_idx) /
                        kResampleSubsampleCount2D);
    Halide::Expr frac_y = Halide::select(clipped_y,
                    0.0f,
                    Halide::cast<float>(coords.frac_y_idx) /
                        kResampleSubsampleCount2D);

    Halide::Expr w0x = cubic_weight((-1.0f) - frac_x);
    Halide::Expr w1x = cubic_weight(0.0f - frac_x);
    Halide::Expr w2x = cubic_weight(1.0f - frac_x);
    Halide::Expr w3x = cubic_weight(2.0f - frac_x);
    Halide::Expr sumx = w0x + w1x + w2x + w3x;
    w0x /= sumx;  w1x /= sumx;  w2x /= sumx;  w3x /= sumx;

    Halide::Expr w0y = cubic_weight((-1.0f) - frac_y);
    Halide::Expr w1y = cubic_weight(0.0f - frac_y);
    Halide::Expr w2y = cubic_weight(1.0f - frac_y);
    Halide::Expr w3y = cubic_weight(2.0f - frac_y);
    Halide::Expr sumy = w0y + w1y + w2y + w3y;
    w0y /= sumy;  w1y /= sumy;  w2y /= sumy;  w3y /= sumy;

    auto sample_row = [&](Halide::Expr yy,
                          Halide::Expr wx0, Halide::Expr wx1,
                          Halide::Expr wx2, Halide::Expr wx3) {
        return wx0 * sample_source(base_x_clamped + 0, yy, c) +
               wx1 * sample_source(base_x_clamped + 1, yy, c) +
               wx2 * sample_source(base_x_clamped + 2, yy, c) +
               wx3 * sample_source(base_x_clamped + 3, yy, c);
    };

    Halide::Expr row0 = sample_row(base_y_clamped + 0, w0x, w1x, w2x, w3x);
    Halide::Expr row1 = sample_row(base_y_clamped + 1, w0x, w1x, w2x, w3x);
    Halide::Expr row2 = sample_row(base_y_clamped + 2, w0x, w1x, w2x, w3x);
    Halide::Expr row3 = sample_row(base_y_clamped + 3, w0x, w1x, w2x, w3x);

    return w0y * row0 + w1y * row1 + w2y * row2 + w3y * row3;
}

}  // namespace
