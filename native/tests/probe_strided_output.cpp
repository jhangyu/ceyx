/**
 * ---
 * file_summary: "Task 0 GPU strided-output feasibility probe (native-rotation spec §9)"
 * functions:
 *   - name: "cpuReferenceOrient"
 *     description: "Naive per-pixel CPU orientation reference, deliberately probe-local"
 *   - name: "transposedShapeFor"
 *     description: "Build the transposed halide_dimension_t[3] dst layout for EXIF 5/6/7/8"
 *   - name: "runDenseControl"
 *     description: "Positive control: dense RGBA8 dst through the same AOT entry"
 *   - name: "runTransposed"
 *     description: "The clause under test: same AOT entry, transposed dst layout"
 *   - name: "main"
 *     description: "Run red proof or the four-orientation P1-P4 matrix and print the verdict block"
 * ---
 *
 * probe_strided_output.cpp — Task 0 of the native EXIF rotation campaign.
 *
 * QUESTION THIS BINARY ANSWERS (spec §9): can the Stage4 Halide AOT kernel be
 * handed a TRANSPOSED host destination — so the oriented frame is produced with
 * no scratch buffer at all (Design G) — correctly (P1), with the right extents
 * (P2), without a silent host staging copy (P3), and without costing more time
 * than the Design C CPU transpose it would replace (P4)?
 *
 * Pre-registration (thresholds, route, predictions, red proof) was written
 * BEFORE this file produced any number:
 *   docs/logs/2026-09-06/gpu-transpose-probe-prereg.md
 *
 * WHY THIS PROBE CALLS THE AOT ENTRY DIRECTLY INSTEAD OF DECODING A DNG.
 * Spec §9.3 says "decode again, binding the destination as a transposed
 * halide_dimension_t layout". No production entry point permits that: the
 * public API takes a raw `uint8_t*` (dng_render_halide.h:37-43) and the
 * destination buffer is built INSIDE the pipeline by
 * `Buffer<uint8_t>::make_interleaved(dst_rgba, dst_w, dst_h, 4)`
 * (dng_render_halide.cpp:1171 split, :1187 non-split). Editing those sources is
 * outside this probe's ownership, so the transposed leg calls the generated
 * kernel entry directly with the same buffers the pipeline would build, and the
 * dense leg is run through BOTH the same direct entry (as the positive control
 * for the synthetic inputs) and the production `runRenderStage4HalideAot` (as
 * the real-behaviour baseline). This deviation is recorded in §8 of the
 * pre-registration document.
 *
 * BOTH KERNEL BRANCHES ARE COVERED, one per build directory, because the
 * destination is constructed at a different source line in each and a probe
 * that exercised only one would prove nothing about the other (spec §7 OQ-4):
 *   - non-split / Metal  -> dng_render_stage4()        (build/)
 *   - split    / Vulkan  -> dng_render_stage4_split()  (build-moltenvk/)
 * The branch is selected by DNG_STAGE4_SPLIT_KERNEL, exactly as the pipeline
 * selects it.
 *
 * Backend is chosen by DNG_GPU_BACKEND and is cached per process
 * (dng_halide_device.cpp:38-41), so ONE PROCESS PER BACKEND. This binary never
 * switches backend in-process.
 *
 * Usage:
 *   ./probe_strided_output [--redproof] [--small-only] [<dng path, optional>]
 *
 * Exit codes (self-captured by the caller with RC=$? on the next line):
 *   0  every orientation PASSED all four clauses on this backend
 *   1  at least one clause FAILED on this backend  (a legitimate probe result)
 *   2  the DENSE POSITIVE CONTROL failed -> configuration error, NOT a backend
 *      FAIL. Per spec §9.2 this must not be recorded as a Vulkan FAIL.
 *   3  --redproof ran and P1 did NOT go red -> the probe cannot fail, so no
 *      green result from it may be accepted (AC-0.7).
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/time.h>

#include "HalideBuffer.h"
#include "HalideRuntime.h"

// NOTE: the production `RenderParams` (include/dng_render_params.h) is NOT used
// here. It carries DNG SDK members (dng_1d_table, AutoPtr<dng_hue_sat_map>) that
// make it non-copyable and drag the whole SDK into this target for no benefit —
// the kernel entry only ever sees the plain float arrays below. Keeping a
// probe-local struct also keeps the probe from silently inheriting a future
// change to a production type it is supposed to be measuring against.
struct ProbeParams {
    float camera_white[3] = {1.0f, 1.0f, 1.0f};
    float camera_to_rgb[9] = {};
    float rgb_to_final[9] = {};
    std::vector<float> exp_ramp, tone_curve, encode_gamma;
    std::vector<float> huesat_table, huesat_encode, huesat_decode;
    int32_t huesat_hue_div = 0, huesat_sat_div = 0, huesat_val_div = 0;
    int32_t huesat_has_table = 0, huesat_has_encoding = 0;
    std::vector<float> look_table, look_encode, look_decode;
    int32_t look_hue_div = 0, look_sat_div = 0, look_val_div = 0;
    int32_t look_has_table = 0, look_has_encoding = 0;
};

#if defined(DNG_STAGE4_SPLIT_KERNEL)
#include "dng_render_stage4_split.h"
#else
#include "dng_render_stage4.h"
#endif

using Halide::Runtime::Buffer;

// ---------------------------------------------------------------------------
// P3 instrument #2 — halide_malloc byte counter around the D2H window.
//
// Spec §9.4 names "a malloc/halide_malloc interposition or an allocator counter
// around the D2H window" as one of the acceptable independent instruments. This
// is that counter. Instrument #1 is the peak-RSS delta below; the two are
// independent because a Metal/Vulkan staging buffer allocated by the driver
// shows up in RSS but NOT in halide_malloc, while a runtime-side host staging
// allocation shows up in both.
// ---------------------------------------------------------------------------
namespace {

volatile size_t g_halide_alloc_bytes = 0;
volatile size_t g_halide_alloc_calls = 0;

void* countingMalloc(void* /*user_context*/, size_t x) {
    g_halide_alloc_bytes += x;
    g_halide_alloc_calls += 1;
    // Mirror Halide's default alignment guarantee.
    void* p = nullptr;
    if (posix_memalign(&p, 128, x ? x : 1) != 0) return nullptr;
    return p;
}

