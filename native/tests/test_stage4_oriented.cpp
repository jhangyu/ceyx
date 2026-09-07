/**
 * test_stage4_oriented.cpp — PRODUCTION Metal fused Stage4 EXIF-orientation
 * gate (plan Task 5 / gate G-A).
 *
 * docs/logs/2026-09-07/gpu_orient_productionization_plan.md, Task 5, and the
 * F-T6-2 content-sensitivity strengthening in
 * docs/logs/2026-09-07/Task_t6_android_device_gate.md.
 *
 * What it proves, per case:
 *   base  = kernel(orientation=1)                    // unoriented render
 *   ref   = ceyx_orient_rgba(base, o)                 // CPU-composed reference
 *   fused = kernel(orientation=o)                     // GPU fused dispatch
 *   assert fused == ref byte-for-byte, for o in 1..8.
 *
 * Cases (plan §Task 5 Behavior, exactly 4, 8 orientations each = 32/run):
 *   1. full-resolution real DNG decode           kernel=dng_render_stage4
 *   2. 8x4 synthetic sub-tile (sub-16 case)       kernel=dng_render_stage4
 *   3. 33x17 synthetic non-tile-aligned           kernel=dng_render_stage4
 *   4. thumbnail-sized real decode (sub-tile out) kernel=dng_render_stage4_scaled_preavg
 *      -- this is also the scale-then-orient ordering gate: the reference is
 *      literally dng_render_stage4_scaled_preavg(orientation=1) followed by
 *      ceyx_orient_rgba(o), so a mismatch here means the box average is not
 *      safe to run on source geometry with the permutation at the store.
 *
 * F-T6-2 content-sensitivity precondition: synthetic sources are filled with
 * an asymmetric per-pixel ramp (never constant, never symmetric under any of
 * the 8 orientations) and the harness asserts, before any byte-compare counts,
 * that the o=1 reference has many distinct byte values AND that every pair of
 * same-shape orientations (1-4 together, 5-8 together) differs somewhere. A
 * black/constant image would pass every orientation trivially and is refused.
 *
 * Device-execution proof: every fused dispatch is issued through a
 * Halide::Runtime::Buffer this harness owns directly (not through the bridge,
 * which hides the buffer), so device_interface/device can be inspected BEFORE
 * copy_to_host() collapses the evidence.
 *
 * Usage:  test_stage4_oriented <dng_path> [--repeat N]
 * Output: one CASE line per (case, orientation); one CONTENT_SENSITIVITY line
 *         per case; one SUMMARY line per repeat; final RC printed by caller.
 * Exit:   0 = every case in every repeat passed; 1 = any failure.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <dng_color_space.h>
#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_image.h>
#include <dng_info.h>
#include <dng_negative.h>
#include <dng_pixel_buffer.h>
#include <dng_render.h>

#include "HalideBuffer.h"
#include "HalideRuntimeMetal.h"

#include "ceyx_orient.h"
#include "dng_pipeline_config.h"
#include "dng_render_halide.h"
#include "dng_render_params.h"

#include "dng_render_stage4.h"
#include "dng_render_stage4_scaled_preavg.h"

using std::cerr;
using std::cout;
using std::string;
using std::vector;

namespace {

// ---------------------------------------------------------------------------
// Byte compare
// ---------------------------------------------------------------------------
struct DiffReport {
    size_t mismatches = 0;
    int max_abs_diff = 0;
    size_t first_offset = 0;
};

DiffReport compareBytes(const uint8_t* a, const uint8_t* b, size_t n) {
    DiffReport r;
    for (size_t i = 0; i < n; ++i) {
        const int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        if (d != 0) {
            if (r.mismatches == 0) r.first_offset = i;
            ++r.mismatches;
            r.max_abs_diff = std::max(r.max_abs_diff, d);
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// Synthetic Stage3 source: asymmetric per-pixel ramp so the render is
// content-sensitive (F-T6-2) independent of the camera colour profile, and
// so no two of the 8 orientations can coincide by symmetry.
// ---------------------------------------------------------------------------
void fillSyntheticStage3(vector<uint16_t>& out, int w, int h, int planes) {
    out.resize(static_cast<size_t>(w) * h * planes);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < planes; ++c) {
                // Distinct, non-linear-ish, asymmetric in x vs y vs c so a
                // transpose or mirror can never reproduce another orientation.
                const uint32_t v = (static_cast<uint32_t>(x) * 4111u +
                                    static_cast<uint32_t>(y) * 977u +
                                    static_cast<uint32_t>(c) * 251u * (x + 3u) +
                                    (x ^ (y * 7 + c))) &
                                   0xFFFFu;
                out[(static_cast<size_t>(y) * w + x) * planes + c] =
                    static_cast<uint16_t>(v);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Kernel dispatch wrappers. Both call the raw Halide AOT entry directly (not
// the host bridge) so this harness can inspect device_interface/device
// BEFORE copy_to_host() collapses that evidence (device-execution proof).
// ---------------------------------------------------------------------------
struct DispatchResult {
    int kernel_rc = -1;
    int copy_rc = -1;
    bool dev_iface_ok = false;
    bool dev_nonzero = false;
    void* dst_device = nullptr;
};

// dst must already be sized to the ORIENTED extent by the caller.
DispatchResult dispatchFull(Halide::Runtime::Buffer<uint16_t>& src_buf,
                            float src_scale, int32_t orientation,
                            int32_t unoriented_w, int32_t unoriented_h,
                            const RenderParams& p,
                            Halide::Runtime::Buffer<float>& exp_buf,
                            Halide::Runtime::Buffer<float>& tone_buf,
                            Halide::Runtime::Buffer<float>& gamma_buf,
                            Halide::Runtime::Buffer<float>& cw_buf,
                            Halide::Runtime::Buffer<float>& c2r_buf,
                            Halide::Runtime::Buffer<float>& r2f_buf,
                            Halide::Runtime::Buffer<float>& hs_table_buf,
                            Halide::Runtime::Buffer<float>& hs_encode_buf,
                            Halide::Runtime::Buffer<float>& hs_decode_buf,
                            Halide::Runtime::Buffer<float>& look_table_buf,
                            Halide::Runtime::Buffer<float>& look_encode_buf,
                            Halide::Runtime::Buffer<float>& look_decode_buf,
                            Halide::Runtime::Buffer<uint8_t>& dst) {
    DispatchResult r;
    src_buf.set_host_dirty();
    dst.set_host_dirty(false);
    r.kernel_rc = dng_render_stage4(
        src_buf.raw_buffer(), src_scale, orientation, unoriented_w, unoriented_h,
        exp_buf.raw_buffer(), tone_buf.raw_buffer(), gamma_buf.raw_buffer(),
        cw_buf.raw_buffer(), c2r_buf.raw_buffer(), r2f_buf.raw_buffer(),
        hs_table_buf.raw_buffer(), hs_encode_buf.raw_buffer(), hs_decode_buf.raw_buffer(),
        p.huesat_hue_div, p.huesat_sat_div, p.huesat_val_div, p.huesat_has_table,
        p.huesat_has_encoding, look_table_buf.raw_buffer(), look_encode_buf.raw_buffer(),
        look_decode_buf.raw_buffer(), p.look_hue_div, p.look_sat_div, p.look_val_div,
        p.look_has_table, p.look_has_encoding, dst.raw_buffer());
    if (r.kernel_rc == 0) {
        r.dev_iface_ok =
            (dst.raw_buffer()->device_interface == halide_metal_device_interface());
        r.dev_nonzero = (dst.raw_buffer()->device != 0);
        r.dst_device = reinterpret_cast<void*>(dst.raw_buffer()->device);
        r.copy_rc = dst.copy_to_host();
    }
    return r;
}

// dst must already be sized to the ORIENTED extent; out_w/out_h stay
// UNORIENTED (plan amendment 2: box geometry never depends on oriented dims).
DispatchResult dispatchScaled(Halide::Runtime::Buffer<uint16_t>& src_buf,
                              float src_scale, int32_t orientation,
                              int32_t unoriented_w, int32_t unoriented_h,
                              int32_t out_w, int32_t out_h,
                              const RenderParams& p,
                              Halide::Runtime::Buffer<float>& exp_buf,
                              Halide::Runtime::Buffer<float>& tone_buf,
                              Halide::Runtime::Buffer<float>& gamma_buf,
                              Halide::Runtime::Buffer<float>& cw_buf,
                              Halide::Runtime::Buffer<float>& c2r_buf,
                              Halide::Runtime::Buffer<float>& r2f_buf,
                              Halide::Runtime::Buffer<float>& hs_table_buf,
                              Halide::Runtime::Buffer<float>& hs_encode_buf,
                              Halide::Runtime::Buffer<float>& hs_decode_buf,
                              Halide::Runtime::Buffer<float>& look_table_buf,
                              Halide::Runtime::Buffer<float>& look_encode_buf,
                              Halide::Runtime::Buffer<float>& look_decode_buf,
                              Halide::Runtime::Buffer<uint8_t>& dst) {
    DispatchResult r;
    src_buf.set_host_dirty();
    dst.set_host_dirty(false);
    r.kernel_rc = dng_render_stage4_scaled_preavg(
        src_buf.raw_buffer(), src_scale, orientation, unoriented_w, unoriented_h,
        out_w, out_h,
        exp_buf.raw_buffer(), tone_buf.raw_buffer(), gamma_buf.raw_buffer(),
        cw_buf.raw_buffer(), c2r_buf.raw_buffer(), r2f_buf.raw_buffer(),
        hs_table_buf.raw_buffer(), hs_encode_buf.raw_buffer(), hs_decode_buf.raw_buffer(),
        p.huesat_hue_div, p.huesat_sat_div, p.huesat_val_div, p.huesat_has_table,
        p.huesat_has_encoding, look_table_buf.raw_buffer(), look_encode_buf.raw_buffer(),
        look_decode_buf.raw_buffer(), p.look_hue_div, p.look_sat_div, p.look_val_div,
        p.look_has_table, p.look_has_encoding, dst.raw_buffer());
    if (r.kernel_rc == 0) {
        r.dev_iface_ok =
            (dst.raw_buffer()->device_interface == halide_metal_device_interface());
        r.dev_nonzero = (dst.raw_buffer()->device != 0);
        r.dst_device = reinterpret_cast<void*>(dst.raw_buffer()->device);
        r.copy_rc = dst.copy_to_host();
    }
    return r;
}

// ---------------------------------------------------------------------------
// One case = one (src, unoriented dst extent, kernel family) tuple, run
// through all 8 orientations.
// ---------------------------------------------------------------------------
struct CaseSpec {
    string label;
    string kernel_name;
    bool scaled;  // true => dng_render_stage4_scaled_preavg
    int src_w, src_h, src_p;
    int dst_w, dst_h;  // UNORIENTED target extent
    vector<uint16_t> data;
};

// Runs one case through all 8 orientations. Returns pass count (0..8) and
// prints one CASE line + one CONTENT_SENSITIVITY line.
struct CaseOutcome {
    int pass = 0;
    int executed = 0;
};

// mutate_orientation, when >= 1, forces every fused dispatch to use that
// orientation instead of the requested one (mutation/red-state control,
// G-9 criterion 5). The reference still uses the REAL requested orientation,
// so the comparator can fail.
CaseOutcome runCase(const CaseSpec& spec, const RenderParams& params,
                    int32_t mutate_orientation) {
    CaseOutcome outcome;
    const size_t src_bytes = spec.data.size() * sizeof(uint16_t);
    (void)src_bytes;

    const int32_t row_step = spec.src_w * spec.src_p;
    const int32_t col_step = spec.src_p;
    const int32_t plane_step = 1;
    halide_dimension_t src_shape[3] = {
        {0, spec.src_w, col_step, 0},
        {0, spec.src_h, row_step, 0},
        {0, spec.src_p, plane_step, 0},
    };
    Halide::Runtime::Buffer<uint16_t> src_buf(
        const_cast<uint16_t*>(spec.data.data()), 3, src_shape);
    const float src_scale = 1.0f / 65535.0f;

    Halide::Runtime::Buffer<float> exp_buf(const_cast<float*>(params.exp_ramp.data()),
                                           static_cast<int>(params.exp_ramp.size()));
    Halide::Runtime::Buffer<float> tone_buf(const_cast<float*>(params.tone_curve.data()),
                                            static_cast<int>(params.tone_curve.size()));
    Halide::Runtime::Buffer<float> gamma_buf(const_cast<float*>(params.encode_gamma.data()),
                                             static_cast<int>(params.encode_gamma.size()));
    Halide::Runtime::Buffer<float> cw_buf(const_cast<float*>(params.camera_white), 3);
    Halide::Runtime::Buffer<float> c2r_buf(const_cast<float*>(params.camera_to_rgb), 3, 3);
    Halide::Runtime::Buffer<float> r2f_buf(const_cast<float*>(params.rgb_to_final), 3, 3);
    Halide::Runtime::Buffer<float> hs_table_buf(
        const_cast<float*>(params.huesat_table.data()),
        static_cast<int>(params.huesat_table.size() / 3), 3);
    Halide::Runtime::Buffer<float> hs_encode_buf(
        const_cast<float*>(params.huesat_encode.data()),
        static_cast<int>(params.huesat_encode.size()));
    Halide::Runtime::Buffer<float> hs_decode_buf(
        const_cast<float*>(params.huesat_decode.data()),
        static_cast<int>(params.huesat_decode.size()));
    Halide::Runtime::Buffer<float> look_table_buf(
        const_cast<float*>(params.look_table.data()),
        static_cast<int>(params.look_table.size() / 3), 3);
    Halide::Runtime::Buffer<float> look_encode_buf(
        const_cast<float*>(params.look_encode.data()),
        static_cast<int>(params.look_encode.size()));
    Halide::Runtime::Buffer<float> look_decode_buf(
        const_cast<float*>(params.look_decode.data()),
        static_cast<int>(params.look_decode.size()));
    exp_buf.set_host_dirty();
    tone_buf.set_host_dirty();
    gamma_buf.set_host_dirty();
    cw_buf.set_host_dirty();
    c2r_buf.set_host_dirty();
    r2f_buf.set_host_dirty();
    hs_table_buf.set_host_dirty();
    hs_encode_buf.set_host_dirty();
    hs_decode_buf.set_host_dirty();
    look_table_buf.set_host_dirty();
    look_encode_buf.set_host_dirty();
    look_decode_buf.set_host_dirty();

    const size_t std_bytes = static_cast<size_t>(spec.dst_w) * spec.dst_h * 4;
    const size_t tr_bytes = std_bytes;  // transpose swaps w/h, same byte count
    vector<uint8_t> base_std(std_bytes, 0xAB);
    vector<uint8_t> ref_rgba(std_bytes, 0);
    vector<uint8_t> fused_std(std_bytes, 0xAB);
    vector<uint8_t> fused_tr(tr_bytes, 0xAB);

    Halide::Runtime::Buffer<uint8_t> dst_std =
        Halide::Runtime::Buffer<uint8_t>::make_interleaved(
            base_std.data(), spec.dst_w, spec.dst_h, 4);
    // Cases 5-8 transpose, so the caller sizes dst as (H, W). Two persistent
    // buffers (never freed/reallocated inside the loop, mirrors
    // test_orient_fused.cpp's determinism note: freeing a Metal-backed dst
    // between dispatches was previously implicated in non-determinism).
    Halide::Runtime::Buffer<uint8_t> fused_dst_std =
        Halide::Runtime::Buffer<uint8_t>::make_interleaved(
            fused_std.data(), spec.dst_w, spec.dst_h, 4);
    Halide::Runtime::Buffer<uint8_t> fused_dst_tr =
        Halide::Runtime::Buffer<uint8_t>::make_interleaved(
            fused_tr.data(), spec.dst_h, spec.dst_w, 4);

    // --- base = kernel(orientation=1), computed once, reused as the CPU
    // orient pass's input for every o in 1..8 (plan Behavior text). ---
    DispatchResult base_r =
        spec.scaled
            ? dispatchScaled(src_buf, src_scale, 1, spec.dst_w, spec.dst_h,
                             spec.dst_w, spec.dst_h, params, exp_buf, tone_buf,
                             gamma_buf, cw_buf, c2r_buf, r2f_buf, hs_table_buf,
                             hs_encode_buf, hs_decode_buf, look_table_buf,
                             look_encode_buf, look_decode_buf, dst_std)
            : dispatchFull(src_buf, src_scale, 1, spec.dst_w, spec.dst_h, params,
                          exp_buf, tone_buf, gamma_buf, cw_buf, c2r_buf, r2f_buf,
                          hs_table_buf, hs_encode_buf, hs_decode_buf,
                          look_table_buf, look_encode_buf, look_decode_buf,
                          dst_std);
    if (base_r.kernel_rc != 0 || base_r.copy_rc != 0) {
        cout << "CASE case=" << spec.label << " o=1 size=" << spec.dst_w << "x"
             << spec.dst_h << " kernel=" << spec.kernel_name
             << " [SETUP FAIL base dispatch] kernel_rc=" << base_r.kernel_rc
             << " copy_rc=" << base_r.copy_rc << "\n";
        return outcome;
    }

    // --- content-sensitivity precondition (F-T6-2) --------------------
    std::set<uint8_t> distinct(base_std.begin(), base_std.end());
    vector<vector<uint8_t>> oriented_std(5);  // index 1..4
    vector<vector<uint8_t>> oriented_tr(9);   // index 5..8

    for (int32_t o = 1; o <= 8; ++o) {
        const bool transposes = ceyx_orientation_transposes(o) != 0;
        const int32_t ow = transposes ? spec.dst_h : spec.dst_w;
        const int32_t oh = transposes ? spec.dst_w : spec.dst_h;
        Halide::Runtime::Buffer<uint8_t>& dst_buf =
            transposes ? fused_dst_tr : fused_dst_std;
        vector<uint8_t>& fused_host = transposes ? fused_tr : fused_std;
        std::fill(fused_host.begin(), fused_host.end(), 0xAB);

        const int32_t dispatch_o =
            (mutate_orientation >= 1) ? mutate_orientation : o;
        DispatchResult r =
            spec.scaled
                ? dispatchScaled(src_buf, src_scale, dispatch_o, spec.dst_w,
                                 spec.dst_h, spec.dst_w, spec.dst_h, params,
                                 exp_buf, tone_buf, gamma_buf, cw_buf, c2r_buf,
                                 r2f_buf, hs_table_buf, hs_encode_buf,
                                 hs_decode_buf, look_table_buf, look_encode_buf,
                                 look_decode_buf, dst_buf)
                : dispatchFull(src_buf, src_scale, dispatch_o, spec.dst_w,
                              spec.dst_h, params, exp_buf, tone_buf, gamma_buf,
                              cw_buf, c2r_buf, r2f_buf, hs_table_buf,
                              hs_encode_buf, hs_decode_buf, look_table_buf,
                              look_encode_buf, look_decode_buf, dst_buf);

        std::fill(ref_rgba.begin(), ref_rgba.end(), 0);
        int32_t ref_w = 0, ref_h = 0;
        const int32_t orc = ceyx_orient_rgba(base_std.data(), ref_rgba.data(),
                                             ref_rgba.size(), spec.dst_w,
                                             spec.dst_h, o, &ref_w, &ref_h);

        outcome.executed++;
        if (r.kernel_rc != 0 || r.copy_rc != 0 || orc != 0) {
            cout << "CASE case=" << spec.label << " o=" << o << " size=" << ow
                 << "x" << oh << " kernel=" << spec.kernel_name
                 << " dev_iface=" << (r.dev_iface_ok ? "metal" : "MISMATCH")
                 << " dst_device=0x" << std::hex
                 << reinterpret_cast<uintptr_t>(r.dst_device) << std::dec
                 << " RESULT=FAIL kernel_rc=" << r.kernel_rc
                 << " copy_rc=" << r.copy_rc << " orient_rc=" << orc << "\n";
            continue;
        }

        const bool dims_ok = (ref_w == ow && ref_h == oh);
        DiffReport diff;
        if (dims_ok) diff = compareBytes(fused_host.data(), ref_rgba.data(), std_bytes);
        const bool pass = dims_ok && diff.mismatches == 0 && r.dev_iface_ok && r.dev_nonzero;
        if (pass) outcome.pass++;

        if (o >= 1 && o <= 4) oriented_std[o] = fused_host;
        if (o >= 5 && o <= 8) oriented_tr[o] = fused_host;

        cout << "CASE case=" << spec.label << " o=" << o << " size=" << ow
             << "x" << oh << " kernel=" << spec.kernel_name
             << " dev_iface=" << (r.dev_iface_ok ? "metal" : "MISMATCH")
             << " dst_device=0x" << std::hex
             << reinterpret_cast<uintptr_t>(r.dst_device) << std::dec
             << " RESULT=" << (pass ? "PASS" : "FAIL")
             << " mismatched_bytes=" << diff.mismatches
             << " max_abs_diff=" << diff.max_abs_diff;
        if (diff.mismatches != 0) cout << " first_byte_offset=" << diff.first_offset;
        if (!dims_ok) cout << " [DIMENSION MISMATCH ref=" << ref_w << "x" << ref_h << "]";
        cout << "\n";
        cout.flush();
    }

    // CONTENT_SENSITIVITY: distinct byte values on the base frame, plus
    // pairwise-differs within each same-shape orientation group.
    int pairs_checked = 0, pairs_differ = 0;
    for (int a = 1; a <= 4; ++a) {
        if (oriented_std[a].empty()) continue;
        for (int b = a + 1; b <= 4; ++b) {
            if (oriented_std[b].empty()) continue;
            ++pairs_checked;
            if (compareBytes(oriented_std[a].data(), oriented_std[b].data(), std_bytes)
                    .mismatches > 0)
                ++pairs_differ;
        }
    }
    for (int a = 5; a <= 8; ++a) {
        if (oriented_tr[a].empty()) continue;
        for (int b = a + 1; b <= 8; ++b) {
            if (oriented_tr[b].empty()) continue;
            ++pairs_checked;
            if (compareBytes(oriented_tr[a].data(), oriented_tr[b].data(), tr_bytes)
                    .mismatches > 0)
                ++pairs_differ;
        }
    }
    cout << "CONTENT_SENSITIVITY case=" << spec.label
         << " distinct_byte_values=" << distinct.size()
         << " orientation_pairs_differing=" << pairs_differ << "/" << pairs_checked
         << (mutate_orientation >= 1 ? " [SKIPPED: mutation run]" : "") << "\n";
    if (mutate_orientation < 1 && (distinct.size() < 8 || pairs_differ < pairs_checked)) {
        cout << "CONTENT_SENSITIVITY case=" << spec.label
             << " VERDICT=REFUSED (image is orientation-insensitive; byte-compares"
                " below do not count)\n";
    }

    return outcome;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <dng_path> [--repeat N]\n";
        return 1;
    }
    const string dngPath = argv[1];
    int repeat = 1;
    int32_t mutate_orientation = -1;  // -1 = disabled (mutation control opt-in via env)
    for (int i = 2; i < argc; ++i) {
        if (string(argv[i]) == "--repeat" && i + 1 < argc) {
            repeat = std::atoi(argv[++i]);
        } else if (string(argv[i]) == "--mutate-orientation" && i + 1 < argc) {
            mutate_orientation = std::atoi(argv[++i]);
        }
    }
    if (repeat < 1) repeat = 1;

    cout << "=== test_stage4_oriented (production gate G-A) ===\n";
    cout << "DNG: " << dngPath << "  repeat=" << repeat;
    if (mutate_orientation >= 1) cout << "  MUTATION_CONTROL orientation=" << mutate_orientation;
    cout << "\n";

    // -----------------------------------------------------------------
    // Decode a real DNG through the SDK once (Stage1-3), reused for the
    // "full" and "thumb" cases' source pixels and for buildRenderParams
    // (the real camera colour profile every case uses).
    // -----------------------------------------------------------------
    dng_host host;
    host.SetPreferredSize(0);
    host.SetMinimumSize(0);
    host.SetMaximumSize(0);
    AutoPtr<dng_negative> negative;
    try {
        dng_file_stream stream(dngPath.c_str());
        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);
        if (!info.IsValidDNG()) {
            cerr << "  [setup FAIL] not a valid DNG\n";
            return 1;
        }
        negative.Reset(host.Make_dng_negative());
        negative->Parse(host, stream, info);
        negative->PostParse(host, stream, info);
        negative->ReadStage1Image(host, stream, info);
        negative->BuildStage2Image(host);
        negative->BuildStage3Image(host);
    } catch (const dng_exception& e) {
        cerr << "  [setup FAIL] dng_exception " << e.ErrorCode() << "\n";
        return 1;
    } catch (...) {
        cerr << "  [setup FAIL] unknown exception during decode\n";
        return 1;
    }

    dng_image* stage3 = const_cast<dng_image*>(negative->Stage3Image());
    if (!stage3) {
        cerr << "  [setup FAIL] no Stage3 image\n";
        return 1;
    }

    dng_render renderer(host, *negative);
    renderer.SetMaximumSize(std::max(negative->DefaultCropSizeH().As_real64(),
                                     negative->DefaultCropSizeV().As_real64()));
    renderer.SetFinalPixelType(ttByte);
    renderer.SetFinalSpace(dng_space_sRGB::Get());

    const dng_rect cropArea = negative->DefaultCropArea();
    uint32_t out_w = 0, out_h = 0;
    dng_render_stage4_output_size(*negative, renderer, out_w, out_h);
    if (out_w != static_cast<uint32_t>(cropArea.W()) ||
        out_h != static_cast<uint32_t>(cropArea.H())) {
        cerr << "  [setup FAIL] Stage4 would resample; this harness's 'full' case"
                " compares the non-resampling path only\n";
        return 1;
    }

    RenderParams params;
    {
        const PipelineConfig config = PipelineConfig::loadFromEnv();
        if (!buildRenderParams(host, *negative, renderer, config, params)) {
            cerr << "  [setup FAIL] buildRenderParams failed\n";
            return 1;
        }
    }

    // Real Stage3 pixels for the "full" case.
    const uint32_t real_w = static_cast<uint32_t>(cropArea.W());
    const uint32_t real_h = static_cast<uint32_t>(cropArea.H());
    vector<uint16_t> real_stage3(static_cast<size_t>(real_w) * real_h * 3);
    {
        dng_pixel_buffer buffer;
        buffer.fArea = cropArea;
        buffer.fPlane = 0;
        buffer.fPlanes = 3;
        buffer.fPixelType = stage3->PixelType();
        buffer.fPixelSize = stage3->PixelSize();
        buffer.fData = real_stage3.data();
        buffer.fRowStep = static_cast<int32_t>(real_w * 3);
        buffer.fColStep = 3;
        buffer.fPlaneStep = 1;
        stage3->Get(buffer);
    }

    // -----------------------------------------------------------------
    // Build the 4 cases (plan Task 5 Behavior).
    // -----------------------------------------------------------------
    vector<CaseSpec> cases;
    {
        CaseSpec full;
        full.label = "full";
        full.kernel_name = "dng_render_stage4";
        full.scaled = false;
        full.src_w = static_cast<int>(real_w);
        full.src_h = static_cast<int>(real_h);
        full.src_p = 3;
        full.dst_w = static_cast<int>(real_w);
        full.dst_h = static_cast<int>(real_h);
        full.data = real_stage3;
        cases.push_back(std::move(full));
    }
    {
        CaseSpec sub;
        sub.label = "8x4";
        sub.kernel_name = "dng_render_stage4";
        sub.scaled = false;
        sub.src_w = 8;
        sub.src_h = 4;
        sub.src_p = 3;
        sub.dst_w = 8;
        sub.dst_h = 4;
        fillSyntheticStage3(sub.data, sub.src_w, sub.src_h, sub.src_p);
        cases.push_back(std::move(sub));
    }
    {
        CaseSpec nonalign;
        nonalign.label = "33x17";
        nonalign.kernel_name = "dng_render_stage4";
        nonalign.scaled = false;
        nonalign.src_w = 33;
        nonalign.src_h = 17;
        nonalign.src_p = 3;
        nonalign.dst_w = 33;
        nonalign.dst_h = 17;
        fillSyntheticStage3(nonalign.data, nonalign.src_w, nonalign.src_h, nonalign.src_p);
        cases.push_back(std::move(nonalign));
    }
    {
        CaseSpec thumb;
        thumb.label = "thumb";
        thumb.kernel_name = "dng_render_stage4_scaled_preavg";
        thumb.scaled = true;
        thumb.src_w = static_cast<int>(real_w);
        thumb.src_h = static_cast<int>(real_h);
        thumb.src_p = 3;
        // Sub-tile scaled output size (scale-then-orient ordering gate).
        thumb.dst_w = 8;
        thumb.dst_h = 6;
        thumb.data = real_stage3;
        cases.push_back(std::move(thumb));
    }

    bool allPass = true;
    for (int rep = 1; rep <= repeat; ++rep) {
        int total_executed = 0, total_pass = 0;
        for (const CaseSpec& c : cases) {
            CaseOutcome o = runCase(c, params, mutate_orientation);
            total_executed += o.executed;
            total_pass += o.pass;
        }
        const int total_fail = total_executed - total_pass;
        if (total_fail != 0) allPass = false;
        cout << "SUMMARY rep=" << rep << " executed=" << total_executed
             << " pass=" << total_pass << " fail=" << total_fail << "\n";
        cout.flush();
    }

    // Mutation control expects failures; a real run expects none.
    const bool result_pass = (mutate_orientation >= 1) ? true : allPass;
    cout << "\nSTAGE4_ORIENTED_RESULT: "
         << ((mutate_orientation >= 1) ? (allPass ? "UNEXPECTED_ALL_PASS" : "EXPECTED_FAILURES_PRESENT")
                                       : (allPass ? "PASS" : "FAIL"))
         << "\n";
    if (mutate_orientation >= 1) {
        // Mutation control's own exit code is non-zero whenever the real run
        // would have passed (it must NOT), matching the plan's mutation
        // acceptance criterion ("RC not equal to 0").
        return allPass ? 0 : 1;
    }
    return result_pass ? 0 : 1;
}
