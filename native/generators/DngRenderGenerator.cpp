#include "Halide.h"
#include "dng_render_stage4_expr.h"
#include "dng_render_stage4_split_expr.h"
#include "ceyx_yuv420_write.h"

using namespace Halide;

// W7 (2026-08-21, Windows port): the dense-planar src/dst layout selected below
// is a workaround for the Halide v21 SPIR-V Tuple/dim(2) codegen bug, i.e. a
// property of the *Vulkan* backend rather than of Android. Selecting it by
// backend lets the Windows-Vulkan AOT target reuse the same workaround.
// Android is unaffected: its AOT target string always carries the vulkan
// feature (CMakeLists.txt AOT_TARGET), so this predicate is true exactly where
// `os == Target::Android` used to be.
static bool uses_vulkan_planar_layout(const Target &t) {
    return t.os == Target::Android || t.has_feature(Target::Vulkan);
}

// mem8 v3 T12: the ONE schedule for a yuv420 plane triple, so the two arm-B
// generators below (and arm A's fused generator) cannot drift into per-family
// scheduling. Templated on the output type only because the two generators'
// Output<> types are distinct C++ types; the body is identical for all callers
// and contains no backend branch beyond has_gpu_feature(), which is the same
// branch every generator in this file already makes.
//
// Deliberately NO compute_at anywhere: a staged producer becomes a Workgroup
// array and is miscompiled below 32 bits on the Vulkan driver this project
// targets, silently. check_gpu_producer_width.py is the mechanical enforcement.
template <typename OutputT>
static void scheduleYuv420Planes(const Target &t, bool guard_tail, Var x, Var y,
                                 OutputT &y_plane, OutputT &cb_plane,
                                 OutputT &cr_plane) {
    OutputT *planes[3] = {&y_plane, &cb_plane, &cr_plane};
    for (OutputT *plane : planes) {
        if (t.has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            if (guard_tail) {
                plane->gpu_tile(x, y, xo, yo, xi, yi, 16, 16,
                                TailStrategy::GuardWithIf);
            } else {
                plane->gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            }
        } else {
            Var yo("yo"), yi("yi");
            plane->split(y, yo, yi, 32).parallel(yo).vectorize(x, 8);
        }
    }
}


class DngRenderStage4 : public Halide::Generator<DngRenderStage4> {
public:
    // Sub-tile tail safety. ON by default: with -no_asserts-no_bounds_query the
    // default (ShiftInwards) tail writes a full tile into a smaller buffer.
    // Compiles on Metal AND on the Android Vulkan target (finding F-V3-9).
    GeneratorParam<bool> guard_tail{"guard_tail", true};

    // Android Vulkan workaround: Android AOT uses dense planar RGB src.
    // Other targets keep the original interleaved RGB layout for performance.
    Input<Buffer<uint16_t>> src{"src", 3};          // x, y, c
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
    Output<Buffer<uint8_t>> dst{"dst", 3};          // x, y, c
    Func rendered_rgb{"rendered_rgb"};

    void generate() {
        Var x("x"), y("y"), c("c");

        if (uses_vulkan_planar_layout(get_target())) {
            // Vulkan SPIR-V workaround: use dense planar src so GPU code never
            // reads interleaved channel-stride-1 input on dim(2).
            src.dim(0).set_stride(1);
            src.dim(1).set_stride(src.dim(0).extent());
            src.dim(2).set_bounds(0, 3);
            src.dim(2).set_stride(src.dim(0).extent() * src.dim(1).extent());
        } else {
            // Original macOS/CPU layout: Stage3 produces interleaved RGB.
            src.dim(0).set_stride(3);
            src.dim(2).set_bounds(0, 3);
            src.dim(2).set_stride(1);
        }
        if (uses_vulkan_planar_layout(get_target())) {
            // Vulkan SPIR-V workaround: use dense planar dst so GPU code never
            // writes interleaved channel-stride-1 output on dim(2).
            dst.dim(0).set_stride(1);
            dst.dim(1).set_stride(dst.dim(0).extent());
            dst.dim(2).set_bounds(0, 3);
            dst.dim(2).set_stride(dst.dim(0).extent() * dst.dim(1).extent());
        } else {
            // W7 (M-11): macOS outputs RGBA8 (stride-4, alpha=255 in-kernel).
            // Eliminates the FFI rgb_to_rgba pass and one ~72MB RGB intermediate.
            dst.dim(0).set_stride(4);
            dst.dim(2).set_bounds(0, 4);
            dst.dim(2).set_stride(1);
        }

        Func src_f("src_f");
        src_f(x, y, c) = src(x, y, c);
        // mem8 v3 T20 step C: the ~420 lines of Stage-4 colour arithmetic that
        // used to sit here now live in dng_render_stage4_expr.h, because the
        // fused Bayer-demosaic+render kernel needs byte-for-byte the SAME
        // arithmetic and a second copy would drift silently -- there is no
        // cross-generator equivalence test in this tree that would catch it.
        // The move was mechanical, not retyped, and it is proven inert: this
        // generator's AOT archive dng_render_stage4.a is byte-identical across
        // the extraction (SHA-256 frozen before, compared after; see
        // native/tests/tmp/t20-05c-extraction-prereg.txt).
        // The ONLY thing a caller varies is where the camera-space RGB triple
        // comes from. Everything behind that seam is backend- and
        // layout-independent, which is why one header serves both backend
        // families with no platform branch.
        ceyx::build_dng_render_stage4_rgb(
            x, y,
            [&](Expr sample_x, Expr sample_y, int channel) {
                return src_f(sample_x, sample_y, channel);
            },
            src.dim(0).extent(), src.dim(1).extent(), src_scale,
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


        if (uses_vulkan_planar_layout(get_target())) {
            dst(x, y, c) = select(c == 0, rendered_rgb(x, y)[0],
                                  c == 1, rendered_rgb(x, y)[1],
                                          rendered_rgb(x, y)[2]);
        } else {
            // W7 (M-11): RGBA8 output with alpha=255 in-kernel.
            dst(x, y, c) = select(c == 0, rendered_rgb(x, y)[0],
                                  c == 1, rendered_rgb(x, y)[1],
                                  c == 2, rendered_rgb(x, y)[2],
                                          cast<uint8_t>(255));
        }
    }

    void schedule() {
        Var x("x"), y("y"), c("c");
        // W7 (M-11): macOS outputs 4 channels (RGBA8); Android (separate
        // DngRenderStage4Android generator) stays 3. This generator is only
        // compiled for non-Android targets.
        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            // P12-W1-03: fuse rendered_rgb into dst via compute_at to eliminate
            // the ~145.8 MB intermediate GPU global-memory roundtrip and collapse
            // two Metal kernel dispatches into one. dst now drives the 16x16 tile.
            dst.bound(c, 0, 4)
               .reorder(c, x, y);
            if (guard_tail) {
                dst.gpu_tile(x, y, xo, yo, xi, yi, 16, 16,
                             TailStrategy::GuardWithIf);
            } else {
                dst.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
            }
            dst.unroll(c);
            rendered_rgb.compute_at(dst, xo)
                        .gpu_threads(x, y);
        } else {
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, 4)
               .reorder(c, x, y)
               .split(y, yo, yi, 32)
               .parallel(yo)
               .vectorize(x, 8)
               .unroll(c);
            rendered_rgb.compute_at(dst, yo)
                        .vectorize(x, 8);
        }
    }
};

// =============================================================================
// Android Vulkan Stage4 generator.
// G2 (Round 2): outputs interleaved RGBA8 directly (same dst layout as the
// macOS kernel: dim0 stride 4, dim2 stride 1 bounds [0,4)), retiring the
// planar-output workaround + host repack. The interleaved dst construct was
// re-verified SAFE on Halide v21 Vulkan (Adreno 750) by the G2 pre-check
// Probe A — fully-inlined kernel, 0/24,000,000 per-channel mismatches at
// 6000x4000 (docs/logs/2026-07-04/Task_g2_vulkan_rg_bug_recheck.md).
// CONSTRAINT: do NOT port the macOS `rendered_rgb.compute_at` schedule as it
// stands. The reason is NOT `compute_at` itself — the previous wording here
// ("any materialized compute_at producer still collapses G/B") generalised
// from a single measurement and is FALSE as stated; T22 measured the actual
// variable and it is the producer's ELEMENT TYPE.
// A Func materialised at a GPU loop level becomes an array in the Workgroup
// storage class, and this driver miscompiles that array when its element type
// is narrower than 32 bits. Measured on Adreno 750 / SM8650, driver V@0762.41,
// two schedules identical character-for-character apart from the staged type,
// same binary, same run, 6000x4000: uint8 gives G/B = 24,000,000/24,000,000
// wrong, every wrong value 255 (the last select branch) — reproducing Probe B's
// R count of 93,750 exactly; uint32 gives 0/0/0/0. uint16 is partially wrong.
// `align_bounds` does NOT rescue it, re-confirming the 2026-07-05 note.
// So: a materialised producer IS available here if its element type is 32-bit
// or wider. `rendered_rgb` is a Tuple of three uint8_t (:465), which is why
// porting it UNCHANGED is still wrong. Widening is normally value-neutral when
// the value is already range-clamped, and the sibling generic-RAW kernel took
// exactly this route (RawBayerDemosaicGenerator.cpp, uint32 staged producer,
// device-gated green) — but that specific widening of `rendered_rgb` has NOT
// been measured, so treat it as an available option to be gated, not a
// finished result.
// The failure is SILENT — kernel_rc=0, no crash, no error code, just wrong
// pixels — so any future materialised producer here needs a mechanical
// element-width check, not a reviewer's memory.
// Until such a port is gated, the kernel body stays fully inlined; unroll(c) +
// select folds c per copy and CSE shares the c-independent pipeline body, so
// inlining costs no redundant compute.
// LIMITS OF THE ABOVE: one device, one driver build. Windows and Linux compile
// this same generator on Vulkan and were NOT exercised — no hardware for them
// here; they are implicated by shared source, not by measurement. The causal
// story (sub-32-bit Workgroup arrays are miscompiled) is inference consistent
// with the data, not vendor-confirmed; what is measured is the input/output
// relation. Evidence: T22, 2026-09-19.
//
// TailStrategy::GuardWithIf COMPILES on this target (RC=0 for
// arm-64-android-vulkan-vk_int8-vk_int16-vk_int64-no_asserts-no_bounds_query,
// host-vulkan and host-metal), verified with a positive control showing the
// guard_tail=true/false archives differ in size and SHA-256 (finding F-V3-9,
// docs/logs/2026-09-07/Task_vk_orient_v3_findings.md). The comment previously
// here claimed it required Vulkan v1.3 dynamic workgroup sizes; that claim was
// false as stated. Device CORRECTNESS of the guard is gated on real hardware
// (Task 6); note F-T6-3 -- TailStrategy::Auto already bounds the tail on this
// device, so guard_tail=false is NOT a red-state correctness control here.
// =============================================================================
class DngRenderStage4Android : public Halide::Generator<DngRenderStage4Android> {
public:
    GeneratorParam<int32_t> diag_stage{"diag_stage", -1};
    // Sub-tile tail safety. ON by default: with -no_asserts-no_bounds_query the
    // default (ShiftInwards) tail writes a full tile into a smaller buffer.
    // Compiles on Metal AND on the Android Vulkan target (finding F-V3-9).
    GeneratorParam<bool> guard_tail{"guard_tail", true};

