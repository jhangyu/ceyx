// Fused generic-RAW Bayer pipeline: CFA mosaic -> display-referred RGBA8, in
// ONE kernel, with no full-frame RGB16 intermediate ever allocated.
//
// WHAT THIS REPLACES
// ------------------
// The two-stage path runs raw_bayer_demosaic into a ~139-245 MB per-lane
// interleaved RGB16 Stage-3 buffer, then runs dng_render_stage4 out of it.
// That intermediate is the single largest per-lane allocation in the decode
// and it exists only to carry data between two kernels. Fusing the two removes
// the buffer outright rather than shrinking it.
//
// WHY THE ARITHMETIC IS NOT COPIED HERE
// --------------------------------------
// The colour arithmetic is NOT duplicated in this file. It lives once in
// dng_render_stage4_expr.h and is called by both this generator and
// DngRenderStage4, so a future colour-science fix cannot land in one and miss
// the other. That extraction was proven inert: dng_render_stage4.a is
// byte-identical across it (SHA-256 frozen before, compared after -- see
// native/tests/tmp/t20-05c-extraction-prereg.txt). Likewise the demosaic taps
// come from build_demosaic_expr() in dng_halide_utils.h, the same helper the
// two-stage kernel uses.
// The only thing this file contributes is the SEAM and the SCHEDULE.
//
// BACKEND PARITY -- there is deliberately no platform guard in here
// -----------------------------------------------------------------
// The campaign's rule forbids sending one platform down a different path.
// Nothing below branches on get_target().os or on a Vulkan feature: the same
// kernel shape is emitted for every backend. That is possible because the
// extracted colour arithmetic contains no layout decision, and because the
// fused kernel's source is a 2D mosaic -- so the dense-planar src workaround
// that the two-stage Vulkan path needs for its 3D interleaved Stage-3 input
// does not arise here at all.
//
// WHY NOTHING IS STAGED IN SHARED MEMORY -- measured, not chosen
// ---------------------------------------------------------------
// The obvious fused schedule stages the normalized mosaic on-chip, exactly as
// RawBayerDemosaicGenerator does. It was tried FIRST and it does not survive
// backend parity:
//   Metal  lowers it fine.
//   Vulkan refuses outright -- "Dynamic workgroup sizes require Vulkan v1.3+".
// The cause is structural, not a tuning knob. In the two-stage kernel the
// demosaic output coordinate IS the Stage-3 coordinate, so one output tile
// needs a statically-known 18x18 halo. Here the render reads through the EXIF
// orientation affine, whose six coefficients are RUNTIME scalars on purpose
// (one archive serves all eight orientations; see DngRenderGenerator.cpp).
// Halide cannot bound a runtime affine at compile time, so the staged extent
// becomes dynamic -- the Metal statement literally frees a variable named
// "normalized.0.shared_size". Metal tolerates a dynamic threadgroup size;
// Vulkan below v1.3 does not.
// Staging here and not there would be precisely the platform divergence this
// campaign forbids, so nothing is staged on EITHER backend. Both targets emit
// ONE dispatch with zero shared memory -- same source, same shape.
// This costs no redundant compute for the same reason the Android Stage-4
// kernel is fully inlined: unroll(c) plus the select folds c per copy and CSE
// shares the c-independent body.
//
// THE HARD CONSTRAINT IF A PRODUCER IS EVER STAGED HERE
// ------------------------------------------------------
// Any Func materialised at a GPU loop level becomes an array in the Workgroup
// storage class, and the Adreno Vulkan driver miscompiles that array when its
// element type is narrower than 32 bits -- SILENTLY, with a zero return code
// (measured; see DngRenderGenerator.cpp's note on the split kernel). So every
// producer this schedule stages is >= 32 bits wide, and that is enforced
// mechanically by native/tests/check_gpu_producer_width.py reading the lowered
// statement, not by this comment. That check enumerates whatever the statement
// actually stages, so it constrains producers added in future, not just the
// ones present today.
// In particular `rendered_rgb` is a Tuple of three uint8_t, so it could never
// be staged here as-is even if the bounds problem above were solved.
#include "Halide.h"
#include "dng_halide_utils.h"
#include "dng_render_stage4_expr.h"

using namespace Halide;

