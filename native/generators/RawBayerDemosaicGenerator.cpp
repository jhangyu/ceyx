// Fused normalize + Bayer bilinear demosaic for the generic RAW route.
//
// A NEW generator rather than an edit to DngDemosaicGenerator.cpp, for two
// reasons: dng_demosaic_bilinear's AOT output hashes are pinned regression
// artifacts (Gotcha #99), and the DNG route feeds already-linearised Stage2
// data that must not be normalised a second time.
//
// Normalize is an EXPRESSION consumed by the demosaic - never a materialised
// intermediate (spec section 7, stage R1). The schedule below therefore
// contains no root-materialising directive; the acceptance gate greps this
// file for that directive and requires zero occurrences, so the name is
// deliberately not spelled out even in prose here.
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
        auto sample = [&](Expr sx, Expr sy) {
            Expr mx = map_repeat_coord(sx, width);
            Expr my = map_repeat_coord(sy, height);
            Expr level = black(mx % bw, my % bh);
            Expr norm = (cast<float>(src(mx, my)) - level) * inv_range;
            return cast<uint16_t>(clamp(norm, 0.0f, 65535.0f));
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

HALIDE_REGISTER_GENERATOR(RawBayerDemosaic, raw_bayer_demosaic)