    // W2: single flat-1D interleaved src (replaces three planar src_r/g/b).
    // Contents = SDK interleaved RGB buffer (row-major, channel stride 1) laid
    // out as one 1D plane of size src_row_stride_px * src_height * 3. The host
    // zero-copy wraps the SDK buffer (no repack_src). Verified bit-exact on
    // Vulkan by the W4-2 probe (Gotcha #95).
    Input<Buffer<uint16_t>> src_rgb{"src_rgb", 1};
    Input<int32_t> src_width{"src_width"};
    Input<int32_t> src_height{"src_height"};
    Input<int32_t> src_row_stride_px{"src_row_stride_px"};
    Input<int32_t> crop_l{"crop_l"};
    Input<int32_t> crop_t{"crop_t"};
    Input<float> src_scale{"src_scale"};
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
    Input<Buffer<float>> exp_ramp{"exp_ramp", 1};
    Input<Buffer<float>> tone_curve{"tone_curve", 1};
    Input<Buffer<float>> encode_gamma{"encode_gamma", 1};
    Input<Buffer<float>> camera_white{"camera_white", 1};
    Input<Buffer<float>> camera_to_rgb{"camera_to_rgb", 1};
    Input<Buffer<float>> rgb_to_final{"rgb_to_final", 1};
    Input<Buffer<float>> huesat_table{"huesat_table", 1};
    Input<Buffer<float>> huesat_encode{"huesat_encode", 1};
    Input<Buffer<float>> huesat_decode{"huesat_decode", 1};
    Input<int32_t> huesat_entry_count{"huesat_entry_count"};
    Input<int32_t> huesat_hue_div{"huesat_hue_div"};
    Input<int32_t> huesat_sat_div{"huesat_sat_div"};
    Input<int32_t> huesat_val_div{"huesat_val_div"};
    Input<int32_t> huesat_has_table{"huesat_has_table"};
    Input<int32_t> huesat_has_encoding{"huesat_has_encoding"};
    Input<Buffer<float>> look_table{"look_table", 1};
    Input<Buffer<float>> look_encode{"look_encode", 1};
    Input<Buffer<float>> look_decode{"look_decode", 1};
    Input<int32_t> look_entry_count{"look_entry_count"};
    Input<int32_t> look_hue_div{"look_hue_div"};
    Input<int32_t> look_sat_div{"look_sat_div"};
    Input<int32_t> look_val_div{"look_val_div"};
    Input<int32_t> look_has_table{"look_has_table"};
    Input<int32_t> look_has_encoding{"look_has_encoding"};

    // G2: interleaved RGBA8 output dst(x, y, c) — dim0 stride 4, dim2 stride 1
    // bounds [0,4), alpha=255 in-kernel. Exact macOS production dst layout;
    // construct verified 0-error on Vulkan by G2 pre-check Probe A. Replaces
    // the W4-3 (b) 2D planar dst + host MT repack (repackPlanarToRGBAMT /
    // repackPlanarToInterleavedMT + RepackThreadPool, all retired).
    //
    // Historical dead ends kept for the record:
    // - Plan (a), W4-1: single interleaved 1D dst_rgb(j), j=i*3+c — mis-lowered
    //   on Vulkan (~8 dB border/coverage corruption; Gotcha #93).
    // - W4-3 (b): 2D planar dst(i, c) — correct but needed a full-frame host
    //   repack pass per decode (~8-12 ms MT) plus a 3-plane D2H.
    Output<Buffer<uint8_t>> dst{"dst", 3};  // x, y, c

    // Per-channel 8-bit results, set by emit_rgb8 (possibly from a diag stage),
    // consumed once at the end of generate() to build dst.
    Expr out8_r, out8_g, out8_b;

    void generate() {
        Var x("x"), y("y");

        // G2: interleaved RGBA8 dst layout (macOS layout; probe-A verified).
        dst.dim(0).set_stride(4);
        dst.dim(2).set_bounds(0, 4);
        dst.dim(2).set_stride(1);

        // mem8 v3 T12-pre: the ~320 lines of Stage-4 colour arithmetic that used
        // to sit here now live in dng_render_stage4_split_expr.h, because the
        // yuv420 output variants of this kernel need byte-for-byte the SAME
        // arithmetic and further copies would drift silently -- there is no
        // cross-generator equivalence test in this tree that would catch it.
        // The move was mechanical, not retyped, and it is proven inert: this
        // generator's AOT archive dng_render_stage4_split.a is byte-identical
        // across the extraction (SHA-256 frozen before, compared after, gate
        // proven live by a perturbed constant; see
        // native/tests/tmp/t12-02-extraction-prereg.txt).
        // The ONLY thing a caller varies is what it does with the three 8-bit
        // channel results. finalize() below is this generator's answer
        // (interleaved RGBA8); the yuv420 variants write planes instead.
        ceyx::build_dng_render_stage4_split_rgb8(
            x, y, diag_stage,
            src_rgb, src_width, src_height, src_row_stride_px,
            crop_l, crop_t, src_scale,
            orient_a_x, orient_b_x, orient_c_x,
            orient_a_y, orient_b_y, orient_c_y,
            exp_ramp, tone_curve, encode_gamma, camera_white,
            camera_to_rgb, rgb_to_final,
            huesat_table, huesat_encode, huesat_decode,
            huesat_entry_count,
            huesat_hue_div, huesat_sat_div, huesat_val_div,
            huesat_has_table, huesat_has_encoding,
            look_table, look_encode, look_decode,
            look_entry_count,
            look_hue_div, look_sat_div, look_val_div,
            look_has_table, look_has_encoding,
            out8_r, out8_g, out8_b);
        finalize(x, y);
    }

    // G2: wire the three channel results into the interleaved RGBA8 output.
    // out8_r/g/b are Exprs over (x, y); dst(x, y, c) selects a channel by c
    // (alpha hard 255). reorder(c,x,y) + unroll(c) folds c to a constant per
    // copy so select() prunes to one channel while the pipeline body
    // (independent of c) is CSE-shared — same compute-1x property the planar
    // kernel had, now with coalesced 4-byte interleaved stores.
    void finalize(Var x, Var y) {
        Var c("c");
        dst(x, y, c) = select(c == 0, out8_r,
                              c == 1, out8_g,
                              c == 2, out8_b,
                                      cast<uint8_t>(255));
    }

    void schedule() {
        Var x("x"), y("y"), c("c");
        // G2: exact Probe-A schedule (the verified-safe configuration). Fully
        // inlined — NO compute_at producer (see class comment; Probe B fails).
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
        } else {
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, 4)
               .reorder(c, x, y)
               .split(y, yo, yi, 32)
               .parallel(yo)
               .vectorize(x, 8)
               .unroll(c);
        }
    }
};

