#include "Halide.h"
#include "dng_halide_utils.h"

using namespace Halide;

class DngDemosaicBilinear : public Halide::Generator<DngDemosaicBilinear> {
public:
    Input<Buffer<uint16_t>> src{"src", 2};  // x, y Bayer CFA
    // CFA phase: parity of the red site's column / row (see
    // dng_halide_utils.h::build_demosaic_expr). Runtime inputs because the
    // AOT kernel is compiled once but CFAPattern varies per DNG file.
    Input<int32_t> red_x{"red_x"};
    Input<int32_t> red_y{"red_y"};
    Output<Buffer<uint16_t>> dst{"dst", 3}; // x, y, RGB channel

    void generate() {
        Var x("x"), y("y"), c("c");

        src.dim(0).set_stride(1);
        dst.dim(0).set_stride(3);
        dst.dim(2).set_bounds(0, 3);
        dst.dim(2).set_stride(1);

        Expr width = src.dim(0).extent();
        Expr height = src.dim(1).extent();

        auto sample = [&](Expr sx, Expr sy) {
            Expr mx = map_repeat_coord(sx, width);
            Expr my = map_repeat_coord(sy, height);
            return src(mx, my);
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

HALIDE_REGISTER_GENERATOR(DngDemosaicBilinear, dng_demosaic_bilinear)
