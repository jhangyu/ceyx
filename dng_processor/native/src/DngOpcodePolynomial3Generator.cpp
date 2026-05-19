/*
---
file_summary: "Halide AOT generator for three interleaved Stage2 MapPolynomial opcodes in one pass."
functions:
  - name: "DngOpcodePolynomial3Generator::generate"
    description: "Applies one polynomial per RGB channel from interleaved uint16 input to interleaved uint16 output."
    lines: "35-106"
  - name: "DngOpcodePolynomial3Generator::schedule"
    description: "Schedules the 3-channel point operation for Metal or CPU fallback."
    lines: "108-128"
---
*/

#include "Halide.h"

using namespace Halide;

namespace {

constexpr int kMaxDegree = 8;
constexpr int kNumCoeffs = kMaxDegree + 1;
constexpr int kPlanes = 3;

}  // namespace

class DngOpcodePolynomial3Generator
    : public Halide::Generator<DngOpcodePolynomial3Generator> {
public:
    Input<Buffer<uint16_t>> src{"src", 3};     // x, y, c interleaved RGB
    Input<Buffer<float>> coeff{"coeff", 2};    // coeff index, channel
    Input<Buffer<int32_t>> degree{"degree", 1};
    Input<float> pixel_range{"pixel_range"};
    Output<Buffer<uint16_t>> dst{"dst", 3};    // x, y, c interleaved RGB

    void generate() {
        Var x("x"), y("y"), c("c");

        src.dim(0).set_stride(kPlanes);
        src.dim(2).set_bounds(0, kPlanes);
        src.dim(2).set_stride(1);
        dst.dim(0).set_stride(kPlanes);
        dst.dim(2).set_bounds(0, kPlanes);
        dst.dim(2).set_stride(1);
        coeff.dim(0).set_bounds(0, kNumCoeffs);
        coeff.dim(1).set_bounds(0, kPlanes);
        degree.dim(0).set_bounds(0, kPlanes);

        Expr inv_range = 1.0f / pixel_range;
        Expr x_norm = cast<float>(src(x, y, c)) * inv_range;

        Expr c0 = coeff(0, c);
        Expr c1 = coeff(1, c);
        Expr c2 = coeff(2, c);
        Expr c3 = coeff(3, c);
        Expr c4 = coeff(4, c);
        Expr c5 = coeff(5, c);
        Expr c6 = coeff(6, c);
        Expr c7 = coeff(7, c);
        Expr c8 = coeff(8, c);
        Expr deg = degree(c);

        Expr horner8 = c0 + x_norm * (c1 + x_norm * (c2 + x_norm *
                       (c3 + x_norm * (c4 + x_norm * (c5 + x_norm *
                       (c6 + x_norm * (c7 + x_norm * c8)))))));
        Expr horner7 = c0 + x_norm * (c1 + x_norm * (c2 + x_norm *
                       (c3 + x_norm * (c4 + x_norm * (c5 + x_norm *
                       (c6 + x_norm * c7))))));
        Expr horner6 = c0 + x_norm * (c1 + x_norm * (c2 + x_norm *
                       (c3 + x_norm * (c4 + x_norm * (c5 + x_norm * c6)))));
        Expr horner5 = c0 + x_norm * (c1 + x_norm * (c2 + x_norm *
                       (c3 + x_norm * (c4 + x_norm * c5))));
        Expr horner4 = c0 + x_norm * (c1 + x_norm * (c2 + x_norm *
                       (c3 + x_norm * c4)));
        Expr horner3 = c0 + x_norm * (c1 + x_norm * (c2 + x_norm * c3));
        Expr horner2 = c0 + x_norm * (c1 + x_norm * c2);
        Expr horner1 = c0 + x_norm * c1;
        Expr horner0 = c0;

        Expr y_unclamped = select(deg <= 0, horner0,
                                  deg == 1, horner1,
                                  deg == 2, horner2,
                                  deg == 3, horner3,
                                  deg == 4, horner4,
                                  deg == 5, horner5,
                                  deg == 6, horner6,
                                  deg == 7, horner7,
                                  horner8);
        Expr y_scaled = clamp(y_unclamped, 0.0f, 1.0f) * pixel_range + 0.5f;
        dst(x, y, c) = cast<uint16_t>(y_scaled);
    }

    void schedule() {
        Var x("x"), y("y"), c("c");

        if (using_autoscheduler()) {
            src.set_estimates({{0, 6000}, {0, 4000}, {0, kPlanes}});
            coeff.set_estimates({{0, kNumCoeffs}, {0, kPlanes}});
            degree.set_estimates({{0, kPlanes}});
            pixel_range.set_estimate(65535.0f);
            dst.set_estimates({{0, 6000}, {0, 4000}, {0, kPlanes}});
        } else if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            dst.bound(c, 0, kPlanes)
               .reorder(c, x, y)
               .gpu_tile(x, y, xo, yo, xi, yi, 16, 16)
               .unroll(c);
        } else {
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, kPlanes)
               .reorder(c, x, y)
               .split(y, yo, yi, 64)
               .parallel(yo)
               .vectorize(x, 16)
               .unroll(c);
        }
    }
};

HALIDE_REGISTER_GENERATOR(DngOpcodePolynomial3Generator,
                          dng_opcode_polynomial3)