void countingFree(void* /*user_context*/, void* ptr) { free(ptr); }

// The default Halide error handler ABORTS. That is the wrong behaviour for a
// probe: a runtime refusal is a RESULT, not a crash, and aborting on the first
// refusal would hide the verdict for every orientation after it. With a handler
// that returns, halide_error returns and the generated code propagates a
// non-zero return code instead — which is exactly the instrument P1 needs.
char g_last_error[1024] = {0};
bool g_saw_error = false;

void recordingErrorHandler(void* /*user_context*/, const char* msg) {
    g_saw_error = true;
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg ? msg : "(null)");
    // Also emit immediately: the generated code does not always survive long
    // enough after a runtime refusal for the recorded copy to be printed, and
    // the refusal text is the primary evidence for orientations 6/7/8.
    fprintf(stderr, "[HALIDE-RUNTIME-ERROR] %s\n", g_last_error);
    fflush(stderr);
}

void clearError() { g_saw_error = false; g_last_error[0] = 0; }

// Peak RSS in bytes. macOS reports ru_maxrss in BYTES, Linux in KILOBYTES.
size_t peakRssBytes() {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<size_t>(ru.ru_maxrss);
#else
    return static_cast<size_t>(ru.ru_maxrss) * 1024u;
#endif
}

double nowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

