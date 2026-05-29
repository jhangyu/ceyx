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
// ---------------------------------------------------------------------------

#include "Halide.h"

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

}  // namespace
