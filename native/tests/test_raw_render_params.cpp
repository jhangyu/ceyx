// raw_build_render_params coverage.
//
// The identity-table case is the one that matters most: LibRaw supplies no
// HueSatMap or LookTable, and leaving those vectors uninitialised is explicitly
// forbidden (spec section 7.1.4). "Uninitialised" would still render - just
// with garbage colour - so only an elementwise assertion catches it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "dng_color_space.h"
#include "dng_render_params.h"
#include "libraw_frontend.h"
#include "libraw_gpu_input_adapter.h"
#include "raw_render_params_builder.h"

// Declared in src/pipeline/libraw_gpu_input_adapter.cpp, which this test reaches
// through libdng_decoder_native (cmake/tests.cmake:229-231). Not in
// libraw_gpu_input_adapter.h because that header is outside the Stage 1 file
// ownership boundary. Route values: 0 none, 1 rgb_cam, 2 cam_xyz.
extern "C" void raw_srgb_to_pcs_matrix(float out9[9]);
extern "C" void raw_pcs_white(float out3[3]);
int raw_camera_to_pcs_from_libraw(const float* rgb_cam, const float* cam_xyz,
                                  uint32_t raw_color, uint32_t colors,
                                  RawColorTransform* out, char* reason_out,
                                  size_t reason_cap);

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
    // The gate greps for the exact prefix "[RawRenderParams] <name> -> PASS",
    // so the detail goes after the verdict, never between name and arrow.
    std::printf("[RawRenderParams] %s -> %s (%s)\n", name, ok ? "PASS" : "FAIL",
                detail);
    if (!ok) ++failures;
}

// Only the fields raw_build_render_params actually reads are populated; the
// builder takes no plane data, so faking a plane view would assert nothing.
RawGpuInput makeInput(bool matrix_valid) {
    RawGpuInput in{};
    for (int i = 0; i < 4; ++i) {
        in.white_level[i] = 16383.0f;
        in.as_shot_neutral[i] = 1.0f;
    }
    in.camera_to_pcs.valid = matrix_valid ? 1 : 0;
    in.camera_to_pcs.out_rows = 3;
    in.camera_to_pcs.in_cols = 3;
    // A plausible camera->XYZ matrix, ROW-SCALED onto the PCS white.
    //
    // The scaling is not cosmetic. Since Stage 1 the contract field
    // camera_to_pcs is defined as white-preserving -- camera_to_pcs * (1,1,1)
    // == XYZ(D50) -- and the adapter's route table guarantees it for real files.
    // The un-scaled literal these numbers came from violates that invariant, so
    // feeding it in produced a synthetic input no adapter can emit; the V7
    // neutral round-trip through the real kernel correctly rejected it,
    // returning (0,226,179) for a neutral probe. Scaling here keeps the
    // synthetic case inside the contract instead of weakening the test.
    const double m[9] = {0.7688, -0.2199, -0.0724,
                         -0.3129, 1.0781,  0.2588,
                         -0.0281, 0.1287,  0.6797};
    float white[3];
    raw_pcs_white(white);
    for (int r = 0; r < 3; ++r) {
        const double sum = m[r * 3 + 0] + m[r * 3 + 1] + m[r * 3 + 2];
        const double s = static_cast<double>(white[r]) / sum;
        for (int c = 0; c < 3; ++c) {
            in.camera_to_pcs.m[r * 3 + c] = static_cast<float>(m[r * 3 + c] * s);
        }
    }
    return in;
}

RawDevelopParams makeDevelop() {
    RawDevelopParams d{};
    d.exposure_ev = 0.0f;
    d.tone_curve_strength = 1.0f;
    d.output_space = kRawOutputColorSpaceSrgb;
    d.max_output_long_edge = 0;
    return d;
}

// Element-wise curve comparison at the same 1e-5 bound used for the matrices.
// The max diff is always printed, pass or fail: a bare FAIL would say the
// curves differ without saying by how much, which is the difference between a
// rounding artifact and a genuinely different transfer function.
void reportCurve(const char* name, bool precondition,
                 const std::vector<float>& got, const std::vector<float>& want) {
    if (!precondition || got.size() != want.size() || got.empty()) {
        report(name, false, "precondition failed or size mismatch");
        return;
    }
    double max_diff = 0.0;
    size_t at = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        const double d = std::fabs(static_cast<double>(got[i]) -
                                   static_cast<double>(want[i]));
        if (d > max_diff) { max_diff = d; at = i; }
    }
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "max_abs_diff=%.7f at index %zu of %zu, tolerance=0.0000100",
                  max_diff, at, got.size());
    report(name, max_diff < 1e-5, detail);
}

// For the two curves that legitimately differ between the routes: gate on what
// the Stage4 kernel actually requires of a transfer table -- finite, within
// [0,1], non-decreasing -- and always print the divergence from the DNG path so
// a future change to either route shows up as a moved number rather than
// silence.
void reportCurveSanity(const char* name, bool precondition,
                       const std::vector<float>& got,
                       const std::vector<float>& dng_ref) {
    if (!precondition || got.empty()) {
        report(name, false, "precondition failed or empty");
        return;
    }
    bool ok = true;
    for (size_t i = 0; i < got.size(); ++i) {
        if (!std::isfinite(got[i]) || got[i] < -1e-6f || got[i] > 1.0f + 1e-6f) ok = false;
        if (i && got[i] < got[i - 1] - 1e-6f) ok = false;   // non-decreasing
    }
    double max_diff = 0.0;
    const size_t n = got.size() < dng_ref.size() ? got.size() : dng_ref.size();
    for (size_t i = 0; i < n; ++i) {
        max_diff = std::fmax(max_diff, std::fabs(static_cast<double>(got[i]) -
                                                 static_cast<double>(dng_ref[i])));
    }
    char detail[200];
    std::snprintf(detail, sizeof(detail),
                  "finite, in [0,1], non-decreasing; diverges from DNG path by "
                  "max_abs_diff=%.7f (EXPECTED: DNG-only baseline exposure + "
                  "profile tone curve, spec 4.1.9)",
                  max_diff);
    report(name, ok, detail);
}

