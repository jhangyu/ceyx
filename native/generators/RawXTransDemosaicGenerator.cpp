// Fused normalize + X-Trans 6x6 demosaic for the generic RAW route.
//
// Phase 17 ships a bounded, preview-grade interpolation, NOT a port of the
// LibRaw/dcraw Markesteijn framework (spec section W3-01). The formula:
//   own channel   -> the normalized sample itself
//   other channel -> weighted mean of the 5x5 neighbours of that colour,
//                    w(i,j) = 1 / (1 + i*i + j*j)
// The 5x5 window guarantees at least one site of every colour for all 36
// phases (shift positions) of the canonical Fujifilm tile, which is what
// makes the algorithm total; the test asserts that property directly rather
// than trusting it.
//
// The CPU reference in src/raw_demosaic_reference.cpp is a literal
// transcription of the same formula, so the >=99 dB / max_abs<=1 gate
// measures codegen, not algorithm choice.
//
// Normalize is an EXPRESSION consumed by the demosaic - never a materialised
// intermediate (spec section 7, stage R1). The schedule below therefore
// contains no root-materialising directive; the acceptance gate greps this
// file for that directive and requires zero occurrences, so the name is
// deliberately not spelled out even in prose here.
//
// The output is a plain 3-D buffer indexed by channel, never a multi-value
// (tupled) Func: Halide v21's SPIR-V R==G defect makes that a Vulkan dead end
// (see the Stage4 split-kernel notes). The acceptance gate greps this file for
// that type's name and requires zero occurrences, so - as with the
// root-materialising directive above - the name is deliberately not spelled
// out even in prose here.
#include "Halide.h"
#include "dng_halide_utils.h"

using namespace Halide;

class RawXTransDemosaic : public Halide::Generator<RawXTransDemosaic> {
public:
    Input<Buffer<uint16_t>> src{"src", 2};   // x, y mosaic
    // Runtime, not a compile-time parameter: the 6x6 arrangement is per-file
    // data (validated against the canonical tile's phase family by
    // src/raw_contract_validate.cpp), exactly like red_x/red_y on the Bayer
    // kernel. One AOT kernel serves every camera.
    Input<Buffer<int32_t>> cfa{"cfa", 2};    // 6x6 RawColorKey values
    Input<Buffer<float>> black{"black", 2};  // black repeat tile
    Input<float> inv_range{"inv_range"};     // 65535 / (white - black_max)
    Output<Buffer<uint16_t>> dst{"dst", 3};  // x, y, RGB interleaved

    void generate() {
        Var x("x"), y("y"), c("c");

        src.dim(0).set_stride(1);
        cfa.dim(0).set_bounds(0, 6);
        cfa.dim(1).set_bounds(0, 6);
        dst.dim(0).set_stride(3);
        dst.dim(2).set_bounds(0, 3);
        dst.dim(2).set_stride(1);

        Expr width = src.dim(0).extent();
        Expr height = src.dim(1).extent();
        Expr bw = max(black.dim(0).extent(), 1);
        Expr bh = max(black.dim(1).extent(), 1);

        // R1 fused into R2: one expression, no intermediate frame.
        // The repeat-6 edge rule lands out-of-range samples on a site of the
        // same X-Trans phase, so the colour read below stays consistent with
        // the sample read here.
        auto norm = [&](Expr sx, Expr sy) {
            Expr mx = map_repeat_coord_n(sx, width, 6);
            Expr my = map_repeat_coord_n(sy, height, 6);
            Expr level = black(mx % bw, my % bh);
            Expr v = (cast<float>(src(mx, my)) - level) * inv_range;
            return clamp(v, 0.0f, 65535.0f);
        };

        // RawColorKey: Red=0, Green=1, Blue=2 (raw_pipeline_contract.h), which
        // is also the dst channel order, so `key == c` is the own-channel test.
        auto key_at = [&](Expr sx, Expr sy) {
            Expr mx = map_repeat_coord_n(sx, width, 6);
            Expr my = map_repeat_coord_n(sy, height, 6);
            return cfa(mx % 6, my % 6);
        };

        Expr own = key_at(x, y);
        Expr center = norm(x, y);

        // 5x5 weighted mean of same-colour neighbours. Non-matching sites
        // contribute a literal zero weight (rather than being dropped), so the
        // summation order matches the scalar reference term for term.
        Expr weighted_sum = 0.0f;
        Expr weight_sum = 0.0f;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                Expr k = key_at(x + dx, y + dy);
                Expr w = select(k == c, 1.0f / (1.0f + float(dx * dx + dy * dy)), 0.0f);
                weighted_sum += w * norm(x + dx, y + dy);
                weight_sum += w;
            }
        }
        Expr interpolated = weighted_sum / max(weight_sum, 1e-6f);

        Expr value = select(own == c, center, interpolated);
        dst(x, y, c) = cast<uint16_t>(clamp(floor(value + 0.5f), 0.0f, 65535.0f));
    }

    void schedule() {
        Var x("x"), y("y"), c("c");

        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            dst.bound(c, 0, 3)
               .reorder(c, x, y)
               .gpu_tile(x, y, xo, yo, xi, yi, 16, 16)
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

HALIDE_REGISTER_GENERATOR(RawXTransDemosaic, raw_xtrans_demosaic)
