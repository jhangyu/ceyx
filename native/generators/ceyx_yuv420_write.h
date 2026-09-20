#ifndef CEYX_YUV420_WRITE_H
#define CEYX_YUV420_WRITE_H

// The ONE yuv420 plane-write implementation, shared by every Stage-4 arm.
//
// WHY THIS FILE EXISTS (mem8 v3, T12)
// ------------------------------------
// The campaign ships yuv420 output on two kernel shapes: the fused
// Bayer-demosaic+render kernel (arm A) and the two-stage Stage-4 kernels in
// both backend families (arm B). The plan is explicit that the arms may differ
// in WHERE the RGB comes from and must NOT differ in HOW yuv420 is written --
// a per-arm or per-backend siting/rounding divergence is exactly the defect
// test Y7 exists to catch, and the standing no-divergence user ruling forbids
// it outright. So the plane math lives here once and every arm calls it.
//
// THE ARITHMETIC IS NOT DEFINED HERE. Every constant comes from
// native/include/ceyx_yuv420_oracle.h, which was transcribed from the
// libjpeg-turbo actually vendored on this tree. This file builds the Halide
// expressions from those symbols; it never spells a coefficient. That is the
// point of the oracle: the kernels, T13's host converter and the tests derive
// from one definition rather than three hand-copied coefficient sets.
//
// THREE THINGS HERE ARE EASY TO GET WRONG AND ARE DELIBERATE
// -----------------------------------------------------------
// 1. Luma and chroma round DIFFERENTLY (kLumaRounding vs kChromaRounding, the
//    latter carrying libjpeg's -1 epsilon). They are separate symbols in the
//    oracle for this reason.
// 2. Chroma is converted PER PIXEL and then box-averaged -- libjpeg runs
//    jccolor before jcsample. Averaging RGB and converting once is a different
//    function, differing by exactly the rounding the fidelity bound measures.
// 3. The 2x2 bias alternates 1,2,1,2 across OUTPUT COLUMNS, restarting each
//    row (oracle chroma_downsample_bias). It is not (a+b+c+d+2)>>2.
//
// ODD EXTENTS: the second source column/row of an edge 2x2 block is clamped to
// the last valid index, which replicates libjpeg's expand_right_edge before
// h2v2_downsample. Plane extents are ceil(w/2) x ceil(h/2) by the T12.0 frozen
// contract; the caller allocates them, this file only produces the values.
//
// LAYOUT IS NOT DECIDED HERE either: three separate 2-D Funcs are produced and
// the HOST computes the three plane offsets into one contiguous destination
// (shape 1). The kernel decides nothing about layout -- the discipline that
// made the fused-orientation work lower correctly on Vulkan after two
// in-kernel formulations mis-lowered.
//
// COST NOTE, stated rather than hidden: the colour body is evaluated once for
// the luma plane and once more per contributing pixel for the chroma planes,
// i.e. ~2x per output pixel. Staging an intermediate to avoid that is barred
// on Vulkan (a runtime-affine gather makes the staged extent dynamic, which
// Vulkan below v1.3 refuses -- T20's finding), so this is the price of the
// no-divergence shape, not an oversight.

#include "Halide.h"

#include "ceyx_yuv420_oracle.h"