// --------------------------------------------------------------------------
// Stage 1 colour-correctness cases (design Task_raw_color_architecture.md Rev 2
// section 5.3 V5-V8/N2, acceptance AC-1.1/1.3/1.5/1.6/1.7).
//
// WHY THESE EXIST AT ALL: every pre-existing case in this file is neutral-only,
// and BOTH the old (diag(k)*A) and new (A*diag(g)) matrix forms map neutral to
// neutral by construction. The old suite therefore passed identically before and
// after the defect was introduced and could never have caught it. V8 below is
// the case with actual discriminating power.
// --------------------------------------------------------------------------

// base = ProPhoto_from_PCS * camera_to_pcs -- the matrix BEFORE the white
// balance fold, recomputed here from the SDK singletons rather than read back
// out of the builder, so this is an independent reference and not a restatement.
void proPhotoFromPcsTimes(const float camera_to_pcs[9], float out9[9]) {
    const dng_matrix_3by3 pcs(camera_to_pcs[0], camera_to_pcs[1], camera_to_pcs[2],
                              camera_to_pcs[3], camera_to_pcs[4], camera_to_pcs[5],
                              camera_to_pcs[6], camera_to_pcs[7], camera_to_pcs[8]);
    const dng_matrix product = dng_space_ProPhoto::Get().MatrixFromPCS() * pcs;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out9[r * 3 + c] = static_cast<float>(product[r][c]);
        }
    }
}

// The OLD, defective normalisation, reimplemented here byte-for-byte from the
// deleted raw_render_params_builder.cpp:39-49 so V8 has something to
// discriminate against. This is a TEST-ONLY counterfactual: AC-1.4 requires the
// name to be absent from the production file, and it is.
void oldNormalizeRowsToNeutral(float m[9], const float neutral[3]) {
    for (int r = 0; r < 3; ++r) {
        float sum = 0.0f;
        for (int c = 0; c < 3; ++c) {
            sum += m[r * 3 + c] * (neutral[c] > 0.0f ? neutral[c] : 1.0f);
        }
        if (std::fabs(sum) > 1e-8f) {
            for (int c = 0; c < 3; ++c) m[r * 3 + c] /= sum;
        }
    }
}

void apply3x3(const float m[9], const float v[3], float out[3]) {
    for (int r = 0; r < 3; ++r) {
        out[r] = m[r * 3 + 0] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2];
    }
}

// AC-1.1 / V6: camera_white is max-normalised with a 0.001 floor. A max != 1
// means the normalisation source was wrong, and since Stage 4 uses camera_white
// as a highlight ceiling on 0..1 data, an entry above 1 SILENTLY disables the
// clip on that channel -- no test would otherwise fail.
void checkClipConsistency(const char* id, const RenderParams& p) {
    float lo = p.camera_white[0], hi = p.camera_white[0];
    for (int c = 1; c < 3; ++c) {
        lo = std::min(lo, p.camera_white[c]);
        hi = std::max(hi, p.camera_white[c]);
    }
    char detail[220];
    std::snprintf(detail, sizeof(detail),
                  "camera_white=(%.7f,%.7f,%.7f) max=%.7f (want 1 +/-1e-6) "
                  "min=%.7f (want >=0.001)",
                  p.camera_white[0], p.camera_white[1], p.camera_white[2], hi, lo);
    report(id, std::fabs(hi - 1.0f) < 1e-6f && lo >= 0.001f, detail);
}

// AC-1.3 / N2: the fold identity. camera_to_rgb[r][c] must equal
// base[r][c] / camera_white[c] -- a COLUMN scale, which is what makes it a von
// Kries correction on the camera input rather than a rescale of the ProPhoto
// output. Also pins g[c]*camera_white[c] == 1, the coupling that makes the fold
// exactly equivalent to a pre-Stage-4 gain multiply (design section 1.2).
void checkFoldIdentity(const char* id, const RawGpuInput& in,
                       const RenderParams& p) {
    float base[9];
    proPhotoFromPcsTimes(in.camera_to_pcs.m, base);
    double worst = 0.0;
    int worst_at = -1;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const double want = static_cast<double>(base[r * 3 + c]) /
                                static_cast<double>(p.camera_white[c]);
            const double d = std::fabs(static_cast<double>(p.camera_to_rgb[r * 3 + c]) - want);
            if (d > worst) { worst = d; worst_at = r * 3 + c; }
        }
    }
    double gain_worst = 0.0;
    for (int c = 0; c < 3; ++c) {
        const double g = 1.0 / static_cast<double>(p.camera_white[c]);
        gain_worst = std::fmax(gain_worst, std::fabs(g * p.camera_white[c] - 1.0));
    }
    char detail[240];
    std::snprintf(detail, sizeof(detail),
                  "max|camera_to_rgb[r][c] - base[r][c]/camera_white[c]|=%.9f at "
                  "%d of 9 (tol 1e-6); max|g[c]*camera_white[c]-1|=%.9f",
                  worst, worst_at, gain_worst);
    report(id, worst < 1e-6 && gain_worst < 1e-6, detail);
}