double medianOf(std::vector<double> v) {
    if (v.empty()) return -1.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// ---------------------------------------------------------------------------
// The EXIF orientation model.
//
// Decomposition is quarterTurnsCw first, THEN mirror horizontally — this
// mirrors exif_orientation.dart's dartdoc exactly, which spec §1.2 names as the
// normative table the native side MUST match.
//
//   5: qt=1 mirrored   6: qt=1 plain   7: qt=3 mirrored   8: qt=3 plain
//
// All four transpose W/H, which is precisely why they are the four this probe
// tests: they are the cases Design G claims to make free.
//
// Forward maps, for a WxH source producing an HxW output:
//   6: out(X,Y) = in(x = Y,       y = H-1-X)
//   5: out(X,Y) = in(x = Y,       y = X)          [6 then horizontal mirror]
//   8: out(X,Y) = in(x = W-1-Y,   y = X)
//   7: out(X,Y) = in(x = W-1-Y,   y = H-1-X)      [8 then horizontal mirror]
// ---------------------------------------------------------------------------

// Naive, per-pixel, deliberately NOT shared with any production helper. A
// shared helper would make the probe agree with itself (spec §9.3 step 2).
void cpuReferenceOrient(const uint8_t* src, int W, int H, int orientation,
                        std::vector<uint8_t>& out) {
    // 5/6/7/8 all transpose; anything else (used by the identity positive
    // control in --selftest) keeps the source dims. Getting this wrong is not
    // hypothetical: the first version of this function hardcoded the transposed
    // dims for every orientation, and the --selftest positive control is what
    // caught it.
    const bool transposes = (orientation >= 5 && orientation <= 8);
    const int OW = transposes ? H : W;
    const int OH = transposes ? W : H;
    out.assign(static_cast<size_t>(OW) * OH * 4, 0);
    for (int Y = 0; Y < OH; ++Y) {
        for (int X = 0; X < OW; ++X) {
            int sx = 0, sy = 0;
            switch (orientation) {
                case 5: sx = Y;         sy = X;         break;
                case 6: sx = Y;         sy = H - 1 - X; break;
                case 7: sx = W - 1 - Y; sy = H - 1 - X; break;
                case 8: sx = W - 1 - Y; sy = X;         break;
                default: sx = X; sy = Y; break;
            }
            const size_t s = (static_cast<size_t>(sy) * W + sx) * 4;
            const size_t d = (static_cast<size_t>(Y) * OW + X) * 4;
            out[d + 0] = src[s + 0];
            out[d + 1] = src[s + 1];
            out[d + 2] = src[s + 2];
            out[d + 3] = src[s + 3];
        }
    }
}

// Inverse of the forward map above, expressed as a strided layout over the
// KERNEL's coordinate space (x in [0,W), y in [0,H), c in [0,4)) addressing a
// dense oriented image of dims (OW=H, OH=W), row stride 4*H:
//
//   5: X=y,       Y=x        -> sx=+4H, sy=+4,  off=0
//   6: X=H-1-y,   Y=x        -> sx=+4H, sy=-4,  off=4*(H-1)
//   7: X=H-1-y,   Y=W-1-x    -> sx=-4H, sy=-4,  off=4*(H-1) + 4*H*(W-1)
//   8: X=y,       Y=W-1-x    -> sx=-4H, sy=+4,  off=4*H*(W-1)
//
// The explicit halide_dimension_t[3] + Buffer(ptr, 3, shape) form is used
// deliberately (spec §9.3 step 3): make_interleaved is dense by construction
// and could not express this.
struct StridedDst {
    halide_dimension_t shape[3];
    size_t origin_offset_bytes;
};

StridedDst transposedShapeFor(int W, int H, int orientation) {
    StridedDst s{};
    const int sxT = 4 * H;   // step along kernel x, in bytes/elements (uint8)
    const int syT = 4;       // step along kernel y
    int sx = sxT, sy = syT;
    size_t off = 0;
    switch (orientation) {
        case 5: sx = +sxT; sy = +syT; off = 0; break;
        case 6: sx = +sxT; sy = -syT; off = static_cast<size_t>(4) * (H - 1); break;
        case 7: sx = -sxT; sy = -syT;
                off = static_cast<size_t>(4) * (H - 1) +
                      static_cast<size_t>(4) * H * (W - 1);
                break;
        case 8: sx = -sxT; sy = +syT;
                off = static_cast<size_t>(4) * H * (W - 1);
                break;
        default: break;
    }
    s.shape[0] = halide_dimension_t{0, W, sx, 0};
    s.shape[1] = halide_dimension_t{0, H, sy, 0};
    s.shape[2] = halide_dimension_t{0, 4, 1, 0};
    s.origin_offset_bytes = off;
    return s;
}

// ---------------------------------------------------------------------------
// Synthetic inputs.
//
// The transposed leg cannot come from a real decode (see the header comment),
// so the inputs are synthesised. The DENSE POSITIVE CONTROL below uses the very
// same inputs: if the control produces a zero return code and a non-degenerate
// frame, the synthesis is validated, and any failure of the transposed leg is
// attributable to the destination layout rather than to the inputs. That is the
// whole reason the control exists.
// ---------------------------------------------------------------------------
struct Inputs {
    int W = 0, H = 0;
    std::vector<uint16_t> src;  // interleaved RGB, row stride 3*W
    ProbeParams params;
};

Inputs makeInputs(int W, int H) {
    Inputs in;
    in.W = W;
    in.H = H;
    in.src.assign(static_cast<size_t>(W) * H * 3, 0);
    // A gradient with a per-channel offset: every pixel differs from its
    // neighbours in all three channels, so a transpose bug cannot hide behind
    // a flat or symmetric image (and W != H below makes a swapped-axis bug
    // structurally visible rather than merely numerically visible).
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const size_t i = (static_cast<size_t>(y) * W + x) * 3;
            in.src[i + 0] = static_cast<uint16_t>((x * 7 + y * 3) & 0xFFFF);
            in.src[i + 1] = static_cast<uint16_t>((x * 3 + y * 11) & 0xFFFF);
            in.src[i + 2] = static_cast<uint16_t>((x * 13 + y * 5) & 0xFFFF);
        }
    }
    ProbeParams& p = in.params;
    p.camera_white[0] = p.camera_white[1] = p.camera_white[2] = 1.0f;
    // Identity 3x3 matrices.
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            p.camera_to_rgb[r * 3 + c] = (r == c) ? 1.0f : 0.0f;
            p.rgb_to_final[r * 3 + c] = (r == c) ? 1.0f : 0.0f;
        }
    // Identity ramps/curves. 1024 entries mirrors the SDK table size class.
    p.exp_ramp.resize(1024);
    p.tone_curve.resize(1024);
    p.encode_gamma.resize(1024);
    for (int i = 0; i < 1024; ++i) {
        const float t = static_cast<float>(i) / 1023.0f;
        p.exp_ramp[i] = t;
        p.tone_curve[i] = t;
        p.encode_gamma[i] = t;
    }
    // Tables present but disabled (has_table / has_encoding = 0), which is the
    // shape buildRenderParams produces for a negative with no hue/sat or look
    // map. Non-empty extents are still required: the kernel's clamped table
    // reads are generated regardless of the has_* flags.
    p.huesat_table.assign(3 * 3, 0.0f);
    p.huesat_encode.assign(256, 0.0f);
    p.huesat_decode.assign(256, 0.0f);
    p.look_table.assign(3 * 3, 0.0f);
    p.look_encode.assign(256, 0.0f);
    p.look_decode.assign(256, 0.0f);
    p.huesat_hue_div = p.huesat_sat_div = p.huesat_val_div = 1;
    p.look_hue_div = p.look_sat_div = p.look_val_div = 1;
    p.huesat_has_table = p.huesat_has_encoding = 0;
    p.look_has_table = p.look_has_encoding = 0;
    return in;
}