// =============================================================================
// P14-W4-4 GO/NO-GO PROBE — isolated, reversible experiment.
//
// Sole purpose: decide whether an INPUT-side interleaved flat-1D gather
// `src_rgb(base + c)` (base = (sy*row_stride + sx)*3) lowers correctly on
// Vulkan, i.e. whether the Stage4 generator can read the Stage3 interleaved
// device buffer directly instead of the current host planar repack.
//
// This is a SEPARATE generator (separate AOT, separate signature) so it does
// NOT touch the production three-planar DngRenderStage4Android at all — maximal
// reversibility. It only does a linear8 passthrough (diag_stage 0 equivalent)
// through the SAME verified 2D-planar dst(i,c) output, isolating the src-read
// construct as the single variable under test.
//
// Gotcha context: #92 = interleaved 3D-buffer channel-stride aliasing (R==G);
// #93 = OUTPUT-side `idx*3+c` mis-lowers under split+gpu_tile. The INPUT-side
// `idx*3+c` gather is the untested construct this probe settles.
// =============================================================================
class DngRenderStage4AndroidProbe
    : public Halide::Generator<DngRenderStage4AndroidProbe> {
public:
    // Single flat interleaved src: contents = SDK interleaved RGB buffer
    // (row-major, channel stride 1) laid out as one 1D plane of size
    // src_row_stride_px * src_height * 3.
    Input<Buffer<uint16_t>> src_rgb{"src_rgb", 1};
    Input<int32_t> src_width{"src_width"};
    Input<int32_t> src_height{"src_height"};
    Input<int32_t> dst_width{"dst_width"};
    Input<int32_t> src_row_stride_px{"src_row_stride_px"};
    Input<int32_t> crop_l{"crop_l"};
    Input<int32_t> crop_t{"crop_t"};
    Input<float> src_scale{"src_scale"};

    // Same verified 2D-planar output as W4-3 (b): dim0=i (stride1), dim1=c
    // (stride N). The dst construct is held constant vs the production kernel so
    // the only variable under test is the interleaved src read.
    Output<Buffer<uint8_t>> dst{"dst", 2};

    Expr out8_r, out8_g, out8_b;

    void generate() {
        Var i("i");

        dst.dim(0).set_stride(1);
        dst.dim(1).set_bounds(0, 3);
        dst.dim(1).set_stride(dst.dim(0).extent());

        Expr dst_x = i % dst_width;
        Expr dst_y = i / dst_width;
        Expr sx = clamp(dst_x + crop_l, 0, src_width - 1);
        Expr sy = clamp(dst_y + crop_t, 0, src_height - 1);

        // INPUT-side interleaved gather under test:
        Expr base = (sy * src_row_stride_px + sx) * 3;
        Expr max_idx = src_rgb.dim(0).extent() - 1;
        Expr s_r = cast<float>(src_rgb(clamp(base + 0, 0, max_idx))) * src_scale;
        Expr s_g = cast<float>(src_rgb(clamp(base + 1, 0, max_idx))) * src_scale;
        Expr s_b = cast<float>(src_rgb(clamp(base + 2, 0, max_idx))) * src_scale;

        auto linear8 = [&](Expr v) {
            return cast<uint8_t>(clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
        };

        out8_r = linear8(s_r);
        out8_g = linear8(s_g);
        out8_b = linear8(s_b);

        Var c("c");
        dst(i, c) = select(c == 0, out8_r, c == 1, out8_g, out8_b);
    }

    void schedule() {
        Var i("i"), c("c");
        if (get_target().has_gpu_feature()) {
            Var io("io"), ii("ii");
            dst.bound(c, 0, 3)
               .reorder(c, i)
               .gpu_tile(i, io, ii, 256)
               .unroll(c);
        } else {
            Var io("io"), ii("ii");
            dst.bound(c, 0, 3)
               .reorder(c, i)
               .split(i, io, ii, 1024)
               .parallel(io)
               .vectorize(ii, 8)
               .unroll(c);
        }
    }
};

// =============================================================================
// Sized (box-filter downscaling) Stage4 generator — macOS/Metal round 1.
//
// Purpose: `targetWidth` / sized decode (docs/logs/2026-08-23/
// targetwidth-sized-decode-handover.md §4 row 6, §5.2). The production
// `dng_render_stage4` AOT must stay bit-identical because its output SHAs are
// pinned gate artifacts (Gotcha #99), so this is a *separate* generator +
// separate AOT rather than a scale path bolted into the existing kernel.
//
// Semantics: identical to DngRenderStage4 (same inputs, same RGBA8 dst layout,
// same render math) except that the rendered result is box-averaged down to
// the requested output size. The averaging happens AFTER the full colour
// math, in float, and BEFORE the single uint8 quantisation — so the kernel
// converges on "box downscale of the full-resolution 8-bit output", which is
// the acceptance reference (contract AC7). Averaging on the source side
// instead would NOT converge on that reference, because the tone curve and
// encode gamma between the two points are non-linear.
//
// Consequence to be aware of: the colour math still evaluates at sensor
// resolution, so this variant shrinks the output buffer but does not shrink
// the render workload.
//
// Cell convention: output pixel x covers source columns
// [x*src_w/dst_width, (x+1)*src_w/dst_width) — an exact integer-ratio box, so
// the cells tile the source exactly and neighbouring cells differ in size by
// at most one pixel. The RDom is sized to the worst-case cell (its bounds
// depend only on the inputs, never on x/y, as Halide requires) and the surplus
// taps are masked out by `in_cell`; the divisor is the true cell area.
//
// The accumulation is a single Tuple-valued update rather than three separate
// sum() reductions, so CSE shares the (expensive, channel-independent) render
// pipeline body across R/G/B instead of evaluating it three times per source
// pixel — the same reasoning as the `unroll(c) + select` note above.
//
// NOTE (round 1 scope): macOS/Metal only. The Vulkan branches below are
// carried over verbatim from DngRenderStage4 so the class stays a faithful
// base, but this generator is NOT wired into the Android AOT and has not been
// checked against Gotcha #93 / #96 on a real Adreno device. Do not claim
// Android coverage from a macOS green.
// =============================================================================
class DngRenderStage4Scaled : public Halide::Generator<DngRenderStage4Scaled> {
public:
    Input<Buffer<uint16_t>> src{"src", 3};          // x, y, c (Stage3 output)
    Input<float> src_scale{"src_scale"};            // usually 1 / 65535
    // Requested output size. Must match the extents of `dst`; passed
    // explicitly so the box geometry never depends on output bounds
    // inference.
    Input<int32_t> dst_width{"dst_width"};
    Input<int32_t> dst_height{"dst_height"};
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
    Output<Buffer<uint8_t>> dst{"dst", 3};          // x, y, c
    Func rendered_rgb{"rendered_rgb"};   // full-res, float, pre-quantisation
    Func box_acc{"box_acc"};             // per-output-pixel cell accumulator

    void generate() {
        Var x("x"), y("y"), c("c");

        if (uses_vulkan_planar_layout(get_target())) {
            src.dim(0).set_stride(1);
            src.dim(1).set_stride(src.dim(0).extent());
            src.dim(2).set_bounds(0, 3);
            src.dim(2).set_stride(src.dim(0).extent() * src.dim(1).extent());
        } else {
            src.dim(0).set_stride(3);
            src.dim(2).set_bounds(0, 3);
            src.dim(2).set_stride(1);
        }
        if (uses_vulkan_planar_layout(get_target())) {
            dst.dim(0).set_stride(1);
            dst.dim(1).set_stride(dst.dim(0).extent());
            dst.dim(2).set_bounds(0, 3);
            dst.dim(2).set_stride(dst.dim(0).extent() * dst.dim(1).extent());
        } else {
            dst.dim(0).set_stride(4);
            dst.dim(2).set_bounds(0, 4);
            dst.dim(2).set_stride(1);
        }

        Func src_f("src_f");
        src_f(x, y, c) = src(x, y, c);

        // Source reads are at FULL resolution and identical to
        // DngRenderStage4 — the downscale happens after the colour math.
        Expr sx = clamp(x, 0, src.dim(0).extent() - 1);
        Expr sy = clamp(y, 0, src.dim(1).extent() - 1);
        Expr s_r = cast<float>(src_f(sx, sy, 0)) * src_scale;
        Expr s_g = cast<float>(src_f(sx, sy, 1)) * src_scale;
        Expr s_b = cast<float>(src_f(sx, sy, 2)) * src_scale;

        Expr wb_r = min(s_r, camera_white(0));
        Expr wb_g = min(s_g, camera_white(1));
        Expr wb_b = min(s_b, camera_white(2));

        Expr p_r0_rg = wb_r * camera_to_rgb(0, 0) + wb_g * camera_to_rgb(1, 0);
        Expr p_g0_rg = wb_r * camera_to_rgb(0, 1) + wb_g * camera_to_rgb(1, 1);
        Expr p_b0_rg = wb_r * camera_to_rgb(0, 2) + wb_g * camera_to_rgb(1, 2);
        Expr p_r0_sum = p_r0_rg + wb_b * camera_to_rgb(2, 0);
        Expr p_g0_sum = p_g0_rg + wb_b * camera_to_rgb(2, 1);
        Expr p_b0_sum = p_b0_rg + wb_b * camera_to_rgb(2, 2);
        Expr p_r0 = clamp(p_r0_sum, 0.0f, 1.0f);
        Expr p_g0 = clamp(p_g0_sum, 0.0f, 1.0f);
        Expr p_b0 = clamp(p_b0_sum, 0.0f, 1.0f);

        Expr abc_r = p_r0;
        Expr abc_g = p_g0;
        Expr abc_b = p_b0;

        auto table_interp = [&](const auto& table, Expr v) {
            Expr xv = clamp(v, 0.0f, 1.0f);
            Expr max_idx = table.dim(0).extent() - 2;
            Expr yv = xv * cast<float>(max_idx);
            Expr idx = clamp(cast<int>(floor(yv)), 0, max_idx);
            Expr frac = yv - cast<float>(idx);
            Expr a = table(idx);
            Expr b = table(idx + 1);
            return a * (1.0f - frac) + b * frac;
        };

        auto rgb_to_hsv = [&](Expr r_, Expr g_, Expr b_, Expr& h, Expr& s, Expr& v) {
            v = max(r_, max(g_, b_));
            Expr mn = min(r_, min(g_, b_));
            Expr gap = v - mn;

            Expr gap_den = select(gap > 0.0f, gap, 1.0f);
            Expr h_r = (g_ - b_) / gap_den;
            Expr h_r_fix = select(h_r < 0.0f, h_r + 6.0f, h_r);
            Expr h_g = 2.0f + (b_ - r_) / gap_den;
            Expr h_b = 4.0f + (r_ - g_) / gap_den;

            h = select(gap > 0.0f,
                       select(r_ == v, h_r_fix,
                              g_ == v, h_g,
                                       h_b),
                       0.0f);
            s = select(gap > 0.0f, gap / v, 0.0f);
        };

        auto hsv_to_rgb = [&](Expr h, Expr s, Expr v, Expr& r_, Expr& g_, Expr& b_) {
            Expr use_sat = s > 0.0f;
            Expr hh = select(h < 0.0f, h + 6.0f, h);
            hh = select(hh >= 6.0f, hh - 6.0f, hh);
            Expr i = cast<int>(hh);
            Expr f = hh - cast<float>(i);
            Expr p = v * (1.0f - s);
            Expr q = v * (1.0f - s * f);
            Expr t = v * (1.0f - s * (1.0f - f));
            Expr cc = clamp(i, 0, 5);

            Expr r_hsv = select(cc == 0, v,
                                cc == 1, q,
                                cc == 2, p,
                                cc == 3, p,
                                cc == 4, t,
                                         v);
            Expr g_hsv = select(cc == 0, t,
                                cc == 1, v,
                                cc == 2, v,
                                cc == 3, q,
                                cc == 4, p,
                                         p);
            Expr b_hsv = select(cc == 0, p,
                                cc == 1, p,
                                cc == 2, t,
                                cc == 3, v,
                                cc == 4, v,
                                         q);

            // Match DNG SDK semantics: if saturation is non-positive, emit gray.
            r_ = select(use_sat, r_hsv, v);
            g_ = select(use_sat, g_hsv, v);
            b_ = select(use_sat, b_hsv, v);
        };

        auto sample_hsv_map = [&](const auto& table,
                                  const auto& encode_table,
                                  const auto& decode_table,
                                  Expr hue_div,
                                  Expr sat_div,
                                  Expr val_div,
                                  Expr has_table,
                                  Expr has_encoding,
                                  Expr r_,
                                  Expr g_,
                                  Expr b_,
                                  Expr& out_r,
                                  Expr& out_g,
                                  Expr& out_b) {
            Expr h, s, v;
            rgb_to_hsv(r_, g_, b_, h, s, v);

            // select() is not short-circuit in Halide; always keep dimensions valid.
            Expr hue_div_safe = max(hue_div, 2);
            Expr sat_div_safe = max(sat_div, 2);
            Expr val_div_safe = max(val_div, 1);

            Expr hue_scale = cast<float>(hue_div_safe) * (1.0f / 6.0f);
            Expr sat_scale = cast<float>(sat_div_safe - 1);
            Expr val_scale = cast<float>(val_div_safe - 1);
            Expr max_hue_index0 = hue_div_safe - 1;
            Expr max_sat_index0 = sat_div_safe - 2;
            Expr max_val_index0 = val_div_safe - 2;
            Expr hue_step = sat_div_safe;
            Expr val_step = hue_div_safe * sat_div_safe;

            Expr v_encoded0 = v;
            Expr use_encode = (has_encoding != 0) && (val_div_safe >= 2);
            Expr v_encoded = select(use_encode, table_interp(encode_table, clamp(v, 0.0f, 1.0f)), v_encoded0);

            Expr h_scaled = h * hue_scale;
            Expr s_scaled = s * sat_scale;
            Expr v_scaled = v_encoded * val_scale;

            Expr h_index0_raw = cast<int>(floor(h_scaled));
            Expr s_index0 = clamp(cast<int>(floor(s_scaled)), 0, cast<int>(max_sat_index0));
            Expr v_index0 = clamp(cast<int>(floor(v_scaled)), 0, cast<int>(max_val_index0));

            Expr h_index0 = clamp(h_index0_raw, 0, cast<int>(max_hue_index0));
            Expr h_index1 = select(h_index0_raw >= max_hue_index0, 0, h_index0 + 1);

            Expr h_fract1 = h_scaled - cast<float>(h_index0);
            Expr s_fract1 = s_scaled - cast<float>(s_index0);
            Expr v_fract1 = v_scaled - cast<float>(v_index0);
            Expr h_fract0 = 1.0f - h_fract1;
            Expr s_fract0 = 1.0f - s_fract1;
            Expr v_fract0 = 1.0f - v_fract1;

            auto tval = [&](Expr idx, int comp) {
                return table(clamp(cast<int>(idx), 0, table.dim(0).extent() - 1), comp);
            };

            Expr base2d0 = h_index0 * hue_step + s_index0;
            Expr base2d1 = h_index1 * hue_step + s_index0;

            Expr hs_hue0 = h_fract0 * tval(base2d0, 0) + h_fract1 * tval(base2d1, 0);
            Expr hs_sat0 = h_fract0 * tval(base2d0, 1) + h_fract1 * tval(base2d1, 1);
            Expr hs_val0 = h_fract0 * tval(base2d0, 2) + h_fract1 * tval(base2d1, 2);

            Expr hs_hue1 = h_fract0 * tval(base2d0 + 1, 0) + h_fract1 * tval(base2d1 + 1, 0);
            Expr hs_sat1 = h_fract0 * tval(base2d0 + 1, 1) + h_fract1 * tval(base2d1 + 1, 1);
            Expr hs_val1 = h_fract0 * tval(base2d0 + 1, 2) + h_fract1 * tval(base2d1 + 1, 2);

            Expr hue_shift_2d = s_fract0 * hs_hue0 + s_fract1 * hs_hue1;
            Expr sat_scale_2d = s_fract0 * hs_sat0 + s_fract1 * hs_sat1;
            Expr val_scale_2d = s_fract0 * hs_val0 + s_fract1 * hs_val1;

            Expr base3d00 = v_index0 * val_step + h_index0 * hue_step + s_index0;
            Expr base3d01 = v_index0 * val_step + h_index1 * hue_step + s_index0;
            Expr base3d10 = base3d00 + val_step;
            Expr base3d11 = base3d01 + val_step;

            auto lerp_hv = [&](int comp, Expr off) {
                return v_fract0 * (h_fract0 * tval(base3d00 + off, comp) + h_fract1 * tval(base3d01 + off, comp)) +
                       v_fract1 * (h_fract0 * tval(base3d10 + off, comp) + h_fract1 * tval(base3d11 + off, comp));
            };

            Expr hue_shift0_3d = lerp_hv(0, 0);
            Expr sat_scale0_3d = lerp_hv(1, 0);
            Expr val_scale0_3d = lerp_hv(2, 0);
            Expr hue_shift1_3d = lerp_hv(0, 1);
            Expr sat_scale1_3d = lerp_hv(1, 1);
            Expr val_scale1_3d = lerp_hv(2, 1);

            Expr hue_shift_3d = s_fract0 * hue_shift0_3d + s_fract1 * hue_shift1_3d;
            Expr sat_scale_3d = s_fract0 * sat_scale0_3d + s_fract1 * sat_scale1_3d;
            Expr val_scale_3d = s_fract0 * val_scale0_3d + s_fract1 * val_scale1_3d;

            Expr use_2d = val_div_safe < 2;
            Expr hue_shift = hue_shift_3d;
            Expr sat_mult = sat_scale_3d;
            Expr val_mult = val_scale_3d;

            Expr hh = h + hue_shift * (6.0f / 360.0f);
            Expr ss = min(s * sat_mult, 1.0f);
            Expr ve = clamp(v_encoded * val_mult, 0.0f, 1.0f);
            Expr vv = select(use_encode, table_interp(decode_table, ve), ve);

            Expr rr, gg, bb;
            hsv_to_rgb(hh, ss, vv, rr, gg, bb);

            out_r = select(has_table != 0, rr, r_);
            out_g = select(has_table != 0, gg, g_);
            out_b = select(has_table != 0, bb, b_);
        };

        Expr p_r1, p_g1, p_b1;
        sample_hsv_map(huesat_table,
                       huesat_encode,
                       huesat_decode,
                       huesat_hue_div,
                       huesat_sat_div,
                       huesat_val_div,
                       huesat_has_table,
                       huesat_has_encoding,
                       abc_r,
                       abc_g,
                       abc_b,
                       p_r1,
                       p_g1,
                       p_b1);

        Expr e_r = table_interp(exp_ramp, p_r1);
        Expr e_g = table_interp(exp_ramp, p_g1);
        Expr e_b = table_interp(exp_ramp, p_b1);

        Expr p_r2, p_g2, p_b2;
        sample_hsv_map(look_table,
                       look_encode,
                       look_decode,
                       look_hue_div,
                       look_sat_div,
                       look_val_div,
                       look_has_table,
                       look_has_encoding,
                       e_r,
                       e_g,
                       e_b,
                       p_r2,
                       p_g2,
                       p_b2);

        auto rgb_tone = [&](Expr r_, Expr g_, Expr b_, Expr& rr, Expr& gg, Expr& bb) {
            Expr tr = table_interp(tone_curve, r_);
            Expr tg = table_interp(tone_curve, g_);
            Expr tb = table_interp(tone_curve, b_);

            Expr rr1 = tr;
            Expr den1 = select((r_ >= g_) && (g_ > b_), r_ - b_, 1.0f);
            Expr gg1 = tb + ((tr - tb) * (g_ - b_) / den1);
            Expr bb1 = tb;

            // Case 2: b > r >= g (RGBTone(b, r, g, bb, rr, gg))
            Expr bb2 = tb;
            Expr gg2 = tg;
            Expr den2 = select((r_ >= g_) && !(g_ > b_) && (b_ > r_), b_ - g_, 1.0f);
            Expr rr2 = gg2 + ((bb2 - gg2) * (r_ - g_) / den2);

            // Case 3: r >= b > g (RGBTone(r, b, g, rr, bb, gg))
            Expr rr3 = tr;
            Expr gg3 = tg;
            Expr den3 = select((r_ >= g_) && !(g_ > b_) && !(b_ > r_) && (b_ > g_), r_ - g_, 1.0f);
            Expr bb3 = gg3 + ((rr3 - gg3) * (b_ - g_) / den3);

            Expr rr4 = tr;
            Expr gg4 = tg;
            Expr bb4 = tg;

            // Case 5: g > r >= b (RGBTone(g, r, b, gg, rr, bb))
            Expr gg5 = tg;
            Expr bb5 = tb;
            Expr den5 = select(!(r_ >= g_) && (r_ >= b_), g_ - b_, 1.0f);
            Expr rr5 = bb5 + ((gg5 - bb5) * (r_ - b_) / den5);

            // Case 6: b > g > r (RGBTone(b, g, r, bb, gg, rr))
            Expr bb6 = tb;
            Expr rr6 = tr;
            Expr den6 = select(!(r_ >= g_) && !(r_ >= b_) && (b_ > g_), b_ - r_, 1.0f);
            Expr gg6 = rr6 + ((bb6 - rr6) * (g_ - r_) / den6);

            // Case 7: g >= b > r (RGBTone(g, b, r, gg, bb, rr))
            Expr gg7 = tg;
            Expr rr7 = tr;
            Expr den7 = select(!(r_ >= g_) && !(r_ >= b_) && !(b_ > g_), g_ - r_, 1.0f);
            Expr bb7 = rr7 + ((gg7 - rr7) * (b_ - r_) / den7);

            Expr c1 = (r_ >= g_) && (g_ > b_);
            Expr c2 = (r_ >= g_) && !(g_ > b_) && (b_ > r_);
            Expr c3 = (r_ >= g_) && !(g_ > b_) && !(b_ > r_) && (b_ > g_);
            Expr c4 = (r_ >= g_) && !(g_ > b_) && !(b_ > r_) && !(b_ > g_);
            Expr c5 = !(r_ >= g_) && (r_ >= b_);
            Expr c6 = !(r_ >= g_) && !(r_ >= b_) && (b_ > g_);

            rr = select(c1, rr1,
                        c2, rr2,
                        c3, rr3,
                        c4, rr4,
                        c5, rr5,
                        c6, rr6,
                            rr7);
            gg = select(c1, gg1,
                        c2, gg2,
                        c3, gg3,
                        c4, gg4,
                        c5, gg5,
                        c6, gg6,
                            gg7);
            bb = select(c1, bb1,
                        c2, bb2,
                        c3, bb3,
                        c4, bb4,
                        c5, bb5,
                        c6, bb6,
                            bb7);
        };

        Expr t_r, t_g, t_b;
        rgb_tone(p_r2, p_g2, p_b2, t_r, t_g, t_b);

        // Keep accumulation order deterministic at the matrix stage.
        Expr tone_r = t_r;
        Expr tone_g = t_g;
        Expr tone_b = t_b;
        Expr f_r_rg = tone_r * rgb_to_final(0, 0) + tone_g * rgb_to_final(1, 0);
        Expr f_g_rg = tone_r * rgb_to_final(0, 1) + tone_g * rgb_to_final(1, 1);
        Expr f_b_rg = tone_r * rgb_to_final(0, 2) + tone_g * rgb_to_final(1, 2);
        Expr f_r_sum = f_r_rg + tone_b * rgb_to_final(2, 0);
        Expr f_g_sum = f_g_rg + tone_b * rgb_to_final(2, 1);
        Expr f_b_sum = f_b_rg + tone_b * rgb_to_final(2, 2);

        Expr f_r = clamp(f_r_sum, 0.0f, 1.0f);
        Expr f_g = clamp(f_g_sum, 0.0f, 1.0f);
        Expr f_b = clamp(f_b_sum, 0.0f, 1.0f);

        auto encode8 = [&](Expr v) {
            Expr g_ = table_interp(encode_gamma, v);
            return clamp(g_ * 255.0f + 0.5f, 0.0f, 255.0f);
        };

        // Same value as DngRenderStage4's output, but kept in float and NOT
        // yet quantised. The `+ 0.5f` rounding bias from encode8 survives the
        // averaging, so the single truncating cast below is a round-to-nearest.
        rendered_rgb(x, y) = Tuple(encode8(f_r), encode8(f_g), encode8(f_b));

        // ---- box-filter downscale of the rendered result ----------------
        Expr src_w = src.dim(0).extent();
        Expr src_h = src.dim(1).extent();
        Expr dw = max(dst_width, 1);
        Expr dh = max(dst_height, 1);
        Expr xx = clamp(x, 0, dw - 1);
        Expr yy = clamp(y, 0, dh - 1);

        // Exact integer-ratio cell. When the requested size is >= the source
        // size this degenerates to a single tap per output pixel, i.e. the
        // kernel is a plain passthrough render at 1:1.
        Expr x0 = (xx * src_w) / dw;
        Expr x1 = ((xx + 1) * src_w) / dw;
        Expr y0 = (yy * src_h) / dh;
        Expr y1 = ((yy + 1) * src_h) / dh;
        Expr cnt_x = max(x1 - x0, 1);
        Expr cnt_y = max(y1 - y0, 1);

        // RDom bounds depend only on the inputs, never on x/y — required.
        Expr max_cw = (src_w + dw - 1) / dw + 1;
        Expr max_ch = (src_h + dh - 1) / dh + 1;
        RDom r(0, max_cw, 0, max_ch, "r");
        Expr in_cell = (r.x < cnt_x) && (r.y < cnt_y);
        Expr rsx = clamp(x0 + r.x, 0, src_w - 1);
        Expr rsy = clamp(y0 + r.y, 0, src_h - 1);

        // One Tuple-valued update, so CSE evaluates the render pipeline once
        // per source pixel and shares it across R/G/B.
        box_acc(x, y) = Tuple(0.0f, 0.0f, 0.0f);
        box_acc(x, y) = Tuple(
            box_acc(x, y)[0] + select(in_cell, rendered_rgb(rsx, rsy)[0], 0.0f),
            box_acc(x, y)[1] + select(in_cell, rendered_rgb(rsx, rsy)[1], 0.0f),
            box_acc(x, y)[2] + select(in_cell, rendered_rgb(rsx, rsy)[2], 0.0f));

        Expr inv_area = 1.0f / cast<float>(cnt_x * cnt_y);
        Expr o_r = cast<uint8_t>(clamp(box_acc(x, y)[0] * inv_area, 0.0f, 255.0f));
        Expr o_g = cast<uint8_t>(clamp(box_acc(x, y)[1] * inv_area, 0.0f, 255.0f));
        Expr o_b = cast<uint8_t>(clamp(box_acc(x, y)[2] * inv_area, 0.0f, 255.0f));

        if (uses_vulkan_planar_layout(get_target())) {
            dst(x, y, c) = select(c == 0, o_r,
                                  c == 1, o_g,
                                          o_b);
        } else {
            dst(x, y, c) = select(c == 0, o_r,
                                  c == 1, o_g,
                                  c == 2, o_b,
                                          cast<uint8_t>(255));
        }
    }

    void schedule() {
        Var x("x"), y("y"), c("c");
        // `rendered_rgb` stays inline inside box_acc's update so that no
        // full-resolution float intermediate is ever materialised (that would
        // be ~3x the size of the RGBA output we are trying to shrink).
        if (get_target().has_gpu_feature()) {
            Var xo("xo"), yo("yo"), xi("xi"), yi("yi");
            dst.bound(c, 0, 4)
               .reorder(c, x, y)
               .gpu_tile(x, y, xo, yo, xi, yi, 16, 16)
               .unroll(c);
            box_acc.compute_at(dst, xo).gpu_threads(x, y);
            box_acc.update(0).gpu_threads(x, y);
        } else {
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, 4)
               .reorder(c, x, y)
               .split(y, yo, yi, 32)
               .parallel(yo)
               .unroll(c);
            box_acc.compute_at(dst, yi);
        }
    }
};