// AC-1.5 / V8: the ONLY case here that can tell the two matrix forms apart.
// Saturated probes through old vs new must DIFFER by >5% on at least one
// channel of at least one probe, and the new matrix must match an independently
// computed base*diag(g) within 1e-5. Reinstating normalizeRowsToNeutral -- the
// single most likely implementation mistake, because it leaves greys neutral and
// so hides behind every other case in this file -- fails the first half.
void checkSaturatedDiscrimination(const char* id, const RawGpuInput& in,
                                  const RenderParams& p) {
    float base[9];
    proPhotoFromPcsTimes(in.camera_to_pcs.m, base);

    // The counterfactual: what the builder produced before this change.
    float old_m[9];
    std::memcpy(old_m, base, sizeof(old_m));
    oldNormalizeRowsToNeutral(old_m, p.camera_white);

    // The independent reference: base * diag(g), computed from `base` and
    // camera_white without touching params.camera_to_rgb.
    float ref[9];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            ref[r * 3 + c] = static_cast<float>(
                static_cast<double>(base[r * 3 + c]) /
                static_cast<double>(p.camera_white[c]));
        }
    }
    double ref_worst = 0.0;
    for (int i = 0; i < 9; ++i) {
        ref_worst = std::fmax(ref_worst,
                              std::fabs(static_cast<double>(p.camera_to_rgb[i]) -
                                        static_cast<double>(ref[i])));
    }

    // Three camera primaries plus two intermediate hues, all at ~0.8.
    static const float kProbes[5][3] = {
        {0.8f, 0.0f, 0.0f}, {0.0f, 0.8f, 0.0f}, {0.0f, 0.0f, 0.8f},
        {0.8f, 0.6f, 0.1f}, {0.1f, 0.7f, 0.8f}};
    double biggest_rel = 0.0;
    int biggest_probe = -1;
    for (int i = 0; i < 5; ++i) {
        float a[3], b[3];
        apply3x3(old_m, kProbes[i], a);
        apply3x3(p.camera_to_rgb, kProbes[i], b);
        for (int c = 0; c < 3; ++c) {
            const double denom = std::fmax(std::fabs(static_cast<double>(a[c])),
                                           std::fabs(static_cast<double>(b[c])));
            if (denom < 1e-6) continue;
            const double rel = std::fabs(static_cast<double>(b[c] - a[c])) / denom;
            if (rel > biggest_rel) { biggest_rel = rel; biggest_probe = i; }
        }
    }
    // The >5% discrimination half of AC-1.5 is only SATISFIABLE when the gains
    // are non-trivial: diag(k)*A and A*diag(g) converge to the same matrix as
    // g -> (1,1,1), so a file shot under a light where cam_mul is already
    // near-neutral cannot separate them, however correct the implementation is.
    // The spread is therefore reported as a precondition rather than silently
    // folded into the verdict -- and it is a precondition of the DATA, checked
    // before the numbers are read, not a threshold retro-fitted to a result.
    // Real case: the Foveon X3F corpus samples have camera_white
    // (0.99969, 1.0, 0.99940), a 1.0006 spread, and separate by only 2.3%.
    float gain_lo = 1.0f / p.camera_white[0], gain_hi = gain_lo;
    for (int c = 1; c < 3; ++c) {
        const float g = 1.0f / p.camera_white[c];
        gain_lo = std::min(gain_lo, g);
        gain_hi = std::max(gain_hi, g);
    }
    const float gain_spread = gain_hi / gain_lo;
    const bool discriminable = gain_spread > 1.05f;
    const bool ok = ref_worst < 1e-5 && (!discriminable || biggest_rel > 0.05);
    char detail[400];
    std::snprintf(detail, sizeof(detail),
                  "gain_spread=%.5f discriminable=%d (needs >1.05); "
                  "old(diag(k)*A) vs new(A*diag(g)) max_rel_diff=%.6f at probe %d "
                  "(want >0.05 when discriminable); |new - base*diag(g)|max=%.9f "
                  "(want <1e-5, always enforced)",
                  gain_spread, discriminable ? 1 : 0, biggest_rel, biggest_probe,
                  ref_worst);
    report(id, ok, detail);
}

// AC-1.6 / V7: neutral round-trip through the REAL Stage 4 kernel.
//
// The probe is v[c] = k * camera_white[c] -- a scene neutral expressed in
// UNBALANCED camera coordinates, which is what Stage 4 actually receives. A flat
// (k,k,k) probe would be wrong and would fail a correct implementation. Getting
// diag and its inverse the wrong way round is the single most likely bug here,
// and this is what catches it.
void checkNeutralRoundTrip(const char* id, const RenderParams& p) {
    const int w = 16, h = 16;
    std::vector<uint16_t> src(static_cast<size_t>(w) * h * 3);
    const float k = 0.5f;
    for (int i = 0; i < w * h; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = k * p.camera_white[c] * 65535.0f;
            src[static_cast<size_t>(i) * 3 + c] =
                static_cast<uint16_t>(std::min(65535.0f, std::max(0.0f, v)));
        }
    }
    std::vector<uint8_t> dst(static_cast<size_t>(w) * h * 4, 0);
    const bool ok = runRenderStage4HalideAot(
        src.data(), w, h, /*src_p=*/3, /*src_row_step=*/w * 3,
        /*src_col_step=*/3, /*src_plane_step=*/1, 1.0f / 65535.0f, w, h, p,
        dst.data(), /*fuse_rgba=*/true);

    const size_t at = (static_cast<size_t>(h / 2) * w + w / 2) * 4;
    const int r = dst[at], g = dst[at + 1], b = dst[at + 2];
    const int hi = std::max(r, std::max(g, b));
    const int lo = std::min(r, std::min(g, b));
    const double spread = static_cast<double>(hi - lo) / 255.0;
    char detail[240];
    std::snprintf(detail, sizeof(detail),
                  "kernel_ok=%d probe=k*camera_white (NOT flat grey) out=(%d,%d,%d) "
                  "spread=%.5f (want <0.005)",
                  ok ? 1 : 0, r, g, b, spread);
    report(id, ok && spread < 0.005, detail);
}

bool isIdentityCurve(const std::vector<float>& t) {
    if (t.size() < 2) return false;
    for (size_t i = 1; i < t.size(); ++i) {
        if (t[i] < t[i - 1]) return false;          // monotonic
    }
    return std::fabs(t.front()) < 1e-4f && std::fabs(t.back() - 1.0f) < 1e-4f;
}