// One call of the generated AOT entry, with a caller-supplied destination
// buffer. `dst_buf` is bound by the caller so the dense and transposed legs
// differ in EXACTLY ONE THING: the destination layout.
//
// The return value is the kernel's own return code, captured directly at the
// call site into a local — never through a pipeline, never inferred.
int invokeKernel(const Inputs& in, Buffer<uint8_t>& dst_buf) {
    const int W = in.W, H = in.H;
    const ProbeParams& p = in.params;
    const int src_row_step = 3 * W;

#if defined(DNG_STAGE4_SPLIT_KERNEL)
    const int flat_len = src_row_step * H;
    Buffer<uint16_t> src_rgb_buf(const_cast<uint16_t*>(in.src.data()), flat_len);
    Buffer<float> exp_buf(const_cast<float*>(p.exp_ramp.data()), (int)p.exp_ramp.size());
    Buffer<float> tone_buf(const_cast<float*>(p.tone_curve.data()), (int)p.tone_curve.size());
    Buffer<float> gamma_buf(const_cast<float*>(p.encode_gamma.data()), (int)p.encode_gamma.size());
    Buffer<float> cw_buf(const_cast<float*>(p.camera_white), 3);
    Buffer<float> c2r_buf(const_cast<float*>(p.camera_to_rgb), 9);
    Buffer<float> r2f_buf(const_cast<float*>(p.rgb_to_final), 9);
    Buffer<float> hs_table_buf(const_cast<float*>(p.huesat_table.data()), (int)p.huesat_table.size());
    Buffer<float> hs_encode_buf(const_cast<float*>(p.huesat_encode.data()), (int)p.huesat_encode.size());
    Buffer<float> hs_decode_buf(const_cast<float*>(p.huesat_decode.data()), (int)p.huesat_decode.size());
    Buffer<float> look_table_buf(const_cast<float*>(p.look_table.data()), (int)p.look_table.size());
    Buffer<float> look_encode_buf(const_cast<float*>(p.look_encode.data()), (int)p.look_encode.size());
    Buffer<float> look_decode_buf(const_cast<float*>(p.look_decode.data()), (int)p.look_decode.size());

    src_rgb_buf.set_host_dirty();
    exp_buf.set_host_dirty();  tone_buf.set_host_dirty();  gamma_buf.set_host_dirty();
    cw_buf.set_host_dirty();   c2r_buf.set_host_dirty();   r2f_buf.set_host_dirty();
    hs_table_buf.set_host_dirty();  hs_encode_buf.set_host_dirty();  hs_decode_buf.set_host_dirty();
    look_table_buf.set_host_dirty(); look_encode_buf.set_host_dirty(); look_decode_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    const int rc = dng_render_stage4_split(
        src_rgb_buf.raw_buffer(), W, H, W,
        /*crop_l=*/0, /*crop_t=*/0, 1.0f / 65535.0f,
        exp_buf.raw_buffer(), tone_buf.raw_buffer(), gamma_buf.raw_buffer(),
        cw_buf.raw_buffer(), c2r_buf.raw_buffer(), r2f_buf.raw_buffer(),
        hs_table_buf.raw_buffer(), hs_encode_buf.raw_buffer(), hs_decode_buf.raw_buffer(),
        (int32_t)(p.huesat_table.size() / 3),
        p.huesat_hue_div, p.huesat_sat_div, p.huesat_val_div,
        p.huesat_has_table, p.huesat_has_encoding,
        look_table_buf.raw_buffer(), look_encode_buf.raw_buffer(), look_decode_buf.raw_buffer(),
        (int32_t)(p.look_table.size() / 3),
        p.look_hue_div, p.look_sat_div, p.look_val_div,
        p.look_has_table, p.look_has_encoding,
        dst_buf.raw_buffer());
    return rc;
#else
    halide_dimension_t src_shape[3] = {
        {0, W, 3, 0},
        {0, H, src_row_step, 0},
        {0, 3, 1, 0},
    };
    Buffer<uint16_t> src_buf(const_cast<uint16_t*>(in.src.data()), 3, src_shape);
    Buffer<float> exp_buf(const_cast<float*>(p.exp_ramp.data()), (int)p.exp_ramp.size());
    Buffer<float> tone_buf(const_cast<float*>(p.tone_curve.data()), (int)p.tone_curve.size());
    Buffer<float> gamma_buf(const_cast<float*>(p.encode_gamma.data()), (int)p.encode_gamma.size());
    Buffer<float> cw_buf(const_cast<float*>(p.camera_white), 3);
    Buffer<float> c2r_buf(const_cast<float*>(p.camera_to_rgb), 3, 3);
    Buffer<float> r2f_buf(const_cast<float*>(p.rgb_to_final), 3, 3);
    Buffer<float> hs_table_buf(const_cast<float*>(p.huesat_table.data()),
                               (int)(p.huesat_table.size() / 3), 3);
    Buffer<float> hs_encode_buf(const_cast<float*>(p.huesat_encode.data()), (int)p.huesat_encode.size());
    Buffer<float> hs_decode_buf(const_cast<float*>(p.huesat_decode.data()), (int)p.huesat_decode.size());
    Buffer<float> look_table_buf(const_cast<float*>(p.look_table.data()),
                                 (int)(p.look_table.size() / 3), 3);
    Buffer<float> look_encode_buf(const_cast<float*>(p.look_encode.data()), (int)p.look_encode.size());
    Buffer<float> look_decode_buf(const_cast<float*>(p.look_decode.data()), (int)p.look_decode.size());

    src_buf.set_host_dirty();
    exp_buf.set_host_dirty();  tone_buf.set_host_dirty();  gamma_buf.set_host_dirty();
    cw_buf.set_host_dirty();   c2r_buf.set_host_dirty();   r2f_buf.set_host_dirty();
    hs_table_buf.set_host_dirty();  hs_encode_buf.set_host_dirty();  hs_decode_buf.set_host_dirty();
    look_table_buf.set_host_dirty(); look_encode_buf.set_host_dirty(); look_decode_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    const int rc = dng_render_stage4(
        src_buf.raw_buffer(), 1.0f / 65535.0f,
        exp_buf.raw_buffer(), tone_buf.raw_buffer(), gamma_buf.raw_buffer(),
        cw_buf.raw_buffer(), c2r_buf.raw_buffer(), r2f_buf.raw_buffer(),
        hs_table_buf.raw_buffer(), hs_encode_buf.raw_buffer(), hs_decode_buf.raw_buffer(),
        p.huesat_hue_div, p.huesat_sat_div, p.huesat_val_div,
        p.huesat_has_table, p.huesat_has_encoding,
        look_table_buf.raw_buffer(), look_encode_buf.raw_buffer(), look_decode_buf.raw_buffer(),
        p.look_hue_div, p.look_sat_div, p.look_val_div,
        p.look_has_table, p.look_has_encoding,
        dst_buf.raw_buffer());
    return rc;
#endif
}

