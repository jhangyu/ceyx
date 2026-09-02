#include "raw_render_params_builder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dng_1d_function.h"
#include "dng_1d_table.h"
#include "dng_color_space.h"
#include "dng_file_stream.h"
#include "dng_host.h"
#include "dng_info.h"
#include "dng_negative.h"
#include "dng_render.h"

namespace {

// The DNG path never hard-codes colour matrices: it composes the SDK's own
// colour-space singletons (dng_render_halide.cpp:789-792). Reusing the exact
// same stateless constants here makes the two routes agree by construction
// instead of to a tolerance. The published/ICC ProPhoto matrices differ from
// the SDK's 4-decimal primaries by up to 8.3e-4, which is 83x the equivalence
// test's 1e-5 bound -- hence no literals in this file.
//
// Only *stateless colour-space constants* are taken from the SDK: no
// dng_negative, dng_info or dng_render state enters this builder.
void matrixToRowMajor3x3(const dng_matrix& m, float out9[9]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out9[r * 3 + c] = static_cast<float>(m[r][c]);
        }
    }
}

// DELETED (design Task_raw_color_architecture.md Rev 2 sections 1.4 / 7 AC-1.4):
// there used to be a row-to-neutral normalisation helper here that divided row r
// of camera_to_rgb by sum_c A[r][c]*n[c], forming diag(k)*A -- a diagonal in the
// OUTPUT (ProPhoto) space. The von Kries correction the DNG path applies is
// A*diag(g), a diagonal in the camera INPUT space (dng_color_spec.cpp:441-446
// folds Invert(refCameraWhite.AsDiagonal()) on the right). The two forms agree
// only when A is diagonal, which a camera->ProPhoto matrix never is: both map
// neutral to neutral -- which is why greys always looked right -- and they
// diverge on every saturated colour, which is the washed-out symptom.
//
// It is a REPLACEMENT, not an addition. Reinstating it alongside the fold below
// would double-correct: greys would stay neutral and mask the error while
// colours over-rotate. AC-1.4 greps this file for the old helper's identifier and
// requires a count of ZERO, which is why this paragraph describes it rather than
// naming it. The numeric guard is the V8 saturated-patch case in
// test_raw_render_params.cpp, which keeps a test-only copy of the old maths as an
// explicit counterfactual.

// Same shape the DNG path produces (dng_render_halide.cpp:831-833): a
// dng_1d_table is kTableSize + 2 floats, uniformly sampled over [0,1] with the
// final slot pinned to the endpoint. Sampling the SDK function directly avoids
// needing a dng_host allocator while giving bit-comparable table contents.
void sampleFunction(std::vector<float>& table, const dng_1d_function& fn) {
    table.resize(static_cast<size_t>(dng_1d_table::kTableSize) + 2u);
    for (int i = 0; i <= dng_1d_table::kTableSize; ++i) {
        const real64 x =
            static_cast<real64>(i) / static_cast<real64>(dng_1d_table::kTableSize);
        table[static_cast<size_t>(i)] = static_cast<float>(fn.Evaluate(x));
    }
    table[static_cast<size_t>(dng_1d_table::kTableSize) + 1u] =
        table[static_cast<size_t>(dng_1d_table::kTableSize)];
}

}  // namespace