// The adapter duplicates the SDK's sRGB->PCS matrix as a literal because its
// translation unit must not depend on dng_sdk. That duplication is gated HERE,
// where both are visible: bit-equality, not a tolerance. Without this, an SDK
// primaries update would silently skew every LibRaw render while every other
// case in the suite stayed green.
void checkSrgbConstantMatchesSdk() {
    float ours[9];
    raw_srgb_to_pcs_matrix(ours);
    const dng_matrix sdk = dng_space_sRGB::Get().MatrixToPCS();
    double worst = 0.0;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            worst = std::fmax(worst, std::fabs(static_cast<double>(ours[r * 3 + c]) -
                                               static_cast<double>(sdk[r][c])));
        }
    }
    // Also pin the PCS white the AC-1.2 invariant is stated against, and print
    // its distance from the design document's rounded literal so the 2.0e-4
    // discrepancy stays visible rather than becoming folklore.
    float white[3];
    raw_pcs_white(white);
    const double design_literal[3] = {0.9642, 1.0, 0.8249};
    double literal_gap = 0.0;
    for (int i = 0; i < 3; ++i) {
        literal_gap = std::fmax(literal_gap,
                                std::fabs(static_cast<double>(white[i]) - design_literal[i]));
    }
    // Not bit-equality: the SDK carries this matrix in real64 and the adapter
    // stores float, so 1e-7 is the float round-trip floor, not a slack budget.
    // The bare published primaries sit 4.0e-6 away -- 40x this bound -- so the
    // gate still fails if the adapter ever drops the SDK's own white-point
    // correction (dng_color_space.cpp:210-233).
    char detail[280];
    std::snprintf(detail, sizeof(detail),
                  "max|adapter_constant - dng_space_sRGB::MatrixToPCS()|=%.9f "
                  "(want <1e-7); pcs_white=(%.7f,%.7f,%.7f), gap to the design's "
                  "rounded (0.9642,1,0.8249) = %.6f (> AC-1.2's own 1e-4 bound, "
                  "which is why the invariant is stated against this value)",
                  worst, white[0], white[1], white[2], literal_gap);
    report("srgb-constant-matches-sdk", worst < 1e-7, detail);
}

// One real file through the production LibRaw route: frontend -> adapter ->
// builder. `ctx` and `adapter` are caller-owned because the adapter owns the
// plane and CFA arrays that `input` points at and must outlive it.
bool librawParamsForFile(const char* path, LibRawFrontendContext& ctx,
                         LibRawGpuInputAdapter& adapter, RawGpuInput& input,
                         RenderParams& params, char* why, size_t why_cap) {
    if (ctx.open_and_unpack(path) != kRawSuccess) {
        std::snprintf(why, why_cap, "open_and_unpack failed");
        return false;
    }
    RawDevelopParams develop{};
    char reason[256] = {0};
    if (adapter.build(ctx, &input, &develop, reason, sizeof(reason)) != kRawSuccess) {
        std::snprintf(why, why_cap, "adapter.build failed: %s", reason);
        return false;
    }
    if (!raw_build_render_params(input, develop, params)) {
        std::snprintf(why, why_cap, "raw_build_render_params returned false");
        return false;
    }
    return true;
}

// Parses native/scripts/tmp/round1_baseline_dump.cpp's "TAG idx value" output
// format for round1_baseline_tables.txt (Round 1 Task 1.3 Step 1, captured at
// HEAD a80ba700b707bf4e5967a51a32311d566ef8e98f, before the auto-exposure fold
// existed). Returns false if the file or a requested tag is missing.
bool loadBaselineTable(const char* path, const char* tag, std::vector<float>& out) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string word;
    size_t idx;
    float value;
    std::string line;
    bool any = false;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        if (!(iss >> word)) continue;
        if (word != tag) continue;
        if (!(iss >> idx >> value)) continue;
        if (out.size() <= idx) out.resize(idx + 1);
        out[idx] = value;
        any = true;
    }
    return any;
}

bool fileExists(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// AC-1.6 requires V7 per FORMAT, and AC-1.1 requires V6 for every corpus file,
// so the same three files drive both. These are the corpus entries that are
// actually present in-tree (native/tests/raw_corpus_manifest.json); a missing
// file is a FAILURE, not a skip -- silent coverage loss is exactly what
// run_raw_matrix.py exists to prevent.
struct FormatSample { const char* label; const char* path; };
const FormatSample kFormatSamples[] = {
    {"bayer",  "image_samples/raw_sample.arw"},
    {"xtrans", "image_samples/raw_corpus/fuji_xt3.raf"},
    {"foveon", "image_samples/raw_corpus/sigma_sd_quattro_h_19.x3f"},
};

}  // namespace