// =============================================================================
// VARIANT A — pre-average sized Stage4 generator (macOS/Metal).
//
// This is the SECOND of two deliberately co-existing scaled kernels. It is
// NOT dead code and it is NOT a duplicate: the two differ only in WHERE the
// box filter sits relative to the colour math, and that single difference is
// the open design question this round exists to let the user decide.
//
//   dng_render_stage4_scaled        (Variant B, above) — averages AFTER the
//     colour math. Satisfies frozen contract AC7 (whose reference is a box
//     downscale of the full-resolution 8-bit output) on all content, but the
//     colour pipeline still evaluates at sensor resolution, so it forfeits the
//     handover §3.1 ~192 ms Stage4 saving.
//
//   dng_render_stage4_scaled_preavg (Variant A, this class) — averages the
//     u16 Stage3 source BEFORE the colour math, so the matrix / HueSat / Look
//     / tone-curve pipeline evaluates at OUTPUT resolution. That is where the
//     ~192 ms comes from. Because the tone curve and encode gamma are
//     non-linear, average-then-render != render-then-average, so it diverges
//     from AC7's reference by an amount driven by intra-cell contrast:
//     measured 27.4 dB on a high-contrast synthetic source but 60-67 dB on a
//     smooth one. Its AC7 failure is content-dependent, not absolute.
//
// Both are emitted as independent AOTs so `dng_render_stage4` itself stays
// bit-identical — its output SHAs are pinned gate artifacts (Gotcha #99).
//
// Cell convention (identical in both): output pixel x covers source columns
// [x*src_w/dst_width, (x+1)*src_w/dst_width), an exact integer-ratio box, so
// cells tile the source exactly and neighbours differ by at most one pixel.
// The RDom is sized to the worst-case cell — its bounds depend only on the
// inputs, never on x/y, as Halide requires — and surplus taps are masked by
// `in_cell`; the divisor is the true cell area.
//
// NOTE (deliberate): `box_avg` is left inline here, so the three channel reads
// each expand their own accumulation loop. Correctness first; hoisting the
// three channels into one pass is a scheduling follow-up.
//
// NOTE (scope): macOS/Metal only. The Vulkan branches below are carried over
// verbatim from DngRenderStage4 so the class stays a faithful base, but this
// generator is NOT wired into the Android AOT and has not been checked against
// Gotcha #93 / #96 on a real Adreno device. Do not claim Android coverage from
// a macOS green.
// =============================================================================
class DngRenderStage4ScaledPreAvg : public Halide::Generator<DngRenderStage4ScaledPreAvg> {
public:
    // Sub-tile tail safety. ON by default: with -no_asserts-no_bounds_query the
    // default (ShiftInwards) tail writes a full tile into a smaller buffer.
    // Compiles on Metal AND on the Android Vulkan target (finding F-V3-9).
    GeneratorParam<bool> guard_tail{"guard_tail", true};