struct LegResult {
    int kernel_rc = -999;
    int copy_rc = -999;
    double wall_ms = -1.0;
    size_t rss_before = 0, rss_after = 0;
    size_t halide_bytes = 0;
    size_t halide_calls = 0;
    int ext0 = -1, ext1 = -1, ext2 = -1;
    std::string error;
    bool guard_low_touched = false;
    bool guard_high_touched = false;
};

// Dense positive control: same kernel, same inputs, dense interleaved dst.
LegResult runDenseControl(const Inputs& in, std::vector<uint8_t>& out_bytes) {
    LegResult r;
    out_bytes.assign(static_cast<size_t>(in.W) * in.H * 4, 0);
    Buffer<uint8_t> dst = Buffer<uint8_t>::make_interleaved(out_bytes.data(), in.W, in.H, 4);
    r.rss_before = peakRssBytes();
    g_halide_alloc_bytes = 0;
    g_halide_alloc_calls = 0;
    clearError();
    const double t0 = nowMs();
    r.kernel_rc = invokeKernel(in, dst);
    if (r.kernel_rc == 0) {
        r.copy_rc = dst.copy_to_host();
    }
    r.wall_ms = nowMs() - t0;
    r.rss_after = peakRssBytes();
    r.halide_bytes = g_halide_alloc_bytes;
    r.halide_calls = g_halide_alloc_calls;
    r.ext0 = dst.dim(0).extent();
    r.ext1 = dst.dim(1).extent();
    r.ext2 = dst.dim(2).extent();
    r.error = g_last_error;
    return r;
}

// The clause under test: identical inputs, transposed destination layout.
LegResult runTransposed(const Inputs& in, int orientation,
                        std::vector<uint8_t>& out_bytes) {
    LegResult r;
    const size_t n = static_cast<size_t>(in.W) * in.H * 4;
    // GUARD REGIONS.
    //
    // Not defensive padding — an instrument. The first attempt at this leg
    // SEGFAULTED (RC=139) for orientations 6/7/8. The reason is the finding
    // itself: `dst.dim(0).set_stride(4)` in the generator is compiled in as a
    // CONSTANT and the AOT code is built without assertions, so the kernel
    // neither honours nor rejects the strides the caller declares — it writes a
    // DENSE frame from the buffer's origin regardless. For orientations 6/7/8
    // the origin sits at a positive offset, so a dense write of a full frame
    // runs off the end of the allocation.
    //
    // With guard regions the same write lands in slack instead of in the heap,
    // and the guard bytes become positive, mechanical evidence of exactly what
    // happened: a kernel that respected the declared layout would leave every
    // guard byte untouched.
    const size_t kGuard = n;
    out_bytes.assign(n + 2 * kGuard, 0xA5);
    uint8_t* frame = out_bytes.data() + kGuard;
    memset(frame, 0, n);
    StridedDst s = transposedShapeFor(in.W, in.H, orientation);
    Buffer<uint8_t> dst(frame + s.origin_offset_bytes, 3, s.shape);
    r.rss_before = peakRssBytes();
    g_halide_alloc_bytes = 0;
    g_halide_alloc_calls = 0;
    clearError();
    const double t0 = nowMs();
    r.kernel_rc = invokeKernel(in, dst);
    if (r.kernel_rc == 0) {
        r.copy_rc = dst.copy_to_host();
    }
    r.wall_ms = nowMs() - t0;
    r.rss_after = peakRssBytes();
    r.halide_bytes = g_halide_alloc_bytes;
    r.halide_calls = g_halide_alloc_calls;
    r.ext0 = dst.dim(0).extent();
    r.ext1 = dst.dim(1).extent();
    r.ext2 = dst.dim(2).extent();
    r.error = g_last_error;
    for (size_t i = 0; i < kGuard; ++i) {
        if (out_bytes[i] != 0xA5) { r.guard_low_touched = true; break; }
    }
    for (size_t i = 0; i < kGuard; ++i) {
        if (out_bytes[n + kGuard + i] != 0xA5) { r.guard_high_touched = true; break; }
    }
    // Hand the caller just the frame, so the P1 comparison is against exactly
    // the bytes the destination layout declared and nothing else.
    memmove(out_bytes.data(), frame, n);
    out_bytes.resize(n);
    return r;
}