namespace ceyx {

// `rgb8_r` / `rgb8_g` / `rgb8_b` are THREE SEPARATE single-value Funcs over
// (x, y), each producing the display-referred R, G or B at OUTPUT coordinate
// (x, y), orientation already applied.
//
// WHY THREE FUNCS AND NOT ONE 3-TUPLE FUNC (2026-09-20, D2 root cause)
// ---------------------------------------------------------------------
// This took a 3-Tuple Func until the tier-1 on-device Vulkan run. The split
// Stage-4 family exists precisely to avoid Vulkan Tuple codegen
// (halide_aot.cmake:86-92), and its rgba8 sibling never forms a Tuple -- but
// the split yuv420 class wrapped its three channel Exprs in a Tuple Func
// purely to match this signature, then this file consumed it by tuple index at
// FIVE coordinates per output pixel. On device that produced planes
// disagreeing with the same device's own rgba8 on 12-33% of pixels, with a
// bimodal error histogram (a one-sided +1 population and a gross population
// separated by a hard gap at d=3..8) -- not the shape float-strictness noise
// makes. Taking the channels un-Tupled conforms the yuv420 classes to the
// split family's own documented design rule. It is NOT a platform guard: both
// families pass three Funcs through this one signature, so the arms still
// cannot diverge in HOW yuv420 is written, which is the whole point of the
// file.
//
// luma_width / luma_height are the ORIENTED output extents, used only to clamp
// the odd-extent edge block.
// PlaneOut is templated only because a generator's `Output<Buffer<uint8_t>>` is
// not a `Halide::Func` and does not convert to one; it is definable with
// operator() exactly like a Func, which is all this function needs. One body,
// every caller.
template <typename PlaneOut>
void build_yuv420_planes(Halide::Var x, Halide::Var y,
                         Halide::Func rgb8_r,
                         Halide::Func rgb8_g,
                         Halide::Func rgb8_b,
                         Halide::Expr luma_width,
                         Halide::Expr luma_height,
                         PlaneOut &y_plane,
                         PlaneOut &cb_plane,
                         PlaneOut &cr_plane) {
    using namespace Halide;
    namespace o = ceyx::yuv420;

    // `index` is a C++ int, resolved at generator time -- this selects one of
    // three Funcs, it does not emit a select() into the pipeline.
    auto ch = [&](Expr px, Expr py, int index) {
        Expr v;
        if (index == 0) {
            v = rgb8_r(px, py);
        } else if (index == 1) {
            v = rgb8_g(px, py);
        } else {
            v = rgb8_b(px, py);
        }
        return cast<int32_t>(v);
    };

    // --- luma, one sample per output pixel ---------------------------------
    {
        Expr r = ch(x, y, 0), g = ch(x, y, 1), b = ch(x, y, 2);
        y_plane(x, y) = cast<uint8_t>(
            (o::kRToY * r + o::kGToY * g + o::kBToY * b + o::kLumaRounding) >>
            o::kScaleBits);
    }

    // --- chroma: convert per pixel, then box-average the 2x2 ---------------
    // Same four source coordinates for Cb and Cr, and the same bias, so a
    // divergence between the two planes is impossible by construction.
    Expr sx0 = 2 * x;
    Expr sx1 = min(2 * x + 1, luma_width - 1);
    Expr sy0 = 2 * y;
    Expr sy1 = min(2 * y + 1, luma_height - 1);
    Expr bias = 1 + (x % 2);  // oracle chroma_downsample_bias, as an Expr

    auto chroma_sample = [&](Expr px, Expr py, int32_t r_coeff, int32_t g_coeff,
                             int32_t b_coeff) {
        // Cast to uint8 FIRST, exactly as libjpeg does: jccolor writes 8-bit
        // samples and jcsample averages those, not the wider intermediates.
        return cast<int32_t>(cast<uint8_t>(
            (r_coeff * ch(px, py, 0) + g_coeff * ch(px, py, 1) +
             b_coeff * ch(px, py, 2) + o::kChromaRounding) >>
            o::kScaleBits));
    };

    auto box = [&](int32_t r_coeff, int32_t g_coeff, int32_t b_coeff) {
        return cast<uint8_t>(
            (chroma_sample(sx0, sy0, r_coeff, g_coeff, b_coeff) +
             chroma_sample(sx1, sy0, r_coeff, g_coeff, b_coeff) +
             chroma_sample(sx0, sy1, r_coeff, g_coeff, b_coeff) +
             chroma_sample(sx1, sy1, r_coeff, g_coeff, b_coeff) + bias) >>
            2);
    };

    cb_plane(x, y) = box(o::kRToCb, o::kGToCb, o::kBToCb);
    cr_plane(x, y) = box(o::kRToCr, o::kGToCr, o::kBToCr);
}

}  // namespace ceyx

#endif  // CEYX_YUV420_WRITE_H