class RawBayerFusedRender : public Halide::Generator<RawBayerFusedRender> {
public:
    // Sub-tile tail safety, same default and same reason as DngRenderStage4:
    // with -no_asserts-no_bounds_query the default tail strategy writes a full
    // tile into a smaller buffer.
    GeneratorParam<bool> guard_tail{"guard_tail", true};

    // --- demosaic side, matching RawBayerDemosaicGenerator's inputs ---
    Input<Buffer<uint16_t>> src{"src", 2};      // x, y CFA mosaic
    Input<int32_t> red_x{"red_x"};
    Input<int32_t> red_y{"red_y"};
    Input<Buffer<float>> black{"black", 2};     // black repeat tile
    Input<float> inv_range{"inv_range"};        // 65535 / (white - black_max)

    // CROP SEAM -- load-bearing for byte-identity, see the note below.
    // The two-stage path demosaics the FULL mosaic plane and only then crops
    // the Stage-3 result. Cropping the mosaic instead would change the CFA
    // phase and, more subtly, change map_repeat_coord's boundary wrap at the
    // crop edge: raw_sample.arw is a 6048-wide plane cropped to 6024, so the
    // two-stage kernel reads REAL pixels at x=6024..6047 when demosaicing
    // x=6023, whereas a cropped mosaic would wrap there. So the mosaic is
    // passed whole and the render's read is offset by these instead.
    Input<int32_t> crop_x{"crop_x"};
    Input<int32_t> crop_y{"crop_y"};
    // The CROPPED extents. These drive the render's clamp, so it matches the
    // two-stage clamp against the cropped Stage-3 view exactly, while the
    // demosaic's own boundary logic keeps using the full plane extents.
    Input<int32_t> src_w{"src_w"};
    Input<int32_t> src_h{"src_h"};

    // --- render side, sliced verbatim from DngRenderGenerator.cpp ---
    Input<float> src_scale{"src_scale"};            // usually 1 / 65535
    // Fused EXIF orientation (productionization plan §1.1, T7b affine form).
    // RUNTIME scalars, not GeneratorParams: one archive serves all 8 cases.
    // ux = a_x*x + b_x*y + c_x, uy = a_y*x + b_y*y + c_y. The host derives all
    // six with ceyx_orient_affine_coeffs(); the unoriented extents are folded
    // into the c terms, so the kernel never sees an orientation value.
    Input<int32_t> orient_a_x{"orient_a_x"};
    Input<int32_t> orient_b_x{"orient_b_x"};
    Input<int32_t> orient_c_x{"orient_c_x"};
    Input<int32_t> orient_a_y{"orient_a_y"};
    Input<int32_t> orient_b_y{"orient_b_y"};
    Input<int32_t> orient_c_y{"orient_c_y"};
    Input<Buffer<float>> exp_ramp{"exp_ramp", 1};   // 4098
    Input<Buffer<float>> tone_curve{"tone_curve", 1}; // 4098
    Input<Buffer<float>> encode_gamma{"encode_gamma", 1}; // 4098
    Input<Buffer<float>> camera_white{"camera_white", 1}; // 3
    Input<Buffer<float>> camera_to_rgb{"camera_to_rgb", 2}; // [col, row] 3x3
    Input<Buffer<float>> rgb_to_final{"rgb_to_final", 2};   // [col, row] 3x3
    Input<Buffer<float>> huesat_table{"huesat_table", 2};   // [entry, component(0..2)]
    Input<Buffer<float>> huesat_encode{"huesat_encode", 1}; // 4098
    Input<Buffer<float>> huesat_decode{"huesat_decode", 1}; // 4098
    Input<int32_t> huesat_hue_div{"huesat_hue_div"};
    Input<int32_t> huesat_sat_div{"huesat_sat_div"};
    Input<int32_t> huesat_val_div{"huesat_val_div"};
    Input<int32_t> huesat_has_table{"huesat_has_table"};
    Input<int32_t> huesat_has_encoding{"huesat_has_encoding"};
    Input<Buffer<float>> look_table{"look_table", 2};       // [entry, component(0..2)]
    Input<Buffer<float>> look_encode{"look_encode", 1};     // 4098
    Input<Buffer<float>> look_decode{"look_decode", 1};     // 4098
    Input<int32_t> look_hue_div{"look_hue_div"};
    Input<int32_t> look_sat_div{"look_sat_div"};
    Input<int32_t> look_val_div{"look_val_div"};
    Input<int32_t> look_has_table{"look_has_table"};
    Input<int32_t> look_has_encoding{"look_has_encoding"};
    Output<Buffer<uint8_t>> dst{"dst", 3};      // x, y, c (RGBA8 interleaved)

