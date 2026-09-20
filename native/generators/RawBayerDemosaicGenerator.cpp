// Fused normalize + Bayer bilinear demosaic for the generic RAW route.
//
// A NEW generator rather than an edit to DngDemosaicGenerator.cpp, for two
// reasons: dng_demosaic_bilinear's AOT output hashes are pinned regression
// artifacts (Gotcha #99), and the DNG route feeds already-linearised Stage2
// data that must not be normalised a second time.
//
// NORMALIZE MUST NOT BE MATERIALISED AT ROOT. The schedule below contains no
// root-materialising directive: `normalized` is staged per GPU TILE, which is
// a different thing from a full-frame intermediate.
//
// This rule was NARROWED twice, and both narrowings are load-bearing:
//
// 1. The reason is ELEMENT WIDTH, not compute_at. The older wording here
//    forbade any materialised producer and claimed "the acceptance gate greps
//    this file for that directive and requires zero occurrences". No such gate
//    ever existed anywhere in this tree -- the claim was prose describing an
//    enforcer that was never written. T22 then measured the actual variable on
//    Adreno 750 / Vulkan: a Func materialised at a GPU loop level becomes an
//    array in the Workgroup storage class, and THAT driver miscompiles the
//    array when its element type is narrower than 32 bits. uint8 gives
//    24,000,000/24,000,000 wrong G/B; uint32 gives zero. Hence the deliberate
//    uint32_t at the `normalized` definition below -- see its own comment, and
//    do not narrow it back.
//
// 2. There IS an enforcer now, and it is mechanical:
//    native/tests/check_gpu_producer_width.py reads the LOWERED STATEMENT
//    (-e stmt) of an AOT target, finds every GPU dispatch, extracts the
//    shared-memory argument (which Halide emits as element_count *
//    element_width_bytes) and fails on any staged producer narrower than 32
//    bits. It keys on that argument rather than on the kernel source because
//    that is the only form textually identical on BOTH backend families: the
//    Metal arm embeds readable Metal C++, the Vulkan arm embeds SPIR-V where
//    the type is an opaque id. It enumerates whatever the statement actually
//    stages, with no symbol allowlist, so producers added in future are
//    covered without editing it. It has been observed FAILING on an injected
//    uint32->uint16 narrowing on both backends.
//
// WHAT THE RULE DOES NOT FORBID, as of mem8 v3 T20: fusing this demosaic into
// the Stage-4 render so no Stage-3 frame is ever allocated. That is
// RawBayerFusedRenderGenerator.cpp, which reuses the same expression via
// build_demosaic_expr(). Note it stages NOTHING at all -- not because of this
// rule, but because its render gathers through a runtime orientation affine,
// which makes any staged extent dynamic, and Vulkan below v1.3 refuses a
// dynamic workgroup size. That constraint and this one are independent.
#include "Halide.h"
#include "dng_halide_utils.h"

using namespace Halide;

class RawBayerDemosaic : public Halide::Generator<RawBayerDemosaic> {
public:
    Input<Buffer<uint16_t>> src{"src", 2};      // x, y CFA mosaic
    // Runtime, not GeneratorParams: the AOT kernel compiles once but CFAPattern
    // and black/white vary per file.
    Input<int32_t> red_x{"red_x"};
    Input<int32_t> red_y{"red_y"};
    Input<Buffer<float>> black{"black", 2};     // black repeat tile
    Input<float> inv_range{"inv_range"};        // 65535 / (white - black_max)
    Output<Buffer<uint16_t>> dst{"dst", 3};     // x, y, RGB interleaved

    // L3 (spec-b sec4.3): the normalized mosaic staged as its own producer so
    // the 9 demosaic taps share one normalize per source pixel instead of
    // each independently re-running map_repeat_coord + black lookup + scale.
    // Stays an EXPRESSION-level producer scheduled compute_at the GPU tile --
    // never root -- per this file's acceptance gate (see header comment).
    Func normalized{"normalized"};

    void generate() {
        Var x("x"), y("y"), c("c");

        src.dim(0).set_stride(1);
        dst.dim(0).set_stride(3);
        dst.dim(2).set_bounds(0, 3);
        dst.dim(2).set_stride(1);

        Expr width = src.dim(0).extent();
        Expr height = src.dim(1).extent();
        Expr bw = max(black.dim(0).extent(), 1);
        Expr bh = max(black.dim(1).extent(), 1);

        // R1 fused into R2: one expression, no intermediate frame.
        {
            Expr mx = map_repeat_coord(x, width);
            Expr my = map_repeat_coord(y, height);
            Expr level = black(mx % bw, my % bh);
            Expr norm = (cast<float>(src(mx, my)) - level) * inv_range;
            // Stored 32-bit on purpose. The natural type here is uint16_t, but
            // a 16-bit array in the GPU workgroup storage class is miscompiled
            // by the Adreno Vulkan driver: the halo ring of the staged tile
            // comes back carrying another workgroup's values, which shows up as
            // wrong tile-border pixels in every channel except the one whose
            // only tap is the centre. It is NOT a missing barrier: the emitted
            // SPIR-V has an unguarded producer store followed, in uniform
            // control flow, by OpControlBarrier with Workgroup execution and
            // memory scope and AcquireRelease|WorkgroupMemory semantics, ahead
            // of every shared read, and spirv-val is clean. The one thing the
            // Metal arm never exercises is a 16-bit array in the Workgroup
            // storage class, and widening it to 32-bit makes the device gate
            // (Adreno 750, 6048x4024 + tail geometries + all four CFA phases)
            // pass. Do not narrow this back to uint16_t.
            // Widening is bit-exact: the value is already clamped to
            // [0, 65535], so the narrowing cast in `sample` below recovers
            // exactly the uint16_t that used to be stored.
            normalized(x, y) = cast<uint32_t>(clamp(norm, 0.0f, 65535.0f));
        }

        auto sample = [&](Expr sx, Expr sy) {
            return cast<uint16_t>(normalized(sx, sy));
        };

        dst(x, y, c) = build_demosaic_expr(x, y, c, sample, red_x, red_y);
    }

    void schedule() {
        Var x("x"), y("y"), c("c");

        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            dst.bound(c, 0, 3)
               .reorder(c, x, y)
               .gpu_tile(x, y, xo, yo, xi, yi, 16, 16)
               .unroll(c);
            normalized.compute_at(dst, xo)
                      .gpu_threads(x, y);
        } else {
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, 3)
               .reorder(c, x, y)
               .split(y, yo, yi, 32)
               .parallel(yo)
               .vectorize(x, 8)
               .unroll(c);
            normalized.compute_at(dst, yo)
                      .vectorize(x, 8);
        }
    }
};

HALIDE_REGISTER_GENERATOR(RawBayerDemosaic, raw_bayer_demosaic)