// Design C's cost, measured rather than assumed, so P4 has a real right-hand
// side. Tiled 32x32 to keep the write side inside one cache-line run, which is
// the shape spec §1.2 specifies for the fallback.
double timeCpuTransposeMs(const std::vector<uint8_t>& src, int W, int H,
                          int orientation, std::vector<uint8_t>& dst) {
    dst.assign(static_cast<size_t>(W) * H * 4, 0);
    const int OW = H;
    const double t0 = nowMs();
    const int TILE = 32;
    for (int y0 = 0; y0 < H; y0 += TILE) {
        for (int x0 = 0; x0 < W; x0 += TILE) {
            const int y1 = std::min(y0 + TILE, H);
            const int x1 = std::min(x0 + TILE, W);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    int X = 0, Y = 0;
                    switch (orientation) {
                        case 5: X = y;         Y = x;         break;
                        case 6: X = H - 1 - y; Y = x;         break;
                        case 7: X = H - 1 - y; Y = W - 1 - x; break;
                        case 8: X = y;         Y = W - 1 - x; break;
                        default: X = x; Y = y; break;
                    }
                    const size_t s = (static_cast<size_t>(y) * W + x) * 4;
                    const size_t d = (static_cast<size_t>(Y) * OW + X) * 4;
                    memcpy(&dst[d], &src[s], 4);
                }
            }
        }
    }
    return nowMs() - t0;
}

}  // namespace

// ---------------------------------------------------------------------------

// AC-0.7 red proof targets orientation 5. Orientations 6/7/8 require a NEGATIVE
// stride, and the Metal runtime refuses those outright
// ("halide_metal_device_malloc: negatives strides are illegal"), so they cannot
// carry a red proof — a refusal is not evidence that the COMPARATOR works.
// Orientation 5 is the pure transpose: all-positive strides, the one case that
// actually reaches the comparison.
constexpr int kRedproofOrientation = 5;

int main(int argc, char** argv) {
    bool redproof = false;
    bool small_only = false;
    bool selftest = false;
    // One orientation per process. Orientations 6/7/8 can take the process down
    // (negative strides are refused by the Metal device allocator and the
    // generated code does not survive the refusal), and a crash in one
    // orientation must not erase the results of the others. Running one per
    // process also means each orientation's exit status is captured
    // individually by the caller's own RC=$?, which is the discipline this
    // campaign requires of every reported code.
    int only_orientation = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--redproof") redproof = true;
        else if (a == "--small-only") small_only = true;
        else if (a == "--selftest") selftest = true;
        else if (a == "--only" && i + 1 < argc) only_orientation = atoi(argv[++i]);
    }

    // Unbuffered: this probe is expected to be able to die mid-run (that is one
    // of its results), and a buffered stdout would throw away the very line
    // that says where it died.
    setvbuf(stdout, nullptr, _IONBF, 0);

    halide_set_custom_malloc(countingMalloc);
    halide_set_custom_free(countingFree);
    halide_set_error_handler(recordingErrorHandler);

    const char* backend_env = std::getenv("DNG_GPU_BACKEND");
#if defined(DNG_STAGE4_SPLIT_KERNEL)
    const char* branch = "split (dng_render_stage4_split) [Vulkan-shaped]";
#else
    const char* branch = "non-split (dng_render_stage4) [Metal-shaped]";
