/*
---
file_summary: |
  Halide AOT generator that implements Adobe DNG SDK's
  `dng_opcode_MapPolynomial` (opcodeID = 8) for Stage 2 OpcodeList2.

  The polynomial is a per-pixel 1-D function applied independently to a
  single plane of a uint16 image (typically R, G, or B in the demosaiced
  Stage 2 buffer). For Phase 10 Sprint C this kernel is created to fold the
  three sequential SDK MapPolynomial calls (one per plane, ~134ms total on
  24MP lossy DNG) into a single GPU dispatch.

  Reference: third_party/dng_sdk/source/dng_misc_opcodes.cpp
    - `dng_opcode_MapPolynomial::ProcessArea` (Horner polynomial, clamp
      to [0, 1], operates on already-normalized float buffer)
    - `dng_opcode_MapPolynomial::BufferPixelType` (returns ttFloat;
      coefficients used at Stage 2 are unmodified — `scale32 = 1`)
  Reference: third_party/dng_sdk/source/dng_reference.cpp
    - `RefCopyArea16_R32` (uint16 → float: divide by pixelRange)
    - `RefCopyAreaR32_16` (float → uint16: multiply by pixelRange, +0.5)

  Net per-pixel algorithm reproduced here:
    x_norm   = clamp(src, 0, pixelRange) / pixelRange
    y_norm   = Pin(0, Horner(coeff32, x_norm), 1)
    dst_u16  = round(y_norm * pixelRange)

  AOT ABI (see class definition below for exact param ordering):
    inputs : src (uint16 2-D), coeff (float[kMaxDegree+1] = 9 entries),
             degree (int32), pixel_range (float)
    output : dst (uint16 2-D, same extent as src)

  Caller responsibilities:
    * Pre-fill the coeff buffer to length 9 (kMaxDegree + 1 = 8 + 1).
      Unused entries (`j > degree`) MUST be zero. SDK does this in
      `dng_opcode_MapPolynomial` constructor.
    * `degree` is the polynomial degree (0..8). SDK's stream parser
      throws ThrowBadFormat if degree > kMaxDegree.
    * `pixel_range` is the uint16 max (typically 65535.0f). This mirrors
      `dng_image::PixelRange()` for ttShort images.
    * Pass single plane at a time: SDK invokes one opcode per plane.

  Schedule:
    * GPU: 16x16 tile via `gpu_tile` (Metal-friendly thread group).
    * CPU: vectorize x by 16 + parallel y in strips of 64 rows.
---
*/

#include "Halide.h"

using namespace Halide;

namespace {

// Mirrors `dng_opcode_MapPolynomial::kMaxDegree` from the Adobe SDK.
// Polynomial degree is in [0, kMaxDegree] (inclusive), so we need
// kMaxDegree + 1 coefficients.
constexpr int kMaxDegree = 8;
constexpr int kNumCoeffs = kMaxDegree + 1;  // 9

}  // namespace

class DngOpcodePolynomialGenerator
    : public Halide::Generator<DngOpcodePolynomialGenerator> {
public:
    // Single-plane uint16 source. We operate per-plane to mirror SDK's
    // `fAreaSpec.Plane()` semantics — the host bridge will dispatch
    // this kernel once per plane (Stage 2 lossy DNG has R/G/B).
    Input<Buffer<uint16_t, 2>> src{"src"};

    // Polynomial coefficients in ascending order (c0, c1, ..., c8).
    // Length is fixed to kNumCoeffs (9) for GPU friendliness — unused
    // entries (index > degree) must be zero on the host side.
    Input<Buffer<float, 1>> coeff{"coeff"};

    // Actual polynomial degree (0..kMaxDegree). Drives the Horner select
    // chain so we can early-out for low-degree polynomials and avoid
    // multiplying-by-zero noise from padding.
    Input<int32_t> degree{"degree"};

    // Pixel range used to normalize uint16 to/from the [0, 1] float
    // domain. Mirrors `dng_image::PixelRange()` (typically 65535.0f).
    Input<float> pixel_range{"pixel_range"};

    Output<Buffer<uint16_t, 2>> dst{"dst"};

    void generate() {
        Var x("x"), y("y");

        // Tell Halide the coeff buffer always has length kNumCoeffs.
        coeff.dim(0).set_bounds(0, kNumCoeffs);

        // Normalize source pixel to [0, 1] float domain. Cast first to
        // float32 — SDK does the same via `RefCopyArea16_R32` using a
        // scalar 1/pixelRange multiply.
        Expr inv_range = 1.0f / pixel_range;
        Expr x_norm = cast<float>(src(x, y)) * inv_range;

        // Horner polynomial expressed as a fully-unrolled chain of
        // `select` statements. For Stage 2 polynomials (which in the
        // lossy DNG sample observed for Phase 10 Sprint C are all
        // degree 1, two-coefficient gain curves), the GPU will see a
        // constant `degree` per dispatch and the JIT/codegen folds the
        // select chain to a single multiply-add.
        //
        // The chain mirrors SDK's switch(fDegree) handling in
        // `ProcessArea` but expressed as expression dataflow so it
        // schedules cleanly on Metal:
        //   degree == 0 -> y = c0
        //   degree == 1 -> y = c0 + x*c1
        //   degree == 2 -> y = c0 + x*(c1 + x*c2)
        //   ...
        //   degree == 8 -> full kMaxDegree Horner
        Expr c0 = coeff(0);
        Expr c1 = coeff(1);
        Expr c2 = coeff(2);
        Expr c3 = coeff(3);
        Expr c4 = coeff(4);
        Expr c5 = coeff(5);
        Expr c6 = coeff(6);
        Expr c7 = coeff(7);
        Expr c8 = coeff(8);

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

        Expr y_unclamped = select(degree <= 0, horner0,
                                  degree == 1, horner1,
                                  degree == 2, horner2,
                                  degree == 3, horner3,
                                  degree == 4, horner4,
                                  degree == 5, horner5,
                                  degree == 6, horner6,
                                  degree == 7, horner7,
                                  horner8);

        // SDK does `Pin_real32(0.0f, y, 1.0f)` per pixel.
        Expr y_clamped = clamp(y_unclamped, 0.0f, 1.0f);

        // Quantize back to uint16. SDK's `RefCopyAreaR32_16` uses
        // `(uint16) (Pin_Overrange(v) * pixelRange + 0.5f)`.
        Expr y_scaled = y_clamped * pixel_range + 0.5f;
        dst(x, y) = cast<uint16_t>(y_scaled);
    }

    void schedule() {
        Var x("x"), y("y");

        if (using_autoscheduler()) {
            // Autoscheduler estimates (rough — single-plane Stage 2
            // typical of 24MP lossy DNG is 6000 x 4000).
            src.set_estimates({{0, 6000}, {0, 4000}});
            coeff.set_estimates({{0, kNumCoeffs}});
            degree.set_estimate(1);
            pixel_range.set_estimate(65535.0f);
            dst.set_estimates({{0, 6000}, {0, 4000}});
        } else if (get_target().has_gpu_feature()) {
            // Metal-friendly 16x16 tile (256 threads per group).
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            dst.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
        } else {
            // CPU fallback: vectorize across x, parallel strips of y.
            Var yo("yo"), yi("yi");
            dst.split(y, yo, yi, 64)
               .parallel(yo)
               .vectorize(x, 16);
        }
    }
};

HALIDE_REGISTER_GENERATOR(DngOpcodePolynomialGenerator,
                          dng_opcode_polynomial)