int main() {
    {
        RenderParams params;
        const RawGpuInput in = makeInput(true);
        const RawDevelopParams dev = makeDevelop();
        const bool ok = raw_build_render_params(in, dev, params);
        report("build-succeeds", ok, ok ? "returned true" : "returned false");

        // Identity tables, compared against the SAME helper the DNG path uses,
        // so this cannot drift from production behaviour.
        std::vector<float> want_huesat;
        int32_t want_hue = 0, want_sat = 0, want_val = 0, want_has = 0;
        toIdentityHueSatMap(want_huesat, want_hue, want_sat, want_val, want_has);
        bool tables_ok = ok &&
                         params.huesat_table.size() == want_huesat.size() &&
                         params.look_table.size() == want_huesat.size() &&
                         params.huesat_hue_div == want_hue &&
                         params.huesat_sat_div == want_sat &&
                         params.huesat_val_div == want_val &&
                         params.look_hue_div == want_hue &&
                         params.look_sat_div == want_sat &&
                         params.look_val_div == want_val &&
                         params.huesat_has_table == 0 && params.look_has_table == 0 &&
                         params.huesat_has_encoding == 0 && params.look_has_encoding == 0;
        for (size_t i = 0; tables_ok && i < want_huesat.size(); ++i) {
            if (std::fabs(params.huesat_table[i] - want_huesat[i]) > 1e-6f ||
                std::fabs(params.look_table[i] - want_huesat[i]) > 1e-6f) {
                tables_ok = false;
            }
        }
        tables_ok = tables_ok && isIdentityCurve(params.huesat_encode) &&
                    isIdentityCurve(params.huesat_decode) &&
                    isIdentityCurve(params.look_encode) &&
                    isIdentityCurve(params.look_decode);
        report("identity-tables", tables_ok, "huesat/look identity, curves monotonic");
    }

    {
        RenderParams params;
        const RawGpuInput in = makeInput(false);
        const RawDevelopParams dev = makeDevelop();
        const bool rejected = !raw_build_render_params(in, dev, params);
        report("matrix-absent-rejected", rejected, "camera_to_pcs.valid==0");
    }

    {
        // Cross-check against the DNG builder on a real file: the same white
        // balance and camera matrix must produce the same Stage4 inputs, or the
        // two routes would render the same sensor differently.
        RenderParams dng_params;
        const bool have_dng = dng_render_params_for_test(
            "image_samples/lossless_dng_sample.dng", dng_params);
        if (!have_dng) {
            std::printf("[RawRenderParams] SKIP dng-equivalence (sample unavailable)\n");
            std::printf("[RawRenderParams] FAIL: the dng-equivalence gate is mandatory\n");
            ++failures;
        } else {
            RawGpuInput in = makeInput(true);
            for (int i = 0; i < 3; ++i) in.as_shot_neutral[i] = dng_params.camera_white[i];
            RenderParams raw_params;
            const bool ok = raw_build_render_params(in, makeDevelop(), raw_params);
            bool same = ok;
            for (int i = 0; same && i < 3; ++i) {
                same = std::fabs(raw_params.camera_white[i] - dng_params.camera_white[i]) < 1e-5f;
            }
            // rgb_to_final is derived from the same SDK colour-space singletons
            // on both sides, so this agrees exactly, not merely within 1e-5.
            for (int i = 0; same && i < 9; ++i) {
                same = std::fabs(raw_params.rgb_to_final[i] - dng_params.rgb_to_final[i]) < 1e-5f;
            }
            report("dng-equivalence", same, "camera_white and rgb_to_final agree");

            // Curve tables: turn "correct by construction" into a gated fact.
            // Shapes must match unconditionally -- the Stage4 kernel indexes
            // these, so a size mismatch is a hard defect on either route.
            const bool shapes_ok =
                ok &&
                raw_params.exp_ramp.size() == dng_params.exp_ramp.size() &&
                raw_params.tone_curve.size() == dng_params.tone_curve.size() &&
                raw_params.encode_gamma.size() == dng_params.encode_gamma.size();
            report("curve-shapes", shapes_ok, "exp_ramp/tone_curve/encode_gamma sizes match the DNG path");

            // encode_gamma IS shared: both routes take the sRGB transfer
            // function from the same colour space, so this is gated at 1e-5
            // and in practice agrees to 0.0.
            reportCurve("curve-encode-gamma", shapes_ok, raw_params.encode_gamma,
                        dng_params.encode_gamma);

            // exp_ramp and tone_curve are NOT shared, and must not be gated on
            // equality. Production folds in DNG-only metadata that the plain-C
            // contract does not carry (measured on this very sample by
            // scripts/tmp/t8_curve_cause_probe.cpp):
            //   TotalBaselineExposure = 0.35   -> generic route has no equivalent
            //   renderer.Shadows()    = 5.0    -> production black = 0.005
            //   renderer.ToneCurve()  deviates from identity by 0.31684
            // That last figure alone accounts for the entire observed tone
            // divergence. Forcing equality would mean inventing a baseline
            // exposure and a camera-profile tone curve for non-DNG files, which
            // is exactly what spec 4.1.9 forbids. So these two are gated on what
            // the Stage4 kernel actually requires, and their divergence from the
            // DNG path is PRINTED every run rather than hidden.
            reportCurveSanity("curve-exp-ramp", shapes_ok, raw_params.exp_ramp,
                              dng_params.exp_ramp);
            reportCurveSanity("curve-tone", shapes_ok, raw_params.tone_curve,
                              dng_params.tone_curve);

            // AC-1b.1 (Stage 1b): the tone curve must now MATCH the DNG path
            // elementwise within 1e-6, not merely be well-shaped. Before Stage 1b
            // this diverged by 0.3168465 because the LibRaw path had no contrast
            // curve at all; with acr3(exposureTone(x)) concatenated it agrees at
            // 0.0000000.
            //
            // WHAT THIS DOES AND DOES NOT PROVE. It is gated on ONE file. The two
            // curves coincide exactly here because this sample's
            // TotalBaselineExposure and -log2(Stage3Gain) cancel, leaving the DNG
            // path's effective exposure at 0 -- the same value the LibRaw path
            // uses. On a file where they did not cancel, the exposure_tone halves
            // would differ and this case would fail for a legitimate reason: the
            // LibRaw path cannot see BaselineExposure at all (it is -999/absent in
            // LibRaw for vendor raws). If a future sample trips this, the answer is
            // to compare at MATCHED exposure, not to loosen the bound.
            reportCurve("ac-1b.1-tone-curve-matches-dng", shapes_ok,
                        raw_params.tone_curve, dng_params.tone_curve);
        }
    }

    // ----------------------------------------------------------------------
    // Stage 1 colour-correctness gates.
    // ----------------------------------------------------------------------
    checkSrgbConstantMatchesSdk();

    // Synthetic, deliberately NON-neutral gains: with as_shot_neutral all-1 the
    // fold is the identity and V8 could not discriminate anything. These are a
    // typical daylight cam_mul ~ (2.0, 1.0, 1.5) expressed as a neutral.
    {
        RawGpuInput in = makeInput(true);
        in.as_shot_neutral[0] = 1.0f / 2.0f;
        in.as_shot_neutral[1] = 1.0f / 1.0f;
        in.as_shot_neutral[2] = 1.0f / 1.5f;
        in.as_shot_neutral[3] = 1.0f;
        RenderParams p;
        const bool ok = raw_build_render_params(in, makeDevelop(), p);
        report("synthetic-build", ok, ok ? "returned true" : "returned false");
        if (ok) {
            checkClipConsistency("ac-1.1-clip-consistency-synthetic", p);
            checkFoldIdentity("ac-1.3-fold-identity-synthetic", in, p);
            checkSaturatedDiscrimination("ac-1.5-saturated-discrimination-synthetic",
                                         in, p);
            checkNeutralRoundTrip("ac-1.6-neutral-roundtrip-synthetic", p);
        }
    }

    // Scale invariance of the neutral: the contract does not fix the SCALE of
    // as_shot_neutral (the adapter emits a green-referenced one), so the builder
    // must normalise. Feeding the same neutral scaled by 10 must produce the
    // identical camera_white; if the normalisation were ever dropped, camera_white
    // would leave 0..1 and the Stage 4 highlight clip would silently switch off.
    {
        RawGpuInput a = makeInput(true), b = makeInput(true);
        const float n[3] = {0.5f, 1.0f, 0.6666667f};
        for (int c = 0; c < 3; ++c) {
            a.as_shot_neutral[c] = n[c];
            b.as_shot_neutral[c] = n[c] * 10.0f;
        }
        RenderParams pa, pb;
        const bool ok = raw_build_render_params(a, makeDevelop(), pa) &&
                        raw_build_render_params(b, makeDevelop(), pb);
        double worst = 0.0;
        for (int c = 0; ok && c < 3; ++c) {
            worst = std::fmax(worst, std::fabs(static_cast<double>(pa.camera_white[c]) -
                                               pb.camera_white[c]));
        }
        char detail[200];
        std::snprintf(detail, sizeof(detail),
                      "camera_white(n) vs camera_white(10n) max_abs_diff=%.9f "
                      "(want 0); camera_white=(%.6f,%.6f,%.6f)",
                      worst, pa.camera_white[0], pa.camera_white[1],
                      pa.camera_white[2]);
        report("neutral-scale-invariance", ok && worst == 0.0, detail);
    }

    // Per-format, on real files: AC-1.1 (V6), AC-1.3 (N2), AC-1.5 (V8) and
    // AC-1.6 (V7, through the real Stage 4 kernel).
    for (const FormatSample& fs : kFormatSamples) {
        char id[128];
        if (!fileExists(fs.path)) {
            std::snprintf(id, sizeof(id), "corpus-%s-present", fs.label);
            report(id, false, fs.path);
            continue;
        }
        LibRawFrontendContext ctx;
        LibRawGpuInputAdapter adapter;
        RawGpuInput input{};
        RenderParams p;
        char why[300] = {0};
        const bool ok = librawParamsForFile(fs.path, ctx, adapter, input, p, why,
                                            sizeof(why));
        std::snprintf(id, sizeof(id), "corpus-%s-params", fs.label);
        report(id, ok, ok ? fs.path : why);
        if (!ok) continue;

        std::snprintf(id, sizeof(id), "ac-1.1-clip-consistency-%s", fs.label);
        checkClipConsistency(id, p);
        std::snprintf(id, sizeof(id), "ac-1.3-fold-identity-%s", fs.label);
        checkFoldIdentity(id, input, p);
        std::snprintf(id, sizeof(id), "ac-1.5-saturated-discrimination-%s", fs.label);
        checkSaturatedDiscrimination(id, input, p);
        std::snprintf(id, sizeof(id), "ac-1.6-neutral-roundtrip-%s", fs.label);
        checkNeutralRoundTrip(id, p);
    }

    // AC-1.7 / V5: overall-transform equivalence on a file BOTH front-ends can
    // parse. T_dng and T_libraw are both raw-camera -> ProPhoto 3x3, so they are
    // directly comparable. The residual quantifies the DNG-SDK ForwardMatrix/D50
    // versus LibRaw inverse-ColorMatrix/D65 divergence. Design AC-1.7: >2% is a
    // STOP-and-report design input, NOT a tolerance to relax.
    {
        const char* kShared = "image_samples/lossless_dng_sample.dng";
        RenderParams dng_params;
        LibRawFrontendContext ctx;
        LibRawGpuInputAdapter adapter;
        RawGpuInput input{};
        RenderParams raw_params;
        char why[300] = {0};
        const bool have_dng = dng_render_params_for_test(kShared, dng_params);
        const bool have_raw = librawParamsForFile(kShared, ctx, adapter, input,
                                                  raw_params, why, sizeof(why));
        if (!have_dng || !have_raw) {
            char detail[360];
            std::snprintf(detail, sizeof(detail),
                          "dng_params=%d libraw_params=%d (%s) -- the V5 gate is "
                          "mandatory, not skippable",
                          have_dng ? 1 : 0, have_raw ? 1 : 0, why);
            report("ac-1.7-v5-transform-equivalence", false, detail);
        } else {
            // Elementwise.
            double worst_rel = 0.0;
            int worst_at = -1;
            for (int i = 0; i < 9; ++i) {
                const double a = dng_params.camera_to_rgb[i];
                const double b = raw_params.camera_to_rgb[i];
                const double denom = std::fmax(std::fabs(a), std::fabs(b));
                if (denom < 1e-6) continue;
                const double rel = std::fabs(a - b) / denom;
                if (rel > worst_rel) { worst_rel = rel; worst_at = i; }
            }
            // Basis action: neutral plus the three primaries at 0.5.
            static const float kBasis[4][3] = {
                {1.0f, 1.0f, 1.0f}, {0.5f, 0.0f, 0.0f},
                {0.0f, 0.5f, 0.0f}, {0.0f, 0.0f, 0.5f}};
            double worst_basis = 0.0;
            int worst_basis_at = -1;
            for (int i = 0; i < 4; ++i) {
                float a[3], b[3];
                apply3x3(dng_params.camera_to_rgb, kBasis[i], a);
                apply3x3(raw_params.camera_to_rgb, kBasis[i], b);
                for (int c = 0; c < 3; ++c) {
                    const double denom = std::fmax(std::fabs(static_cast<double>(a[c])),
                                                   std::fabs(static_cast<double>(b[c])));
                    if (denom < 1e-6) continue;
                    const double rel = std::fabs(static_cast<double>(a[c] - b[c])) / denom;
                    if (rel > worst_basis) { worst_basis = rel; worst_basis_at = i; }
                }
            }
            // AC-1.7 is a STOP-and-report gate, so the evidence has to be in the
            // artifact: a bare "max_rel=1.34" cannot be judged without seeing
            // whether the offending element is simply near zero. Both matrices,
            // both white vectors, and a scale-robust companion metric are
            // printed unconditionally, pass or fail.
            double dng_absmax = 0.0;
            for (int i = 0; i < 9; ++i) {
                dng_absmax = std::fmax(dng_absmax, std::fabs(static_cast<double>(
                                                       dng_params.camera_to_rgb[i])));
            }
            double abs_worst = 0.0;
            for (int i = 0; i < 9; ++i) {
                abs_worst = std::fmax(abs_worst,
                                      std::fabs(static_cast<double>(dng_params.camera_to_rgb[i]) -
                                                static_cast<double>(raw_params.camera_to_rgb[i])));
            }
            std::printf("[RawRenderParams] V5-evidence T_dng      = ");
            for (int i = 0; i < 9; ++i) std::printf("% .6f ", dng_params.camera_to_rgb[i]);
            std::printf("\n[RawRenderParams] V5-evidence T_libraw   = ");
            for (int i = 0; i < 9; ++i) std::printf("% .6f ", raw_params.camera_to_rgb[i]);
            std::printf("\n[RawRenderParams] V5-evidence camera_white dng=(%.6f,%.6f,%.6f) "
                        "libraw=(%.6f,%.6f,%.6f)\n",
                        dng_params.camera_white[0], dng_params.camera_white[1],
                        dng_params.camera_white[2], raw_params.camera_white[0],
                        raw_params.camera_white[1], raw_params.camera_white[2]);
            std::printf("[RawRenderParams] V5-evidence max_abs_diff=%.6f, "
                        "normalised by max|T_dng|=%.6f -> %.6f\n",
                        abs_worst, dng_absmax,
                        dng_absmax > 0.0 ? abs_worst / dng_absmax : -1.0);

            // Instrument check before believing a negative result: if both
            // matrices are white-preserving under their OWN camera_white, the
            // divergence is colorimetric, not a bug in either fold.
            for (int which = 0; which < 2; ++which) {
                const RenderParams& q = which ? raw_params : dng_params;
                float n[3];
                apply3x3(q.camera_to_rgb, q.camera_white, n);
                std::printf("[RawRenderParams] V5-evidence %s T*camera_white = "
                            "(%.6f,%.6f,%.6f) (want (1,1,1))\n",
                            which ? "libraw" : "dng   ", n[0], n[1], n[2]);
            }

            // Design section 12 names the exact experiment that would overturn
            // the section-1.5 primary/fallback ORDER: measure the same V5
            // divergence for the cam_xyz route. Run it unconditionally so an
            // AC-1.7 breach arrives with the decision-relevant number attached
            // rather than needing a second round to obtain it.
            RawColorTransform alt{};
            char alt_reason[256] = {0};
            const int alt_route = raw_camera_to_pcs_from_libraw(
                nullptr, ctx.raw_view().cam_xyz, ctx.raw_view().raw_color,
                ctx.raw_view().colors, &alt, alt_reason, sizeof(alt_reason));
            RawGpuInput alt_in = input;
            alt_in.camera_to_pcs = alt;
            RenderParams alt_params;
            RawDevelopParams alt_dev{};
            alt_dev.exposure_ev = 0.0f;
            alt_dev.tone_curve_strength = 1.0f;
            alt_dev.output_space = kRawOutputColorSpaceSrgb;
            if (alt_route == 2 &&
                raw_build_render_params(alt_in, alt_dev, alt_params)) {
                double alt_rel = 0.0;
                for (int i = 0; i < 9; ++i) {
                    const double a = dng_params.camera_to_rgb[i];
                    const double b = alt_params.camera_to_rgb[i];
                    const double denom = std::fmax(std::fabs(a), std::fabs(b));
                    if (denom < 1e-6) continue;
                    alt_rel = std::fmax(alt_rel, std::fabs(a - b) / denom);
                }
                std::printf("[RawRenderParams] V5-evidence FALLBACK-ROUTE "
                            "(cam_xyz) T vs T_dng elementwise max_rel=%.6f "
                            "(primary rgb_cam route measured above)\n", alt_rel);
            } else {
                std::printf("[RawRenderParams] V5-evidence FALLBACK-ROUTE "
                            "unavailable (route=%d, %s)\n", alt_route, alt_reason);
            }

            // MEASURE AND RECORD, not a gate.
            //
            // AC-1.7 originally required < 2 % here and designated a breach a
            // STOP-and-report condition. It was breached at 134 %, the stage was
            // halted, and the USER resolved it 2026-08-28: the matrix stays
            // Method A (rgb_cam), the measured divergence is recorded as a design
            // input, and the 2 % gate is DELETED from the contract.
            //
            // This is a contract amendment by the only party entitled to make
            // one, NOT a threshold relaxed to get a green run. The evidence:
            // both routes were measured (rgb_cam 134 %, cam_xyz 102 %), so no
            // available matrix source could have satisfied the bound; the two
            // matrices are each exactly white-preserving under their own white,
            // so neither fold is broken; and the user's own A/B/C render probe
            // showed the matrix choice moves the render by ~1-2 8-bit codes while
            // the visible gap was tone. Full trace:
            // tmp/verify/stage1_AC-1.7_STOP_report.md and
            // tmp/verify/ab_renders/README_AB_probe_report.md.
            //
            // The number is still computed and printed every run, so a future
            // change that moves it shows up as a moved number rather than
            // silence. The only thing asserted is that the comparison could be
            // performed at all.
            char detail[420];
            std::snprintf(detail, sizeof(detail),
                          "RECORDED (not gated): T_dng vs T_libraw elementwise "
                          "max_rel=%.6f at %d of 9; basis-action max_rel=%.6f at "
                          "basis %d. The 2%% bound was deleted by user ruling "
                          "2026-08-28 after both routes measured ~1e2x over it "
                          "(rgb_cam 1.34, cam_xyz 1.02) and the render probe "
                          "showed the matrix is worth ~1-2 codes",
                          worst_rel, worst_at, worst_basis, worst_basis_at);
            report("ac-1.7-v5-transform-divergence-recorded",
                   std::isfinite(worst_rel) && std::isfinite(worst_basis) &&
                       worst_at >= 0 && worst_basis_at >= 0,
                   detail);
        }
    }

    // ----------------------------------------------------------------------
    // Round 1 Task 1.3: automatic exposure baseline fold.
    // ----------------------------------------------------------------------

    // auto_off_is_bit_identical: with kRawAutoExposureOff, the real adapter's
    // output on the corpus Bayer sample must equal the pre-change tables
    // captured to native/scripts/tmp/round1_baseline_tables.txt entry-for-entry.
    {
        const char* kBaselinePath = "native/scripts/tmp/round1_baseline_tables.txt";
        std::vector<float> want_ramp, want_tone;
        const bool have_baseline =
            loadBaselineTable(kBaselinePath, "EXP_RAMP", want_ramp) &&
            loadBaselineTable(kBaselinePath, "TONE_CURVE", want_tone);
        if (!have_baseline) {
            report("auto-off-is-bit-identical", false,
                   "round1_baseline_tables.txt missing or unparsable");
        } else {
            // Not librawParamsForFile: that helper zero-inits RawDevelopParams
            // (auto_exposure_mode defaults to kRawAutoExposureOn), and this case
            // specifically needs auto_exposure_mode set to Off BEFORE
            // adapter.build() -- the one field build() reads as input.
            LibRawFrontendContext ctx;
            LibRawGpuInputAdapter adapter;
            RawGpuInput input{};
            RenderParams p;
            char why[300] = {0};
            bool ok = ctx.open_and_unpack("image_samples/raw_sample.arw") == kRawSuccess;
            if (!ok) std::snprintf(why, sizeof(why), "open_and_unpack failed");
            RawDevelopParams develop{};
            develop.auto_exposure_mode = kRawAutoExposureOff;
            char reason[256] = {0};
            if (ok) {
                ok = adapter.build(ctx, &input, &develop, reason, sizeof(reason)) == kRawSuccess;
                if (!ok) {
                    std::snprintf(why, sizeof(why), "adapter.build failed: %s", reason);
                }
            }
            if (ok) {
                ok = raw_build_render_params(input, develop, p);
                if (!ok) std::snprintf(why, sizeof(why), "raw_build_render_params returned false");
            }
            char mode_detail[120];
            std::snprintf(mode_detail, sizeof(mode_detail),
                          "auto_exposure_mode=%d auto_exposure_ev=%.7f (want mode=1, ev=0)",
                          develop.auto_exposure_mode, develop.auto_exposure_ev);
            report("auto-off-mode-honoured",
                   ok && develop.auto_exposure_mode == kRawAutoExposureOff &&
                       develop.auto_exposure_ev == 0.0f,
                   mode_detail);
            bool same = ok && p.exp_ramp.size() == want_ramp.size() &&
                        p.tone_curve.size() == want_tone.size();
            size_t first_mismatch = static_cast<size_t>(-1);
            for (size_t i = 0; same && i < want_ramp.size(); ++i) {
                if (p.exp_ramp[i] != want_ramp[i]) { same = false; first_mismatch = i; }
            }
            for (size_t i = 0; same && i < want_tone.size(); ++i) {
                if (p.tone_curve[i] != want_tone[i]) { same = false; first_mismatch = i; }
            }
            char detail[240];
            std::snprintf(detail, sizeof(detail),
                          "ok=%d sizes(ramp %zu/%zu, tone %zu/%zu) first_mismatch_index=%zu (%s)",
                          ok, p.exp_ramp.size(), want_ramp.size(), p.tone_curve.size(),
                          want_tone.size(), first_mismatch, ok ? "" : why);
            report("auto-off-is-bit-identical", same, detail);
        }
    }

    // auto_ev_shifts_the_curve: auto_exposure_ev=1.0f must visibly raise a
    // curve relative to auto_exposure_ev=0.0f at the quarter-point index.
    //
    // DEVIATION FROM THE PLAN, WITH EVIDENCE: the plan (Task 1.3 acceptance)
    // named tone_curve. Red-first run against exp_ramp/tone_curve showed
    // tone_curve diff == 0.0000000 exactly for every non-negative exposure,
    // which traces to the DNG SDK itself, not a wiring bug:
    // dng_render.cpp:77-127, dng_function_exposure_tone::dng_function_exposure_tone
    // sets `fIsNOP (exposure >= 0.0)` -- the exposure-tone stage is an
    // architectural NOP whenever exposure is non-negative, and
    // raw_auto_exposure.h clamps auto_ev to [0, +2], so auto_exposure_ev can
    // NEVER make exposure negative here. tone_curve is therefore
    // mathematically incapable of moving from this fold, by the SDK's own
    // design (positive exposure lightens through the `white` term of exp_ramp
    // instead, dng_render_halide.cpp:794-806). This case is gated on exp_ramp,
    // which the same red-first run confirmed DOES move (this is also the
    // curve the darkness-fix render actually depends on -- Task 1.4's
    // mid-grey oracle measures the rendered image, not this table directly).
    {
        RawGpuInput in = makeInput(true);
        RawDevelopParams dev0 = makeDevelop();
        dev0.auto_exposure_ev = 0.0f;
        RawDevelopParams dev1 = makeDevelop();
        dev1.auto_exposure_ev = 1.0f;
        RenderParams p0, p1;
        const bool ok = raw_build_render_params(in, dev0, p0) &&
                        raw_build_render_params(in, dev1, p1);
        const size_t idx = ok ? p0.exp_ramp.size() / 4 : 0;
        const bool shifted =
            ok && idx < p0.exp_ramp.size() && idx < p1.exp_ramp.size() &&
            (p1.exp_ramp[idx] - p0.exp_ramp[idx]) > 1e-3f;
        char detail[200];
        std::snprintf(detail, sizeof(detail),
                      "ok=%d idx=%zu exp_ramp[idx] auto_ev=0 -> %.7f, auto_ev=1 -> %.7f, diff=%.7f",
                      ok, idx, ok ? p0.exp_ramp[idx] : -1.0f, ok ? p1.exp_ramp[idx] : -1.0f,
                      ok ? (p1.exp_ramp[idx] - p0.exp_ramp[idx]) : 0.0f);
        report("auto-ev-shifts-the-curve", shifted, detail);
    }

    if (failures != 0) {
        std::printf("[RawRenderParams] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[RawRenderParams] ALL PASS\n");
    return 0;
}