    // Staged 32-bit on purpose -- see the header comment. uint16_t is the
    // natural type and is miscompiled in Workgroup storage on Adreno.
    // Widening is bit-exact: the value is clamped to [0, 65535] before the
    // cast, and the demosaic taps narrow it straight back.
    Func normalized{"normalized"};
    // Left INLINE (never scheduled): reading it inlines the 9-tap demosaic
    // over `normalized`, which is the shape the two-stage kernel already ships
    // and which has passed the Adreno device gate.
    Func demosaiced{"demosaiced"};
    Func rendered_rgb{"rendered_rgb"};

    void generate() {
        Var x("x"), y("y"), c("c");

        src.dim(0).set_stride(1);
        // Interleaved RGBA8 out, alpha written in-kernel. Same layout on every
        // backend: the interleaved dst construct was re-verified safe on
        // Halide v21 Vulkan (Adreno 750) for a kernel of this shape.
        dst.dim(0).set_stride(4);
        dst.dim(2).set_bounds(0, 4);
        dst.dim(2).set_stride(1);

        Expr width = src.dim(0).extent();
        Expr height = src.dim(1).extent();
        Expr bw = max(black.dim(0).extent(), 1);
        Expr bh = max(black.dim(1).extent(), 1);

        {
            Expr mx = map_repeat_coord(x, width);
            Expr my = map_repeat_coord(y, height);
            Expr level = black(mx % bw, my % bh);
            Expr norm = (cast<float>(src(mx, my)) - level) * inv_range;
            normalized(x, y) = cast<uint32_t>(clamp(norm, 0.0f, 65535.0f));
        }

        auto sample_normalized = [&](Expr sx, Expr sy) {
            return cast<uint16_t>(normalized(sx, sy));
        };
        demosaiced(x, y, c) =
            build_demosaic_expr(x, y, c, sample_normalized, red_x, red_y);

        // THE SEAM. In the two-stage pipeline this read came from a
        // materialised Stage-3 buffer; here it is the demosaic itself, so the
        // intermediate never exists. The arithmetic on the far side is
        // byte-for-byte the same code the two-stage kernel runs.
        ceyx::build_dng_render_stage4_rgb(
            x, y,
            [&](Expr sample_x, Expr sample_y, int channel) {
                return demosaiced(sample_x + crop_x, sample_y + crop_y, channel);
            },
            src_w, src_h, src_scale,
            orient_a_x, orient_b_x, orient_c_x,
            orient_a_y, orient_b_y, orient_c_y,
            exp_ramp, tone_curve, encode_gamma, camera_white,
            camera_to_rgb, rgb_to_final,
            huesat_table, huesat_encode, huesat_decode,
            huesat_hue_div, huesat_sat_div, huesat_val_div,
            huesat_has_table, huesat_has_encoding,
            look_table, look_encode, look_decode,
            look_hue_div, look_sat_div, look_val_div,
            look_has_table, look_has_encoding,
            rendered_rgb);

        dst(x, y, c) = select(c == 0, rendered_rgb(x, y)[0],
                              c == 1, rendered_rgb(x, y)[1],
                              c == 2, rendered_rgb(x, y)[2],
                                      cast<uint8_t>(255));
    }

    void schedule() {
        Var x("x"), y("y"), c("c");

        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            dst.bound(c, 0, 4)
               .reorder(c, x, y);
            if (guard_tail) {
                dst.gpu_tile(x, y, xo, yo, xi, yi, 16, 16,
                             TailStrategy::GuardWithIf);
            } else {
                dst.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            }
            dst.unroll(c);
            // 16x16 is the production tile the tile-bench measured. 32x32 was
            // faster there but was never validated against threadgroup-memory
            // and occupancy limits on smaller Apple GPUs, so changing it is a
            // separate, measured decision.
            // normalized is deliberately NOT staged here. See the header note
            // "WHY NOTHING IS STAGED": the render gathers through a RUNTIME
            // orientation affine, so a staged producer's extent is not known
            // at compile time, and Vulkan refuses a dynamic workgroup size.
        } else {
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, 4)
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

HALIDE_REGISTER_GENERATOR(RawBayerFusedRender, raw_bayer_fused_render)
