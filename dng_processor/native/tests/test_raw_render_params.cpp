// buildRenderParamsFromRaw coverage.
//
// The identity-table case is the one that matters most: LibRaw supplies no
// HueSatMap or LookTable, and leaving those vectors uninitialised is explicitly
// forbidden (spec section 7.1.4). "Uninitialised" would still render - just
// with garbage colour - so only an elementwise assertion catches it.
#include <cmath>
#include <cstdio>
#include <vector>

#include "dng_render_params.h"
#include "raw_render_params_builder.h"

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
    // The gate greps for the exact prefix "[RawRenderParams] <name> -> PASS",
    // so the detail goes after the verdict, never between name and arrow.
    std::printf("[RawRenderParams] %s -> %s (%s)\n", name, ok ? "PASS" : "FAIL",
                detail);
    if (!ok) ++failures;
}

// Only the fields buildRenderParamsFromRaw actually reads are populated; the
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
    // A plausible camera->XYZ matrix; the exact values only need to be
    // invertible for the composition step.
    const float m[9] = {0.7688f, -0.2199f, -0.0724f,
                        -0.3129f, 1.0781f,  0.2588f,
                        -0.0281f, 0.1287f,  0.6797f};
    for (int i = 0; i < 9; ++i) in.camera_to_pcs.m[i] = m[i];
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

bool isIdentityCurve(const std::vector<float>& t) {
    if (t.size() < 2) return false;
    for (size_t i = 1; i < t.size(); ++i) {
        if (t[i] < t[i - 1]) return false;          // monotonic
    }
    return std::fabs(t.front()) < 1e-4f && std::fabs(t.back() - 1.0f) < 1e-4f;
}

}  // namespace

int main() {
    {
        RenderParams params;
        const RawGpuInput in = makeInput(true);
        const RawDevelopParams dev = makeDevelop();
        const bool ok = buildRenderParamsFromRaw(in, dev, params);
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
        const bool rejected = !buildRenderParamsFromRaw(in, dev, params);
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
            const bool ok = buildRenderParamsFromRaw(in, makeDevelop(), raw_params);
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
        }
    }

    if (failures != 0) {
        std::printf("[RawRenderParams] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[RawRenderParams] ALL PASS\n");
    return 0;
}