    Input<Buffer<uint16_t>> src{"src", 3};          // x, y, c (Stage3 output)
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
    // Requested output size, in UNORIENTED geometry (i.e. equal to
    // unoriented_width/unoriented_height). The box cells tile the source, so
    // this must NOT be the swapped `dst` extent for a transposing orientation.
    Input<int32_t> out_w{"out_w"};
    Input<int32_t> out_h{"out_h"};
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
    Output<Buffer<uint8_t>> dst{"dst", 3};          // x, y, c
    Func rendered_rgb{"rendered_rgb"};
    Func box_avg{"box_avg"};

    void generate() {
        Var x("x"), y("y"), c("c");

        if (uses_vulkan_planar_layout(get_target())) {
            src.dim(0).set_stride(1);
            src.dim(1).set_stride(src.dim(0).extent());
            src.dim(2).set_bounds(0, 3);
            src.dim(2).set_stride(src.dim(0).extent() * src.dim(1).extent());
        } else {
            src.dim(0).set_stride(3);
            src.dim(2).set_bounds(0, 3);
            src.dim(2).set_stride(1);
        }
        if (uses_vulkan_planar_layout(get_target())) {
            dst.dim(0).set_stride(1);
            dst.dim(1).set_stride(dst.dim(0).extent());
            dst.dim(2).set_bounds(0, 3);
            dst.dim(2).set_stride(dst.dim(0).extent() * dst.dim(1).extent());
        } else {
            dst.dim(0).set_stride(4);
            dst.dim(2).set_bounds(0, 4);
            dst.dim(2).set_stride(1);
        }

        Func src_f("src_f");
        src_f(x, y, c) = src(x, y, c);

        // Fused EXIF orientation: permute the OUTPUT coordinate back to the
        // unoriented coordinate before any pixel math runs. Every arithmetic
        // expression below is untouched, so the fused result is a pure index
        // permutation of the unfused one -> byte-exact by construction.
        // Table mirrors native/tests/oracle/ceyx_orient_oracle.cpp exactly.
        //   1: (x, y)              5: (y, x)
        //   2: (W-1-x, y)          6: (y, H-1-x)
        //   3: (W-1-x, H-1-y)      7: (W-1-y, H-1-x)
        //   4: (x, H-1-y)          8: (W-1-y, x)
        // Cases 5-8 transpose, so the caller sizes dst as (H, W).
        //
        // AFFINE FORM (T7b). The kernel decides NOTHING about orientation. The
        // host computes six int32 coefficients with ceyx_orient_affine_coeffs()
        // (native/include/ceyx_orient.h) and the kernel evaluates one integer
        // multiply-add per axis. There is deliberately no select, no boolean,
        // no cast-from-bool and no comparison against an orientation value
        // anywhere below; the unoriented extents are folded into the c terms on
        // the host, so no extent scalar reaches the kernel either.
        // Two earlier IN-KERNEL formulations mis-lowered on Adreno 750 / Vulkan
        // while their CPU controls were 8/8 correct on the same text:
        //   F-T6-1 (Task_t6_android_device_gate.md) 8-way equality chain of
        //     selects -- asked 4,5,6 it produced the image for 3, asked 8 it
        //     produced 7, byte-exactly. Suspected CSE collapse across select
        //     arms sharing a value expression.
        //   F-T8-1 (Task_t8_android_device_gate.md) branch-free three-flag form
        //     -- 30/40 byte-compares mismatched; every orientation with at
        //     least one flag set behaved as if more flags were set, and only
        //     the all-clear and all-set cases survived. Consistent with the
        //     bool-to-int conversion lowering to selects internally.
        // DO NOT re-land either form on any backend, and do not reintroduce an
        // orientation scalar here: the branch belongs on the host.
        Expr ux = orient_a_x * x + orient_b_x * y + orient_c_x;
        Expr uy = orient_a_y * x + orient_b_y * y + orient_c_y;
        // ---- end fused orientation permutation ----

        // ---- box-filter downscale of the Stage3 source -----------------
        // The permuted (unoriented) output coordinate drives the box geometry,
        // so a transposing orientation averages exactly the same source cell it
        // does today and only the destination index is permuted.
        Expr src_w = src.dim(0).extent();
        Expr src_h = src.dim(1).extent();
        Expr ow = max(out_w, 1);
        Expr oh = max(out_h, 1);
        Expr xx = clamp(ux, 0, ow - 1);
        Expr yy = clamp(uy, 0, oh - 1);

        // Exact integer-ratio cell. When out >= src this degenerates to a
        // single tap per output pixel (count clamped to >= 1), i.e. the kernel
        // is a no-op passthrough at 1:1.
        Expr x0 = (xx * src_w) / ow;
        Expr x1 = ((xx + 1) * src_w) / ow;
        Expr y0 = (yy * src_h) / oh;
        Expr y1 = ((yy + 1) * src_h) / oh;
        Expr cnt_x = max(x1 - x0, 1);
        Expr cnt_y = max(y1 - y0, 1);

        // RDom bounds depend only on the inputs, never on x/y — required.
        Expr max_cw = (src_w + ow - 1) / ow + 1;
        Expr max_ch = (src_h + oh - 1) / oh + 1;
        RDom r(0, max_cw, 0, max_ch, "r");
        Expr in_cell = (r.x < cnt_x) && (r.y < cnt_y);
        Expr rsx = clamp(x0 + r.x, 0, src_w - 1);
        Expr rsy = clamp(y0 + r.y, 0, src_h - 1);

        box_avg(x, y, c) =
            sum(select(in_cell, cast<float>(src_f(rsx, rsy, c)), 0.0f)) /
            cast<float>(cnt_x * cnt_y);

        Expr s_r = box_avg(x, y, 0) * src_scale;
        Expr s_g = box_avg(x, y, 1) * src_scale;
        Expr s_b = box_avg(x, y, 2) * src_scale;
        // ---- end box filter; everything below matches DngRenderStage4 ----

        Expr wb_r = min(s_r, camera_white(0));
        Expr wb_g = min(s_g, camera_white(1));
        Expr wb_b = min(s_b, camera_white(2));

        Expr p_r0_rg = wb_r * camera_to_rgb(0, 0) + wb_g * camera_to_rgb(1, 0);
        Expr p_g0_rg = wb_r * camera_to_rgb(0, 1) + wb_g * camera_to_rgb(1, 1);
        Expr p_b0_rg = wb_r * camera_to_rgb(0, 2) + wb_g * camera_to_rgb(1, 2);
        Expr p_r0_sum = p_r0_rg + wb_b * camera_to_rgb(2, 0);
        Expr p_g0_sum = p_g0_rg + wb_b * camera_to_rgb(2, 1);
        Expr p_b0_sum = p_b0_rg + wb_b * camera_to_rgb(2, 2);
        Expr p_r0 = clamp(p_r0_sum, 0.0f, 1.0f);
        Expr p_g0 = clamp(p_g0_sum, 0.0f, 1.0f);
        Expr p_b0 = clamp(p_b0_sum, 0.0f, 1.0f);

        Expr abc_r = p_r0;
        Expr abc_g = p_g0;
        Expr abc_b = p_b0;

        auto table_interp = [&](const auto& table, Expr v) {
            Expr xv = clamp(v, 0.0f, 1.0f);
            Expr max_idx = table.dim(0).extent() - 2;
            Expr yv = xv * cast<float>(max_idx);
            Expr idx = clamp(cast<int>(floor(yv)), 0, max_idx);
            Expr frac = yv - cast<float>(idx);
            Expr a = table(idx);
            Expr b = table(idx + 1);
            return a * (1.0f - frac) + b * frac;
        };

        auto rgb_to_hsv = [&](Expr r_, Expr g_, Expr b_, Expr& h, Expr& s, Expr& v) {
            v = max(r_, max(g_, b_));
            Expr mn = min(r_, min(g_, b_));
            Expr gap = v - mn;

            Expr gap_den = select(gap > 0.0f, gap, 1.0f);
            Expr h_r = (g_ - b_) / gap_den;
            Expr h_r_fix = select(h_r < 0.0f, h_r + 6.0f, h_r);
            Expr h_g = 2.0f + (b_ - r_) / gap_den;
            Expr h_b = 4.0f + (r_ - g_) / gap_den;

            h = select(gap > 0.0f,
                       select(r_ == v, h_r_fix,
                              g_ == v, h_g,
                                       h_b),
                       0.0f);
            s = select(gap > 0.0f, gap / v, 0.0f);
        };

        auto hsv_to_rgb = [&](Expr h, Expr s, Expr v, Expr& r_, Expr& g_, Expr& b_) {
            Expr use_sat = s > 0.0f;
            Expr hh = select(h < 0.0f, h + 6.0f, h);
            hh = select(hh >= 6.0f, hh - 6.0f, hh);
            Expr i = cast<int>(hh);
            Expr f = hh - cast<float>(i);
            Expr p = v * (1.0f - s);
            Expr q = v * (1.0f - s * f);
            Expr t = v * (1.0f - s * (1.0f - f));
            Expr cc = clamp(i, 0, 5);

            Expr r_hsv = select(cc == 0, v,
                                cc == 1, q,
                                cc == 2, p,
                                cc == 3, p,
                                cc == 4, t,
                                         v);
            Expr g_hsv = select(cc == 0, t,
                                cc == 1, v,
                                cc == 2, v,
                                cc == 3, q,
                                cc == 4, p,
                                         p);
            Expr b_hsv = select(cc == 0, p,
                                cc == 1, p,
                                cc == 2, t,
                                cc == 3, v,
                                cc == 4, v,
                                         q);

            // Match DNG SDK semantics: if saturation is non-positive, emit gray.
            r_ = select(use_sat, r_hsv, v);
            g_ = select(use_sat, g_hsv, v);
            b_ = select(use_sat, b_hsv, v);
        };

        auto sample_hsv_map = [&](const auto& table,
                                  const auto& encode_table,
                                  const auto& decode_table,
                                  Expr hue_div,
                                  Expr sat_div,
                                  Expr val_div,
                                  Expr has_table,
                                  Expr has_encoding,
                                  Expr r_,
                                  Expr g_,
                                  Expr b_,
                                  Expr& out_r,
                                  Expr& out_g,
                                  Expr& out_b) {
            Expr h, s, v;
            rgb_to_hsv(r_, g_, b_, h, s, v);

            // select() is not short-circuit in Halide; always keep dimensions valid.
            Expr hue_div_safe = max(hue_div, 2);
            Expr sat_div_safe = max(sat_div, 2);
            Expr val_div_safe = max(val_div, 1);

            Expr hue_scale = cast<float>(hue_div_safe) * (1.0f / 6.0f);
            Expr sat_scale = cast<float>(sat_div_safe - 1);
            Expr val_scale = cast<float>(val_div_safe - 1);
            Expr max_hue_index0 = hue_div_safe - 1;
            Expr max_sat_index0 = sat_div_safe - 2;
            Expr max_val_index0 = val_div_safe - 2;
            Expr hue_step = sat_div_safe;
            Expr val_step = hue_div_safe * sat_div_safe;

            Expr v_encoded0 = v;
            Expr use_encode = (has_encoding != 0) && (val_div_safe >= 2);
            Expr v_encoded = select(use_encode, table_interp(encode_table, clamp(v, 0.0f, 1.0f)), v_encoded0);

            Expr h_scaled = h * hue_scale;
            Expr s_scaled = s * sat_scale;
            Expr v_scaled = v_encoded * val_scale;

            Expr h_index0_raw = cast<int>(floor(h_scaled));
            Expr s_index0 = clamp(cast<int>(floor(s_scaled)), 0, cast<int>(max_sat_index0));
            Expr v_index0 = clamp(cast<int>(floor(v_scaled)), 0, cast<int>(max_val_index0));

            Expr h_index0 = clamp(h_index0_raw, 0, cast<int>(max_hue_index0));
            Expr h_index1 = select(h_index0_raw >= max_hue_index0, 0, h_index0 + 1);

            Expr h_fract1 = h_scaled - cast<float>(h_index0);
            Expr s_fract1 = s_scaled - cast<float>(s_index0);
            Expr v_fract1 = v_scaled - cast<float>(v_index0);
            Expr h_fract0 = 1.0f - h_fract1;
            Expr s_fract0 = 1.0f - s_fract1;
            Expr v_fract0 = 1.0f - v_fract1;

            auto tval = [&](Expr idx, int comp) {
                return table(clamp(cast<int>(idx), 0, table.dim(0).extent() - 1), comp);
            };

            Expr base2d0 = h_index0 * hue_step + s_index0;
            Expr base2d1 = h_index1 * hue_step + s_index0;

            Expr hs_hue0 = h_fract0 * tval(base2d0, 0) + h_fract1 * tval(base2d1, 0);
            Expr hs_sat0 = h_fract0 * tval(base2d0, 1) + h_fract1 * tval(base2d1, 1);
            Expr hs_val0 = h_fract0 * tval(base2d0, 2) + h_fract1 * tval(base2d1, 2);

            Expr hs_hue1 = h_fract0 * tval(base2d0 + 1, 0) + h_fract1 * tval(base2d1 + 1, 0);
            Expr hs_sat1 = h_fract0 * tval(base2d0 + 1, 1) + h_fract1 * tval(base2d1 + 1, 1);
            Expr hs_val1 = h_fract0 * tval(base2d0 + 1, 2) + h_fract1 * tval(base2d1 + 1, 2);

            Expr hue_shift_2d = s_fract0 * hs_hue0 + s_fract1 * hs_hue1;
            Expr sat_scale_2d = s_fract0 * hs_sat0 + s_fract1 * hs_sat1;
            Expr val_scale_2d = s_fract0 * hs_val0 + s_fract1 * hs_val1;

            Expr base3d00 = v_index0 * val_step + h_index0 * hue_step + s_index0;
            Expr base3d01 = v_index0 * val_step + h_index1 * hue_step + s_index0;
            Expr base3d10 = base3d00 + val_step;
            Expr base3d11 = base3d01 + val_step;

            auto lerp_hv = [&](int comp, Expr off) {
                return v_fract0 * (h_fract0 * tval(base3d00 + off, comp) + h_fract1 * tval(base3d01 + off, comp)) +
                       v_fract1 * (h_fract0 * tval(base3d10 + off, comp) + h_fract1 * tval(base3d11 + off, comp));
            };

            Expr hue_shift0_3d = lerp_hv(0, 0);
            Expr sat_scale0_3d = lerp_hv(1, 0);
            Expr val_scale0_3d = lerp_hv(2, 0);
            Expr hue_shift1_3d = lerp_hv(0, 1);
            Expr sat_scale1_3d = lerp_hv(1, 1);
            Expr val_scale1_3d = lerp_hv(2, 1);

            Expr hue_shift_3d = s_fract0 * hue_shift0_3d + s_fract1 * hue_shift1_3d;
            Expr sat_scale_3d = s_fract0 * sat_scale0_3d + s_fract1 * sat_scale1_3d;
            Expr val_scale_3d = s_fract0 * val_scale0_3d + s_fract1 * val_scale1_3d;

            Expr use_2d = val_div_safe < 2;
            Expr hue_shift = hue_shift_3d;
            Expr sat_mult = sat_scale_3d;
            Expr val_mult = val_scale_3d;

            Expr hh = h + hue_shift * (6.0f / 360.0f);
            Expr ss = min(s * sat_mult, 1.0f);
            Expr ve = clamp(v_encoded * val_mult, 0.0f, 1.0f);
            Expr vv = select(use_encode, table_interp(decode_table, ve), ve);

            Expr rr, gg, bb;
            hsv_to_rgb(hh, ss, vv, rr, gg, bb);

            out_r = select(has_table != 0, rr, r_);
            out_g = select(has_table != 0, gg, g_);
            out_b = select(has_table != 0, bb, b_);
        };

        Expr p_r1, p_g1, p_b1;
        sample_hsv_map(huesat_table,
                       huesat_encode,
                       huesat_decode,
                       huesat_hue_div,
                       huesat_sat_div,
                       huesat_val_div,
                       huesat_has_table,
                       huesat_has_encoding,
                       abc_r,
                       abc_g,
                       abc_b,
                       p_r1,
                       p_g1,
                       p_b1);

        Expr e_r = table_interp(exp_ramp, p_r1);
        Expr e_g = table_interp(exp_ramp, p_g1);
        Expr e_b = table_interp(exp_ramp, p_b1);

        Expr p_r2, p_g2, p_b2;
        sample_hsv_map(look_table,
                       look_encode,
                       look_decode,
                       look_hue_div,
                       look_sat_div,
                       look_val_div,
                       look_has_table,
                       look_has_encoding,
                       e_r,
                       e_g,
                       e_b,
                       p_r2,
                       p_g2,
                       p_b2);

        auto rgb_tone = [&](Expr r_, Expr g_, Expr b_, Expr& rr, Expr& gg, Expr& bb) {
            Expr tr = table_interp(tone_curve, r_);
            Expr tg = table_interp(tone_curve, g_);
            Expr tb = table_interp(tone_curve, b_);

            Expr rr1 = tr;
            Expr den1 = select((r_ >= g_) && (g_ > b_), r_ - b_, 1.0f);
            Expr gg1 = tb + ((tr - tb) * (g_ - b_) / den1);
            Expr bb1 = tb;

            // Case 2: b > r >= g (RGBTone(b, r, g, bb, rr, gg))
            Expr bb2 = tb;
            Expr gg2 = tg;
            Expr den2 = select((r_ >= g_) && !(g_ > b_) && (b_ > r_), b_ - g_, 1.0f);
            Expr rr2 = gg2 + ((bb2 - gg2) * (r_ - g_) / den2);

            // Case 3: r >= b > g (RGBTone(r, b, g, rr, bb, gg))
            Expr rr3 = tr;
            Expr gg3 = tg;
            Expr den3 = select((r_ >= g_) && !(g_ > b_) && !(b_ > r_) && (b_ > g_), r_ - g_, 1.0f);
            Expr bb3 = gg3 + ((rr3 - gg3) * (b_ - g_) / den3);

            Expr rr4 = tr;
            Expr gg4 = tg;
            Expr bb4 = tg;

            // Case 5: g > r >= b (RGBTone(g, r, b, gg, rr, bb))
            Expr gg5 = tg;
            Expr bb5 = tb;
            Expr den5 = select(!(r_ >= g_) && (r_ >= b_), g_ - b_, 1.0f);
            Expr rr5 = bb5 + ((gg5 - bb5) * (r_ - b_) / den5);

            // Case 6: b > g > r (RGBTone(b, g, r, bb, gg, rr))
            Expr bb6 = tb;
            Expr rr6 = tr;
            Expr den6 = select(!(r_ >= g_) && !(r_ >= b_) && (b_ > g_), b_ - r_, 1.0f);
            Expr gg6 = rr6 + ((bb6 - rr6) * (g_ - r_) / den6);

            // Case 7: g >= b > r (RGBTone(g, b, r, gg, bb, rr))
            Expr gg7 = tg;
            Expr rr7 = tr;
            Expr den7 = select(!(r_ >= g_) && !(r_ >= b_) && !(b_ > g_), g_ - r_, 1.0f);
            Expr bb7 = rr7 + ((gg7 - rr7) * (b_ - r_) / den7);

            Expr c1 = (r_ >= g_) && (g_ > b_);
            Expr c2 = (r_ >= g_) && !(g_ > b_) && (b_ > r_);
            Expr c3 = (r_ >= g_) && !(g_ > b_) && !(b_ > r_) && (b_ > g_);
            Expr c4 = (r_ >= g_) && !(g_ > b_) && !(b_ > r_) && !(b_ > g_);
            Expr c5 = !(r_ >= g_) && (r_ >= b_);
            Expr c6 = !(r_ >= g_) && !(r_ >= b_) && (b_ > g_);

            rr = select(c1, rr1,
                        c2, rr2,
                        c3, rr3,
                        c4, rr4,
                        c5, rr5,
                        c6, rr6,
                            rr7);
            gg = select(c1, gg1,
                        c2, gg2,
                        c3, gg3,
                        c4, gg4,
                        c5, gg5,
                        c6, gg6,
                            gg7);
            bb = select(c1, bb1,
                        c2, bb2,
                        c3, bb3,
                        c4, bb4,
                        c5, bb5,
                        c6, bb6,
                            bb7);
        };

        Expr t_r, t_g, t_b;
        rgb_tone(p_r2, p_g2, p_b2, t_r, t_g, t_b);

        // Keep accumulation order deterministic at the matrix stage.
        Expr tone_r = t_r;
        Expr tone_g = t_g;
        Expr tone_b = t_b;
        Expr f_r_rg = tone_r * rgb_to_final(0, 0) + tone_g * rgb_to_final(1, 0);
        Expr f_g_rg = tone_r * rgb_to_final(0, 1) + tone_g * rgb_to_final(1, 1);
        Expr f_b_rg = tone_r * rgb_to_final(0, 2) + tone_g * rgb_to_final(1, 2);
        Expr f_r_sum = f_r_rg + tone_b * rgb_to_final(2, 0);
        Expr f_g_sum = f_g_rg + tone_b * rgb_to_final(2, 1);
        Expr f_b_sum = f_b_rg + tone_b * rgb_to_final(2, 2);

        Expr f_r = clamp(f_r_sum, 0.0f, 1.0f);
        Expr f_g = clamp(f_g_sum, 0.0f, 1.0f);
        Expr f_b = clamp(f_b_sum, 0.0f, 1.0f);

        auto encode8 = [&](Expr v) {
            Expr g_ = table_interp(encode_gamma, v);
            return clamp(g_ * 255.0f + 0.5f, 0.0f, 255.0f);
        };

        rendered_rgb(x, y) = Tuple(cast<uint8_t>(encode8(f_r)),
                                   cast<uint8_t>(encode8(f_g)),
                                   cast<uint8_t>(encode8(f_b)));

        if (uses_vulkan_planar_layout(get_target())) {
            dst(x, y, c) = select(c == 0, rendered_rgb(x, y)[0],
                                  c == 1, rendered_rgb(x, y)[1],
                                          rendered_rgb(x, y)[2]);
        } else {
            dst(x, y, c) = select(c == 0, rendered_rgb(x, y)[0],
                                  c == 1, rendered_rgb(x, y)[1],
                                  c == 2, rendered_rgb(x, y)[2],
                                          cast<uint8_t>(255));
        }
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
            rendered_rgb.compute_at(dst, xo)
                        .gpu_threads(x, y);
        } else {
            Var yo("yo"), yi("yi");
            dst.bound(c, 0, 4)
               .reorder(c, x, y)
               .split(y, yo, yi, 32)
               .parallel(yo)
               .unroll(c);
            rendered_rgb.compute_at(dst, yo);
        }
    }
};