#endif
    printf("=== probe_strided_output — Task 0, native-rotation spec §9 ===\n");
    printf("BACKEND_ENV=%s\n", backend_env ? backend_env : "(unset)");
    printf("KERNEL_BRANCH=%s\n", branch);
    printf("MODE=%s\n", redproof ? "REDPROOF (AC-0.7)" : "MEASURE");

    // Correctness at a small, non-square size — non-square so a swapped-axis
    // bug is structurally visible and not merely numerically visible.
    const int SW = 641, SH = 419;
    // Timing / P3 at a production-shaped frame (~92.5 MB RGBA8), which is the
    // size the memory instruments need in order to resolve one frame.
    const int PW = 5936, PH = 4104;

    Inputs small = makeInputs(SW, SH);

    // ---------------- Dense positive control ----------------
    std::vector<uint8_t> dense_small;
    LegResult dc = runDenseControl(small, dense_small);
    printf("\n[CONTROL] dense %dx%d kernel_rc=%d copy_rc=%d wall=%.2fms "
           "extents=[%d,%d,%d] halide_alloc=%zuB/%zu calls\n",
           SW, SH, dc.kernel_rc, dc.copy_rc, dc.wall_ms,
           dc.ext0, dc.ext1, dc.ext2, dc.halide_bytes, dc.halide_calls);
    bool control_nondegenerate = false;
    for (size_t i = 0; i < dense_small.size() && !control_nondegenerate; ++i)
        if (dense_small[i] != 0) control_nondegenerate = true;
    printf("[CONTROL] non_degenerate_output=%s\n", control_nondegenerate ? "yes" : "NO");

    if (dc.kernel_rc != 0 || dc.copy_rc != 0 || !control_nondegenerate) {
        printf("\nVERDICT: CONFIGURATION ERROR — the dense positive control did not "
               "produce a frame on this backend. Per spec §9.2 this is NOT a backend "
               "FAIL and must not be recorded as one. Nothing about the transposed "
               "destination has been measured.\n");
        printf("PROBE_EXIT=2\n");
        return 2;
    }

    // -----------------------------------------------------------------------
    // COMPARATOR SELF-TEST (AC-0.7, positive AND negative control).
    //
    // The red proof below shows P1 going red. On its own that is weak evidence:
    // a comparator hardwired to "FAIL" would also go red. So the comparator is
    // first shown to produce BOTH colours on data whose answer is known
    // independently of the GPU:
    //   green  — dense control bytes vs their own identity reference
    //   red    — the same bytes vs the orientation-5 reference
    // A comparator that cannot do both has not been shown to test anything.
    // -----------------------------------------------------------------------
    if (selftest) {
        std::vector<uint8_t> ident, wrong;
        cpuReferenceOrient(dense_small.data(), SW, SH, 1, ident);
        cpuReferenceOrient(dense_small.data(), SW, SH, 5, wrong);
        const bool green = (ident.size() == dense_small.size()) &&
                           memcmp(ident.data(), dense_small.data(), ident.size()) == 0;
        const bool red = !((wrong.size() == dense_small.size()) &&
                           memcmp(wrong.data(), dense_small.data(), wrong.size()) == 0);
        printf("\n[SELFTEST] comparator green on identity reference: %s\n",
               green ? "yes" : "NO");
        printf("[SELFTEST] comparator red on orientation-5 reference: %s\n",
               red ? "yes" : "NO");
        printf("[SELFTEST] RESULT: %s\n",
               (green && red)
                   ? "the P1 comparator produces BOTH colours — it is capable of "
                     "passing and capable of failing"
                   : "the P1 comparator is STUCK — no result from this probe may "
                     "be accepted");
        printf("PROBE_EXIT=%d\n", (green && red) ? 0 : 3);
        return (green && red) ? 0 : 3;
    }

    const int orientations[4] = {5, 6, 7, 8};
    bool any_fail = false;
    bool redproof_went_red = false;

    for (int oi = 0; oi < 4; ++oi) {
        const int o = orientations[oi];
        if (only_orientation != 0 && o != only_orientation) continue;

        std::vector<uint8_t> ref;
        // AC-0.7: with --redproof, orientation 6's output is compared against
        // the orientation 8 reference. P1 MUST go red. A probe that cannot be
        // shown to fail has not been shown to test anything.
        const int ref_orientation = (redproof && o == kRedproofOrientation) ? 8 : o;
        cpuReferenceOrient(dense_small.data(), SW, SH, ref_orientation, ref);

        std::vector<uint8_t> got;
        LegResult tr = runTransposed(small, o, got);

        const bool p1 = (tr.kernel_rc == 0) && (tr.copy_rc == 0) &&
                        (got.size() == ref.size()) &&
                        (memcmp(got.data(), ref.data(), ref.size()) == 0);
        // P2: the transposed layout declares an HxW oriented extent. The
        // kernel's coordinate space stays WxH; what must be true is that the
        // destination the caller receives spans H columns and W rows.
        const bool p2 = (tr.ext0 == SW) && (tr.ext1 == SH) && (tr.ext2 == 4);

        printf("\n[ORIENT %d]%s kernel_rc=%d copy_rc=%d P1=%s P2=%s "
               "extents=[%d,%d,%d]\n",
               o, (redproof && o == kRedproofOrientation) ? " (REDPROOF: compared vs orient-8 reference)" : "",
               tr.kernel_rc, tr.copy_rc, p1 ? "PASS" : "FAIL", p2 ? "PASS" : "FAIL",
               tr.ext0, tr.ext1, tr.ext2);
        if (!tr.error.empty())
            printf("[ORIENT %d] halide_runtime_error=\"%s\"\n", o, tr.error.c_str());
        printf("[ORIENT %d] guard_region_touched: low=%s high=%s "
               "(a kernel honouring the declared layout touches NEITHER)\n",
               o, tr.guard_low_touched ? "YES" : "no",
               tr.guard_high_touched ? "YES" : "no");

        if (redproof && o == kRedproofOrientation) {
            redproof_went_red = !p1;
            printf("[REDPROOF] P1 went red: %s\n", redproof_went_red ? "YES" : "NO");
            continue;
        }

        if (!p1 || !p2) {
            any_fail = true;
            printf("[ORIENT %d] P3=NOT_REACHED P4=NOT_REACHED — the kernel rejected or "
                   "mis-produced the transposed destination, so there is nothing to "
                   "time and nothing to instrument.\n", o);
            continue;
        }

        if (small_only) continue;

        // ---------------- P3 + P4 at production frame size ----------------
        Inputs big = makeInputs(PW, PH);
        std::vector<uint8_t> big_dense;
        LegResult bd = runDenseControl(big, big_dense);
        if (bd.kernel_rc != 0 || bd.copy_rc != 0) {
            any_fail = true;
            printf("[ORIENT %d] production-size dense control failed "
                   "(kernel_rc=%d copy_rc=%d) — P3/P4 not measurable\n",
                   o, bd.kernel_rc, bd.copy_rc);
            continue;
        }

        std::vector<uint8_t> cpu_out;
        std::vector<double> cpu_iters, dense_iters, trans_iters;

        // >=1 discarded warmup, then >=5 measured iterations, MEDIAN reported.
        std::vector<uint8_t> scratch;
        (void)timeCpuTransposeMs(big_dense, PW, PH, o, cpu_out);  // warmup
        for (int i = 0; i < 5; ++i)
            cpu_iters.push_back(timeCpuTransposeMs(big_dense, PW, PH, o, cpu_out));

        LegResult warm_d = runDenseControl(big, scratch);          // warmup
        (void)warm_d;
        for (int i = 0; i < 5; ++i)
            dense_iters.push_back(runDenseControl(big, scratch).wall_ms);

        std::vector<uint8_t> tbytes;
        LegResult warm_t = runTransposed(big, o, tbytes);          // warmup
        (void)warm_t;
        LegResult last_t;
        for (int i = 0; i < 5; ++i) {
            last_t = runTransposed(big, o, tbytes);
            trans_iters.push_back(last_t.wall_ms);
        }

        const double m_cpu = medianOf(cpu_iters);
        const double m_dense = medianOf(dense_iters);
        const double m_trans = medianOf(trans_iters);

        // P3 instrument 1: peak-RSS delta across the transposed run.
        const long rss_delta = (long)last_t.rss_after - (long)last_t.rss_before;
        const double frame_mb = (double)PW * PH * 4 / (1024.0 * 1024.0);
        // P3 instrument 2: halide_malloc bytes inside the transposed run.
        const bool inst1_clean = rss_delta < (long)(0.5 * PW * PH * 4);
        const bool inst2_clean = last_t.halide_bytes < (size_t)(0.5 * PW * PH * 4);
        const bool p3 = inst1_clean && inst2_clean;
        const bool p4 = m_trans <= (m_dense + m_cpu);

        printf("[ORIENT %d] P3 inst1 rss_delta=%ldB (frame=%.1fMB) clean=%s | "
               "inst2 halide_alloc=%zuB/%zu calls clean=%s => P3=%s\n",
               o, rss_delta, frame_mb, inst1_clean ? "yes" : "no",
               last_t.halide_bytes, last_t.halide_calls, inst2_clean ? "yes" : "no",
               p3 ? "PASS" : "FAIL");
        printf("[ORIENT %d] P4 median_transposed=%.2fms vs "
               "median_dense=%.2fms + median_cpu_transpose=%.2fms = %.2fms => P4=%s\n",
               o, m_trans, m_dense, m_cpu, m_dense + m_cpu, p4 ? "PASS" : "FAIL");
        printf("[ORIENT %d] (the pipeline's own [Stage4-Perf] copy_host= line is "
               "emitted above under DNG_PIPELINE_VERBOSE=1; it is the second, "
               "independent timing instrument)\n", o);

        if (!p3 || !p4) any_fail = true;
    }

    if (redproof) {
        printf("\nREDPROOF RESULT: %s\n",
               redproof_went_red
                   ? "P1 went RED against a deliberately wrong reference — the probe "
                     "is capable of failing, so a green result from it is meaningful."
                   : "P1 did NOT go red against a deliberately wrong reference — the "
                     "probe cannot fail and NO green result from it may be accepted.");
        printf("PROBE_EXIT=%d\n", redproof_went_red ? 0 : 3);
        return redproof_went_red ? 0 : 3;
    }

    printf("\nBACKEND VERDICT: %s\n", any_fail ? "FAIL" : "PASS");
    printf("(spec §9 rule: GO requires Metal PASS AND Vulkan PASS; a single backend "
           "passing is NO-GO, not partial success)\n");
    printf("PROBE_EXIT=%d\n", any_fail ? 1 : 0);
    return any_fail ? 1 : 0;
}