bool raw_build_render_params(const RawGpuInput& input,
                              const RawDevelopParams& develop,
                              RenderParams& params) {
    if (develop.output_space != kRawOutputColorSpaceSrgb) return false;
    // Never invent a matrix (spec section 4.1.9). Phase 17 delivers 3-colour
    // Bayer / X-Trans only, so a non-3x3 transform is unsupported rather than
    // silently reinterpreted -- m[] is row-major with in_cols stride, so
    // reading a 3x4 as a 3x3 would scramble the coefficients.
    if (input.camera_to_pcs.valid == 0) return false;
    if (input.camera_to_pcs.out_rows != 3u || input.camera_to_pcs.in_cols != 3u) {
        return false;
    }

    // camera_white must be MAX-normalised with a 0.001 floor, because that is
    // what Stage 4 assumes: its min(s_c, camera_white(c)) is a highlight ceiling
    // on un-white-balanced data (DngRenderGenerator.cpp:87-89), positioned so a
    // channel-clipped highlight lands on the neutral vector. The SDK derives it
    // the same way -- whiteScale = 1/MaxEntry, then Pin_real64(0.001, .., 1.0)
    // at dng_color_spec.cpp:413-421. An entry above 1.0 would sit outside the
    // 0..1 signal range and SILENTLY disable the clip on that channel.
    //
    // TERMINOLOGY TRAP: on LibRaw's reciprocal quantity, gains m[c], this same
    // operation is MIN-normalisation. The contract carries a neutral
    // (n[c] proportional to 1/m[c]) whose absolute scale is arbitrary, so
    // dividing by max_c(n) here is exactly the design's
    // camera_white[c] = clamp(min_c(m)/m[c], 0.001, 1.0). Applying "min" to the
    // neutral instead would produce a global colour cast with no test failure.
    // This is the ONE place the normalisation happens; the adapter deliberately
    // emits an unnormalised neutral.
    float neutral[3];
    float max_neutral = 0.0f;
    for (int i = 0; i < 3; ++i) {
        neutral[i] = (input.as_shot_neutral[i] > 0.0f &&
                      std::isfinite(input.as_shot_neutral[i]))
                         ? input.as_shot_neutral[i]
                         : 1.0f;
        max_neutral = std::max(max_neutral, neutral[i]);
    }
    if (!(max_neutral > 0.0f)) max_neutral = 1.0f;
    float gain[3];
    for (int i = 0; i < 3; ++i) {
        params.camera_white[i] =
            std::min(1.0f, std::max(0.001f, neutral[i] / max_neutral));
        gain[i] = 1.0f / params.camera_white[i];   // >= 1 by construction
    }
    params.camera_white_vec = dng_vector_3(params.camera_white[0],
                                           params.camera_white[1],
                                           params.camera_white[2]);

    // camera -> XYZ(D50) from the contract, composed with the SAME
    // XYZ -> ProPhoto matrix the DNG path uses.
    dng_matrix_3by3 camera_to_pcs(input.camera_to_pcs.m[0], input.camera_to_pcs.m[1],
                                  input.camera_to_pcs.m[2], input.camera_to_pcs.m[3],
                                  input.camera_to_pcs.m[4], input.camera_to_pcs.m[5],
                                  input.camera_to_pcs.m[6], input.camera_to_pcs.m[7],
                                  input.camera_to_pcs.m[8]);
    // THE FOLD. diag(gain) is a RIGHT (column) multiply: it scales the camera
    // INPUT channels before the matrix mixes them, which is what makes the
    // matrix's negative off-diagonal terms subtract from a properly scaled
    // channel and preserves full chroma separation. Composing on the right also
    // makes this the same transform as multiplying the gains into the pixels
    // before Stage 4, exactly: since gain[c] > 0, min commutes with positive
    // per-channel scaling, so A*diag(g)*min(s, w) == A*min(s (*) g, 1). The fold
    // is preferred over the pixel multiply because it keeps the gains and the
    // clip vector derived from ONE array in ONE file -- a coupling that is
    // invisible, and trivially broken, once it spans a file boundary -- and
    // because the pre-Stage-4 kernels emit uint16, which would quantise and hard
    // clip the boosted channels (RawBayerDemosaicGenerator.cpp:48-49).
    //
    // No row normalisation follows. camera_to_pcs is contractually
    // white-preserving (camera_to_pcs * (1,1,1) == PCS white, enforced in the
    // adapter's route table) and ProPhoto's white IS the PCS white, so
    // ProPhoto::MatrixFromPCS() * camera_to_pcs already maps neutral to neutral.
    const dng_matrix_3by3 gain_diag(gain[0], 0.0, 0.0,
                                    0.0, gain[1], 0.0,
                                    0.0, 0.0, gain[2]);
    const dng_matrix camera_to_rgb =
        dng_space_ProPhoto::Get().MatrixFromPCS() * camera_to_pcs * gain_diag;
    const dng_matrix rgb_to_final =
        dng_space_sRGB::Get().MatrixFromPCS() * dng_space_ProPhoto::Get().MatrixToPCS();
    params.camera_to_rgb_mat = camera_to_rgb;
    params.rgb_to_final_mat = rgb_to_final;

    matrixToRowMajor3x3(camera_to_rgb, params.camera_to_rgb);
    matrixToRowMajor3x3(rgb_to_final, params.rgb_to_final);

    // Exposure / tone / gamma via the same SDK function objects the DNG path
    // feeds into dng_1d_table, so the Stage4 kernel sees identically shaped and
    // identically valued curves. A linear encode_gamma here would render every
    // generic-route image visibly dark, so the sRGB transfer function is taken
    // from the colour space rather than assumed.
    // BASELINE EXPOSURE STAYS ZERO, and that is a measurement, not an omission.
    // The DNG path adds negative.TotalBaselineExposure(profileID) here
    // (dng_render_halide.cpp:789-793). LibRaw's analogue,
    // imgdata.color.dng_levels.baseline_exposure, is -999.0 on every corpus
    // sample: that is LibRaw's ABSENT initialiser (init_close_utils.cpp:97,
    // :187) and it is only ever overwritten from a DNG BaselineExposure TIFF tag
    // (tiff.cpp:1522), which vendor raws do not carry.
    //
    // CONTRACT LINE, because the obvious predicate is wrong: any future check for
    // this value MUST treat <= -900 as absent. A `!= 0` test passes -999 straight
    // through and would fold -999 EV into the exposure. Measured 2026-08-28 on
    // ARW, RAF and X3F; evidence in tmp/verify/ab_renders/README_AB_probe_report.md.
    //
    // Round 1 Task 1.3 (explore_codebase_color_gap.md §6 H1): the darkness
    // defect's root cause is that LibRaw's own auto-brighten gain -- which
    // only runs inside the forbidden dcraw_process call (spec §13.1) -- is
    // never applied on this route. develop.auto_exposure_ev is the adapter's
    // own histogram-estimator replacement (raw_auto_exposure.h), already
    // guaranteed 0.0f when auto_exposure_mode is off or the estimate was not
    // kOk (LibRawGpuInputAdapter::build()) -- folded additively in log space
    // at the same point develop.exposure_ev is, so auto gain and any future
    // user EV compose exactly like two exposure_ev contributions would.
    const real64 exposure = static_cast<real64>(develop.exposure_ev) +
                            static_cast<real64>(develop.auto_exposure_ev);
    const real64 white = 1.0 / std::pow(2.0, std::max<real64>(0.0, exposure));

    // Shadows black lift, matching dng_render_halide.cpp:794-797:
    //   black = Shadows * ShadowScale * Stage3Gain * 0.001, capped at 0.99*white.
    // Shadows is the SDK renderer default of 5.0.
    //
    // DOCUMENTED ASSUMPTIONS: ShadowScale and Stage3Gain are dng_negative
    // properties with no LibRaw analogue, so both are taken as 1.0 -- their DNG
    // defaults -- giving black = 0.005. Stage3Gain also appears in the DNG
    // exposure term as -log2(Stage3Gain), which is 0 at gain 1.0 and so drops out
    // above. If a camera ever has a real Stage3Gain != 1 this substitution is
    // wrong, and nothing here detects that.
    const real64 black = std::min<real64>(5.0 * 1.0 * 1.0 * 0.001, 0.99 * white);
    const dng_function_exposure_ramp ramp_fn(white, black, black);
    sampleFunction(params.exp_ramp, ramp_fn);

    // ACR3 contrast curve, concatenated exactly as the DNG path composes it
    // (dng_render_halide.cpp:806, and dng_render.cpp:939-942 in the SDK itself).
    //
    // ORDER MATTERS AND IS NOT OBVIOUS: dng_1d_concatenate(a, b) evaluates
    // b(a(x)), so this is acr3(exposureTone(x)) -- the exposure tone first, the
    // contrast curve on top. Writing the arguments the other way round compiles,
    // runs, and produces a subtly wrong curve.
    //
    // Without this the LibRaw path had NO contrast curve at all, which was the
    // dominant term in the "washed out" report -- larger in the render than the
    // white-balance defect Stage 1 fixed. The user's A/B/C probe measured it:
    // adding this plus the black lift moved p95 luma from 122.3 to 171.4 against
    // the DNG path's 189.9, while the matrix choice was worth 1-2 codes.
    const dng_function_exposure_tone exposure_tone(exposure);
    const dng_tone_curve_acr3_default acr3_tone;
    const dng_1d_concatenate total_tone(exposure_tone, acr3_tone);
    sampleFunction(params.tone_curve, total_tone);
    if (develop.tone_curve_strength != 1.0f) {
        const float strength = std::max(develop.tone_curve_strength, 0.0f);
        for (size_t i = 0; i < params.tone_curve.size(); ++i) {
            const float linear =
                static_cast<float>(i) / static_cast<float>(dng_1d_table::kTableSize);
            params.tone_curve[i] =
                linear + (params.tone_curve[i] - linear) * strength;
        }
    }

    sampleFunction(params.encode_gamma, dng_space_sRGB::Get().GammaFunction());

    // The generic RAW frontend supplies no HueSatMap or LookTable: EXPLICIT
    // identity, never uninitialised (7.1.4). toIdentityHueSatMap sets the
    // divisions itself (2/2/2) and has_table to 0 -- same values the DNG path
    // gets, so the kernel sees one shape from both frontends.
    toIdentityHueSatMap(params.huesat_table, params.huesat_hue_div,
                        params.huesat_sat_div, params.huesat_val_div,
                        params.huesat_has_table);
    params.huesat_has_encoding = 0;

    toIdentityHueSatMap(params.look_table, params.look_hue_div,
                        params.look_sat_div, params.look_val_div,
                        params.look_has_table);
    params.look_has_encoding = 0;

    toIdentityCurve(params.huesat_encode);
    toIdentityCurve(params.huesat_decode);
    toIdentityCurve(params.look_encode);
    toIdentityCurve(params.look_decode);

    // The AutoPtr members stay null on purpose: they are only read on the SDK
    // render path, which the generic route never takes.
    return true;
}

bool dng_render_params_for_test(const char* dng_path, RenderParams& out) {
    if (!dng_path) return false;
    try {
        dng_file_stream stream(dng_path);
        dng_host host;
        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);
        if (!info.IsValidDNG()) return false;

        AutoPtr<dng_negative> negative(host.Make_dng_negative());
        negative->Parse(host, stream, info);
        negative->PostParse(host, stream, info);
        negative->ReadStage1Image(host, stream, info);
        negative->BuildStage2Image(host);
        negative->BuildStage3Image(host);

        dng_render renderer(host, *negative);
        const PipelineConfig config = PipelineConfig::loadFromEnv();
        return buildRenderParams(host, *negative, renderer, config, out);
    } catch (...) {
        return false;
    }
}