// =============================================================================
// mem8 v3 T12 — the yuv420 OUTPUT VARIANTS of the two Stage-4 families.
//
// These are ARM B (two-stage), which the plan requires first because it is the
// arm every platform needs: the non-split class serves the Metal family, the
// split class serves the Vulkan family (Android / Windows / Linux). Arm A —
// the same output variant grafted onto T20's fused kernel — is a separate
// generator and a separate commit.
//
// WHY TWO CLASSES AND NOT A GeneratorParam. A Halide generator cannot switch
// output ARITY on a GeneratorParam, and yuv420 is three outputs where RGBA8 is
// one. That is also why the format is never an Expr inside a kernel: format
// selection is the host choosing which AOT entry to call.
//
// WHAT IS SHARED, AND WHY THAT IS THE WHOLE POINT
//   * the colour arithmetic — dng_render_stage4_expr.h (non-split, extracted by
//     T20) and dng_render_stage4_split_expr.h (split, extracted by T12-pre).
//     Each yuv420 class calls the SAME header its RGBA8 sibling calls, so a
//     colour-science change cannot land on one output format and not the other.
//   * the plane math — ceyx_yuv420_write.h, ONE implementation used by both
//     classes here and by arm A. The arms may differ in where the RGB comes
//     from; they must not differ in how yuv420 is written (test Y7, and the
//     standing no-divergence ruling).
//
// LAYOUT: shape 1 — three separate 2-D uint8 outputs, each tightly packed
// (row stride == plane width, asserted here by set_stride(1) on dim 0). The
// HOST computes the three plane offsets into one contiguous caller-owned
// destination and passes three halide_buffer_t views over it, so the FFI
// destination stays a single buffer and the frozen byte-count formula
// w*h + 2*ceil(w/2)*ceil(h/2) (raw_ffi_api.h) is unchanged. Shape 2 — one
// output with a computed linear plane index — is BARRED: that is plan (a) /
// W4-1, already recorded as mis-lowering on Vulkan (Gotcha #93, :604-606).
//
// NO STAGED PRODUCER on either class, for the reason the class comment above
// DngRenderStage4Android records: a Func materialised at a GPU loop level
// becomes a Workgroup array, and this driver miscompiles that array below
// 32 bits — silently. The colour body is inlined, which costs ~2x evaluation
// (once for luma, once more per contributing chroma pixel) and is the price of
// the shape; check_gpu_producer_width.py is the mechanical check that no
// producer sneaks in.
// =============================================================================
class DngRenderStage4Yuv420 : public Halide::Generator<DngRenderStage4Yuv420> {
public:
    GeneratorParam<bool> guard_tail{"guard_tail", true};

