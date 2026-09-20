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

#ifdef CEYX_DBGVK_WIDE32
// Scratch-probe marker (Task #9). Arbitrary, but must be a value the real luma
// arithmetic cannot produce, so a degenerate plane is distinguishable from a
// real one. Not present in any shipping build.
inline constexpr int32_t kDbgWideMarker = 0x5A5A5A;
#endif

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

        // THE LUMA DOT PRODUCT IS SUMMED IN SPLIT HIGH/LOW HALVES. This is
        // ADOPTED ON MEASUREMENT ALONE (2026-09-20): on device it eliminates
        // the "+1" error population that four rounds of downstream fixes could
        // not touch. THE MECHANISM IS NOT ESTABLISHED. Do not add one to this
        // comment without device evidence.
        //
        // Not a precision choice and not a platform guard: both arms and both
        // backends get this identical body, so the arms cannot diverge in HOW
        // yuv420 is written.
        //
        // THE EVIDENCE, and all that is claimed. Two builds differing ONLY in
        // this expression, same shipping configuration, same device, md5 of the
        // device pair verified both directions, V-checks on record:
        //   Y |kernel-control| == 1 population, per route
        //     HEAD form   3110547 / 11198070 / 15429072   (dbg-vf-24, verdict
        //                                                  dbg-vf-25)
        //     this form        32 /        0 /        0   (dbg-vf-17, verdict
        //                                                  dbg-vf-18)
        //     routes in order: bayer-arw, xtrans-raf, linearrgb-x3f.
        // The residual 32 on bayer-arw is TWO-SIDED (15 above, 17 below) and
        // bayer-arw alone takes its control from the FUSED kernel, whose 1-LSB
        // cross-kernel residue is a closed non-defect. The "+1" defect was
        // strictly one-sided. If bayer's residual is ever shown one-sided or
        // growing, that attribution is falsified and this reopens.
        //
        // WHAT THIS DOES NOT FIX. A SECOND, INDEPENDENT defect remains and is
        // tracked separately: a gross (>=9) error population, invariant under
        // the luma expression -- the mid-gross bucket is bit-identical, count
        // for count, between the two builds above (1187898 / 6483171 / 1149874
        // in BOTH), so it is not produced by this arithmetic. Byte-exactness of
        // the Y plane against the host control is therefore still RED on
        // Vulkan, for a reason that is not this expression. Metal is exact.
        //
        // WHY EARLIER ATTEMPTS FAILED, as a do-not-retry list -- each refuted on
        // its own device evidence, each with the identical affected population:
        // clamping or casting before the store (dbg-vk2-13, t12d5-09-verdicts;
        // the value is already wrong upstream of both), unsigned shift
        // (dbg-vk2-44), int64 accumulation (dbg-vk2-94, the widening provably
        // in the SPIR-V), fp32 evaluation (t12e2-27, the float constants
        // provably in the SPIR-V).
        //
        // A PREVIOUS VERSION OF THIS COMMENT ARGUED that the split works
        // because no intermediate it forms reaches 2^23. THAT ARGUMENT IS
        // FALSIFIED, and it is recorded here so it is not re-derived: this same
        // split was measured pre-cast through the wide32 instrument and was
        // REFUTED there (t12e2-38), and chroma's coefficients produce
        // intermediates above 2^23 while chroma is byte-perfect. Staying under
        // 2^23 is neither necessary nor sufficient.
        //
        // The identity below is nonetheless exact, which is why adopting it is
        // safe regardless of mechanism: with ti = 65536*hi + li, 0 <= li <
        // 65536 (every ti is non-negative, so >> and & are exactly quotient and
        // remainder), and 2*tg = 38470*g,
        //     S = tr + 2*tg + tb + 32768 = 65536*H + L,  L >= 0
        // hence floor(S/65536) = H + (L >> 16), which is what the oracle
        // computes, for every input. Bounds: tr <= 4996725, tg <= 4904925,
        // tb <= 1905105, L <= 294908, H <= 253, result <= 255 -- so the
        // unclamped uint8 store below cannot overflow. The pre-registered
        // content check that the simplifier does not refold the split back into
        // a single dot product is native/tests/tmp/t12e2-30-runC-prereg.txt
        // (%int_19235 present / 38470 absent; re-confirmed for the shipping
        // build in dbg-vf-18 V2a).
        static_assert(o::kGToY % 2 == 0,
                      "the split below halves kGToY exactly; an odd "
                      "coefficient voids the exactness proof");
        Expr tr = Expr(o::kRToY) * r;
        Expr tg = Expr(o::kGToY / 2) * g;
        Expr tb = Expr(o::kBToY) * b;
        Expr scale_mask = Expr((1 << o::kScaleBits) - 1);
        Expr luma_hi = (tr >> o::kScaleBits) + 2 * (tg >> o::kScaleBits) +
                       (tb >> o::kScaleBits);
        Expr luma_lo = (tr & scale_mask) + 2 * (tg & scale_mask) +
                       (tb & scale_mask) + Expr(o::kLumaRounding);
        Expr luma_scaled = luma_hi + (luma_lo >> o::kScaleBits);

#ifdef CEYX_DBGVK_WIDE32
        // DBG-VK2 SCRATCH PROBE. Compile-time inert: no build configuration
        // defines CEYX_DBGVK_WIDE32, and it is retained deliberately because it
        // is the only instrument that can read this kernel's pre-cast value --
        // the uint8 store destroys it -- and the defect-#2 investigation is
        // still open.
        // Row 0 carries a fixed marker so a degenerate all-zero plane cannot be
        // mistaken for a clean result.
        //
        // CEYX_DBGVK_INTFORM re-publishes the OLD integer form through this
        // same instrument. It is the negative control: on device it must still
        // read S - 2^24 for S >= 2^23. Without it, "the float form reads
        // clean" cannot be distinguished from "I changed the instrument and it
        // stopped being able to see the defect".
#ifdef CEYX_DBGVK_INTFORM
        Expr dbg_val = cast<int32_t>(
            (o::kRToY * r + o::kGToY * g + o::kBToY * b + o::kLumaRounding) >>
            o::kScaleBits);
#else
        Expr dbg_val = cast<int32_t>(luma_scaled);
#endif
        y_plane(x, y) = select(y == 0, Expr(kDbgWideMarker), dbg_val);
#else
        // No clamp: luma_scaled is provably in [0,255] (see the bounds above),
        // and a clamp here is a refuted route (dbg-vk2-13, t12d5-09-verdicts).
        // This is HEAD's store idiom (t12e-01-head-write-h.txt:115-117).
        y_plane(x, y) = cast<uint8_t>(luma_scaled);
#endif
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

#ifdef CEYX_DBGVK_WIDE32
    (void)box;
    cb_plane(x, y) = Expr(kDbgWideMarker);
    cr_plane(x, y) = Expr(kDbgWideMarker);
#else
    cb_plane(x, y) = box(o::kRToCb, o::kGToCb, o::kBToCb);
    cr_plane(x, y) = box(o::kRToCr, o::kGToCr, o::kBToCr);
#endif
}

}  // namespace ceyx

#endif  // CEYX_YUV420_WRITE_H
