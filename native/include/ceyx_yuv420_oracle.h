#pragma once

// ============================================================================
// mem8 v3 T12/T13 — THE yuv420 conversion oracle, in ONE place (SR-11, R-K).
//
// The plan's §3 obligation is "ONE oracle, BOTH backends": the Metal arm, the
// Vulkan arm and T13's host converter are three implementations of a single
// definition, and there is no cross-implementation equivalence test in this
// tree that would catch a silent drift between three hand-copied coefficient
// sets. So the constants live here exactly once and every implementation
// derives its arithmetic from these symbols rather than from a literal.
//
// PROVENANCE — read from the libjpeg-turbo actually vendored on this tree, not
// from memory (project rule: never write a third-party API/constant from
// training memory). Paths and lines, re-read 2026-09-20:
//
//   native/third_party/libjpeg-turbo/src/jccolor.c:45-47
//       Y  =  0.29900*R + 0.58700*G + 0.11400*B
//       Cb = -0.16874*R - 0.33126*G + 0.50000*B + CENTERJSAMPLE
//       Cr =  0.50000*R - 0.41869*G - 0.08131*B + CENTERJSAMPLE
//   native/third_party/libjpeg-turbo/src/jccolor.c:68-71
//       SCALEBITS 16, CBCR_OFFSET (CENTERJSAMPLE<<16), ONE_HALF (1<<15),
//       FIX(x) ((x)*(1<<16)+0.5)
//   native/third_party/libjpeg-turbo/src/jccolor.c:227-241
//       the table construction, including the two DIFFERENT rounding terms:
//       Y adds ONE_HALF, while Cb/Cr add CBCR_OFFSET + ONE_HALF - 1. That
//       "- 1" is libjpeg's 0.5-epsilon fudge, documented at :233-236 as what
//       keeps the maximum output rounding to MAXJSAMPLE instead of
//       MAXJSAMPLE+1 -- i.e. it is what makes range-limiting unnecessary.
//       Dropping it is a silent off-by-one at the top of the range.
//   native/third_party/libjpeg-turbo/src/jcsample.c:284-289  (h2v2_downsample)
//       the 2x2 chroma box average is NOT a plain (a+b+c+d+2)>>2: the bias
//       alternates 1,2,1,2 across output columns and RESTARTS at 1 on every
//       row. Reproduced here rather than approximated, because approximating
//       it costs a systematic +-1 LSB that looks exactly like a coefficient
//       error when a fidelity bound is missed.
//
// WHY THE ENCODER IS THE ORACLE: encode_ffi_api.cpp feeds JCS_EXT_RGBA rows
// into jpeg_set_defaults() + jpeg_write_scanlines, i.e. libjpeg's own
// RGB->YCbCr with JFIF defaults. Full-range BT.601, centered chroma. If the
// decode path used a different convention, a decode->display round trip and a
// decode->encode round trip would disagree, and nothing in the tree would say so.
//
// ORDER OF OPERATIONS IS PART OF THE ORACLE: libjpeg converts EVERY pixel to
// Cb/Cr first and downsamples the CHROMA (jcsample runs after jccolor). It
// does not average RGB and convert once. The two differ only by rounding, but
// "only by rounding" is precisely the size of the bound Y4/Y7 assert against.
// ============================================================================

#include <stdint.h>

namespace ceyx {
namespace yuv420 {

// libjpeg's fixed-point scale. Named, not spelled 16, so a reader who changes
// one of the three uses cannot leave the other two behind.
inline constexpr int kScaleBits = 16;
inline constexpr int32_t kOneHalf = static_cast<int32_t>(1) << (kScaleBits - 1);
inline constexpr int32_t kCenterSample = 128;
inline constexpr int32_t kCbCrOffset = kCenterSample << kScaleBits;

// FIX(x) from jccolor.c:71, evaluated at compile time. constexpr double
// arithmetic is exact enough here: each value is a single rounding of a
// literal, which is what libjpeg's macro does at C compile time too.
inline constexpr int32_t fix(double coefficient) {
    return static_cast<int32_t>(coefficient * (1L << kScaleBits) + 0.5);
}

inline constexpr int32_t kRToY = fix(0.29900);
inline constexpr int32_t kGToY = fix(0.58700);
inline constexpr int32_t kBToY = fix(0.11400);
inline constexpr int32_t kRToCb = -fix(0.16874);
inline constexpr int32_t kGToCb = -fix(0.33126);
inline constexpr int32_t kBToCb = fix(0.50000);
inline constexpr int32_t kRToCr = fix(0.50000);
inline constexpr int32_t kGToCr = -fix(0.41869);
inline constexpr int32_t kBToCr = -fix(0.08131);

// The two rounding terms, deliberately separate symbols because they are NOT
// the same value (jccolor.c:230 vs :237).
inline constexpr int32_t kLumaRounding = kOneHalf;
inline constexpr int32_t kChromaRounding = kCbCrOffset + kOneHalf - 1;

// Scalar forms. The kernels build the identical expressions in Halide from the
// same constants; these are what T13's host converter and the tests' oracle use.
inline uint8_t luma_from_rgb(int32_t r, int32_t g, int32_t b) {
    return static_cast<uint8_t>(
        (kRToY * r + kGToY * g + kBToY * b + kLumaRounding) >> kScaleBits);
}

inline uint8_t cb_from_rgb(int32_t r, int32_t g, int32_t b) {
    return static_cast<uint8_t>(
        (kRToCb * r + kGToCb * g + kBToCb * b + kChromaRounding) >> kScaleBits);
}

inline uint8_t cr_from_rgb(int32_t r, int32_t g, int32_t b) {
    return static_cast<uint8_t>(
        (kRToCr * r + kGToCr * g + kBToCr * b + kChromaRounding) >> kScaleBits);
}

// jcsample.c:284-289's alternating bias, as a function of the OUTPUT column.
// bias starts at 1 for column 0 and alternates 1,2,1,2...; it restarts every
// row, so it depends on the output column alone.
inline constexpr int32_t chroma_downsample_bias(int32_t output_column) {
    return 1 + (output_column & 1);
}

// The 2x2 box average libjpeg applies to already-converted chroma samples.
inline uint8_t box_average_2x2(int32_t top_left, int32_t top_right,
                               int32_t bottom_left, int32_t bottom_right,
                               int32_t output_column) {
    return static_cast<uint8_t>(
        (top_left + top_right + bottom_left + bottom_right +
         chroma_downsample_bias(output_column)) >> 2);
}

// Plane geometry, from the T12.0 frozen contract (raw_ffi_api.h). Expressed
// once so nobody re-derives ceil(w/2) with an integer division that silently
// truncates -- the odd-dimension case Y3/Y9 exist to catch.
inline constexpr int32_t chroma_extent(int32_t luma_extent) {
    return (luma_extent + 1) / 2;
}

}  // namespace yuv420
}  // namespace ceyx