    Input<Buffer<uint16_t>> src{"src", 3};          // x, y, c
    Input<float> src_scale{"src_scale"};
    Input<int32_t> orient_a_x{"orient_a_x"};
    Input<int32_t> orient_b_x{"orient_b_x"};
    Input<int32_t> orient_c_x{"orient_c_x"};
    Input<int32_t> orient_a_y{"orient_a_y"};
    Input<int32_t> orient_b_y{"orient_b_y"};
    Input<int32_t> orient_c_y{"orient_c_y"};
    Input<Buffer<float>> exp_ramp{"exp_ramp", 1};
    Input<Buffer<float>> tone_curve{"tone_curve", 1};
    Input<Buffer<float>> encode_gamma{"encode_gamma", 1};
    Input<Buffer<float>> camera_white{"camera_white", 1};
    Input<Buffer<float>> camera_to_rgb{"camera_to_rgb", 2};
    Input<Buffer<float>> rgb_to_final{"rgb_to_final", 2};
    Input<Buffer<float>> huesat_table{"huesat_table", 2};
    Input<Buffer<float>> huesat_encode{"huesat_encode", 1};
    Input<Buffer<float>> huesat_decode{"huesat_decode", 1};
    Input<int32_t> huesat_hue_div{"huesat_hue_div"};
    Input<int32_t> huesat_sat_div{"huesat_sat_div"};
    Input<int32_t> huesat_val_div{"huesat_val_div"};
    Input<int32_t> huesat_has_table{"huesat_has_table"};
    Input<int32_t> huesat_has_encoding{"huesat_has_encoding"};
    Input<Buffer<float>> look_table{"look_table", 2};
    Input<Buffer<float>> look_encode{"look_encode", 1};
    Input<Buffer<float>> look_decode{"look_decode", 1};
    Input<int32_t> look_hue_div{"look_hue_div"};
    Input<int32_t> look_sat_div{"look_sat_div"};
    Input<int32_t> look_val_div{"look_val_div"};
    Input<int32_t> look_has_table{"look_has_table"};
    Input<int32_t> look_has_encoding{"look_has_encoding"};

