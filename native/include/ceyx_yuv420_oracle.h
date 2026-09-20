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

// ---------------------------------------------------------------------------
// THE INVERSE (T12 milestone 3 / T13): YCbCr -> RGB, for ceyx_yuv420_to_rgba8.
//
// It lives beside the forward direction for the reason the file header gives:
// one definition, every implementation derives from it. A separately written
// inverse is exactly the silent drift this header exists to prevent -- and an
// inverse that is not libjpeg's would make a decode->display round trip and a
// decode->encode round trip disagree with nothing in the tree saying so.
//
// PROVENANCE, read from the vendored source on this tree (not from memory):
//   native/third_party/libjpeg-turbo/src/jdcolor.c:236-250 (build_ycc_rgb_table)
//       Cr=>R  = (FIX(1.40200) * x + ONE_HALF) >> SCALEBITS
//       Cb=>B  = (FIX(1.77200) * x + ONE_HALF) >> SCALEBITS
//       Cr=>G  = -FIX(0.71414) * x                 (kept SCALED)
//       Cb=>G  = -FIX(0.34414) * x + ONE_HALF      (kept SCALED, ONE_HALF
//                                                   pre-added so the inner
//                                                   loop need not)
//       where x = sample - CENTERJSAMPLE.
//   native/third_party/libjpeg-turbo/src/jdcolext.c:61-65 (the inner loop)
//       R = range_limit[y + Cr_r]
//       G = range_limit[y + ((Cb_g + Cr_g) >> SCALEBITS)]
//       B = range_limit[y + Cb_b]
//
// TWO THINGS THAT ARE EASY TO GET WRONG AND ARE THEREFORE SPELLED OUT:
//   1. The red and blue terms are shifted when the TABLE is built; the green
//      term is NOT -- its two contributions are summed at full scale and
//      shifted ONCE. Shifting each green contribution separately is a
//      different function (double rounding) and differs by an LSB.
//   2. ONE_HALF is added to the Cb=>G table entry, never to the Cr=>G one. It
//      appears exactly once in the green sum, which is what the single shift
//      above needs. Adding it to both, or to neither, is a systematic bias.
//
// RANGE LIMITING: libjpeg indexes a sample_range_limit TABLE rather than
// clamping, because DCT noise can push `y + delta` outside 0..MAXJSAMPLE. Over
// the range reachable from 8-bit inputs that table IS a clamp to 0..255, and
// this path has no DCT noise (its input is our own kernel's output), so a
// clamp is the faithful reading here rather than an approximation of one.
inline constexpr int32_t kCrToR = fix(1.40200);
inline constexpr int32_t kCbToB = fix(1.77200);
inline constexpr int32_t kCrToG = -fix(0.71414);
inline constexpr int32_t kCbToG = -fix(0.34414);

inline uint8_t clamp_to_sample(int32_t value) {
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

// `cb`/`cr` are the stored 0..255 samples; the centering is applied here so no
// caller has to remember it.
inline void rgb_from_ycbcr(int32_t y, int32_t cb, int32_t cr, uint8_t *out_r,
                           uint8_t *out_g, uint8_t *out_b) {
    const int32_t cb_centered = cb - kCenterSample;
    const int32_t cr_centered = cr - kCenterSample;
    const int32_t r_term = (kCrToR * cr_centered + kOneHalf) >> kScaleBits;
    const int32_t b_term = (kCbToB * cb_centered + kOneHalf) >> kScaleBits;
    // Scaled sum, ONE shift, ONE_HALF added exactly once -- see note 1 and 2.
    const int32_t g_term =
        (kCbToG * cb_centered + kOneHalf + kCrToG * cr_centered) >> kScaleBits;
    *out_r = clamp_to_sample(y + r_term);
    *out_g = clamp_to_sample(y + g_term);
    *out_b = clamp_to_sample(y + b_term);
}

// Plane geometry, from the T12.0 frozen contract (raw_ffi_api.h). Expressed
// once so nobody re-derives ceil(w/2) with an integer division that silently
// truncates -- the odd-dimension case Y3/Y9 exist to catch.
inline constexpr int32_t chroma_extent(int32_t luma_extent) {
    return (luma_extent + 1) / 2;
}

}  // namespace yuv420
}  // namespace ceyx