    // Y, Cb, Cr. Extents are set by the caller: w x h and ceil(w/2) x ceil(h/2).
    Output<Buffer<uint8_t>> y_plane{"y_plane", 2};
    Output<Buffer<uint8_t>> cb_plane{"cb_plane", 2};
    Output<Buffer<uint8_t>> cr_plane{"cr_plane", 2};

    Func rendered_rgb{"rendered_rgb"};

    void generate() {
        Var x("x"), y("y"), c("c");

        // src layout: identical to DngRenderStage4's, because it reads the same
        // Stage-3 buffer. Backend predicate, never the OS.
        if (uses_vulkan_planar_layout(get_target())) {
            src.dim(0).set_stride(1);
            src.dim(1).set_stride(src.dim(0).extent());
            src.dim(2).set_bounds(0, 3);
            src.dim(2).set_stride(src.dim(0).extent() * src.dim(1).extent());
        } else {
            src.dim(0).set_stride(3);
            src.dim(2).set_bounds(0, 3);
            src.dim(2).set_stride(1);
        }

        // Tight packing is the frozen contract, not a Halide default we hope
        // holds: assert it on every plane.
        y_plane.dim(0).set_stride(1);
        cb_plane.dim(0).set_stride(1);
        cr_plane.dim(0).set_stride(1);

        Func src_f("src_f");
        src_f(x, y, c) = src(x, y, c);
        ceyx::build_dng_render_stage4_rgb(
            x, y,
            [&](Expr sample_x, Expr sample_y, int channel) {
                return src_f(sample_x, sample_y, channel);
            },
            src.dim(0).extent(), src.dim(1).extent(), src_scale,
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

        // build_dng_render_stage4_rgb produces this family's usual 3-Tuple
        // Func; split it into three single-value Funcs here, because the plane
        // write now takes channels un-Tupled (ceyx_yuv420_write.h, D2 root
        // cause). These three are INLINE, so the tuple expression is evaluated
        // exactly as it was before and the change is a no-op on this arm --
        // and the signature is the same one the split arm calls, so the two
        // arms still share one plane-write body.
        Func rgb8_r("rgb8_r"), rgb8_g("rgb8_g"), rgb8_b("rgb8_b");
        rgb8_r(x, y) = rendered_rgb(x, y)[0];
        rgb8_g(x, y) = rendered_rgb(x, y)[1];
        rgb8_b(x, y) = rendered_rgb(x, y)[2];

        // The luma plane's extents ARE the oriented output extents, so the
        // odd-extent edge clamp reads them from there rather than from a
        // separate scalar the host could get wrong.
        ceyx::build_yuv420_planes(x, y, rgb8_r, rgb8_g, rgb8_b,
                                  y_plane.dim(0).extent(),
                                  y_plane.dim(1).extent(),
                                  y_plane, cb_plane, cr_plane);
    }

    void schedule() {
        Var x("x"), y("y");
        scheduleYuv420Planes(get_target(), guard_tail, x, y,
                             y_plane, cb_plane, cr_plane);
    }
};

class DngRenderStage4SplitYuv420
    : public Halide::Generator<DngRenderStage4SplitYuv420> {
public:
    GeneratorParam<int32_t> diag_stage{"diag_stage", -1};
    GeneratorParam<bool> guard_tail{"guard_tail", true};

    Input<Buffer<uint16_t>> src_rgb{"src_rgb", 1};
    Input<int32_t> src_width{"src_width"};
    Input<int32_t> src_height{"src_height"};
    Input<int32_t> src_row_stride_px{"src_row_stride_px"};
    Input<int32_t> crop_l{"crop_l"};
    Input<int32_t> crop_t{"crop_t"};
    Input<float> src_scale{"src_scale"};
    Input<int32_t> orient_a_x{"orient_a_x"};
    Input<int32_t> orient_b_x{"orient_b_x"};
    Input<int32_t> orient_c_x{"orient_c_x"};
    Input<int32_t> orient_a_y{"orient_a_y"};
    Input<int32_t> orient_b_y{"orient_b_y"};
    Input<int32_t> orient_c_y{"orient_c_y"};
    Input<Buffer<float>> exp_ramp{"exp_ramp", 1};
    Input<Buffer<float>> tone_curve{"tone_curve", 1};
    Input<Buffer<float>> encode_gamma{"encode_gamma", 1};
    Input<Buffer<float>> camera_white{"camera_white", 1};
    Input<Buffer<float>> camera_to_rgb{"camera_to_rgb", 1};
    Input<Buffer<float>> rgb_to_final{"rgb_to_final", 1};
    Input<Buffer<float>> huesat_table{"huesat_table", 1};
    Input<Buffer<float>> huesat_encode{"huesat_encode", 1};
    Input<Buffer<float>> huesat_decode{"huesat_decode", 1};
    Input<int32_t> huesat_entry_count{"huesat_entry_count"};
    Input<int32_t> huesat_hue_div{"huesat_hue_div"};
    Input<int32_t> huesat_sat_div{"huesat_sat_div"};
    Input<int32_t> huesat_val_div{"huesat_val_div"};
    Input<int32_t> huesat_has_table{"huesat_has_table"};
    Input<int32_t> huesat_has_encoding{"huesat_has_encoding"};
    Input<Buffer<float>> look_table{"look_table", 1};
    Input<Buffer<float>> look_encode{"look_encode", 1};
    Input<Buffer<float>> look_decode{"look_decode", 1};
    Input<int32_t> look_entry_count{"look_entry_count"};
    Input<int32_t> look_hue_div{"look_hue_div"};
    Input<int32_t> look_sat_div{"look_sat_div"};
    Input<int32_t> look_val_div{"look_val_div"};
    Input<int32_t> look_has_table{"look_has_table"};
    Input<int32_t> look_has_encoding{"look_has_encoding"};

    Output<Buffer<uint8_t>> y_plane{"y_plane", 2};
    Output<Buffer<uint8_t>> cb_plane{"cb_plane", 2};
    Output<Buffer<uint8_t>> cr_plane{"cr_plane", 2};

    void generate() {
        Var x("x"), y("y");

        y_plane.dim(0).set_stride(1);
        cb_plane.dim(0).set_stride(1);
        cr_plane.dim(0).set_stride(1);

        // The split family's colour body produces three Exprs over (x, y). The
        // yuv420 write needs to evaluate them at the four source coordinates of
        // a chroma block, so each is wrapped in its OWN single-value Func.
        //
        // These were ONE 3-Tuple Func until 2026-09-20. That was the D2 root
        // cause: this whole kernel family exists to keep Vulkan away from Tuple
        // codegen (halide_aot.cmake:86-92), its rgba8 sibling forms no Tuple
        // (:383-389), and the Tuple here was introduced only to match the
        // plane-write signature -- which then consumed it by tuple index at
        // five coordinates per output pixel. On device that produced planes
        // disagreeing with this same device's rgba8 on 12-33% of pixels with a
        // bimodal error histogram. The signature now takes channels un-Tupled
        // and BOTH families pass three Funcs, so this is a conformance fix, not
        // a platform guard, and the two arms still share one write body.
        // All three Funcs are INLINE (never compute_at'd), so no Workgroup
        // array is materialised.
        Expr out8_r, out8_g, out8_b;
        ceyx::build_dng_render_stage4_split_rgb8(
            x, y, diag_stage,
            src_rgb, src_width, src_height, src_row_stride_px,
            crop_l, crop_t, src_scale,
            orient_a_x, orient_b_x, orient_c_x,
            orient_a_y, orient_b_y, orient_c_y,
            exp_ramp, tone_curve, encode_gamma, camera_white,
            camera_to_rgb, rgb_to_final,
            huesat_table, huesat_encode, huesat_decode,
            huesat_entry_count,
            huesat_hue_div, huesat_sat_div, huesat_val_div,
            huesat_has_table, huesat_has_encoding,
            look_table, look_encode, look_decode,
            look_entry_count,
            look_hue_div, look_sat_div, look_val_div,
            look_has_table, look_has_encoding,
            out8_r, out8_g, out8_b);

        Func rgb8_r("rgb8_r"), rgb8_g("rgb8_g"), rgb8_b("rgb8_b");
        rgb8_r(x, y) = out8_r;
        rgb8_g(x, y) = out8_g;
        rgb8_b(x, y) = out8_b;

        ceyx::build_yuv420_planes(x, y, rgb8_r, rgb8_g, rgb8_b,
                                  y_plane.dim(0).extent(),
                                  y_plane.dim(1).extent(),
                                  y_plane, cb_plane, cr_plane);
    }

    void schedule() {
        Var x("x"), y("y");
        scheduleYuv420Planes(get_target(), guard_tail, x, y,
                             y_plane, cb_plane, cr_plane);
    }
};

HALIDE_REGISTER_GENERATOR(DngRenderStage4, dng_render_stage4)
HALIDE_REGISTER_GENERATOR(DngRenderStage4Scaled, dng_render_stage4_scaled)
HALIDE_REGISTER_GENERATOR(DngRenderStage4ScaledPreAvg, dng_render_stage4_scaled_preavg)
HALIDE_REGISTER_GENERATOR(DngRenderStage4Android, dng_render_stage4_split)
HALIDE_REGISTER_GENERATOR(DngRenderStage4AndroidProbe, dng_render_stage4_split_probe)
HALIDE_REGISTER_GENERATOR(DngRenderStage4Yuv420, dng_render_stage4_yuv420)
HALIDE_REGISTER_GENERATOR(DngRenderStage4SplitYuv420, dng_render_stage4_split_yuv420)
