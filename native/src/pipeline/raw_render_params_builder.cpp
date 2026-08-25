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

// Scale rows so a neutral camera input maps to a neutral output. On the DNG
// path this normalisation is already folded into spec->CameraToPCS(); the
// generic contract hands us an un-normalised camera->XYZ matrix plus a separate
// as-shot neutral, so it has to happen here.
void normalizeRowsToNeutral(float m[9], const float neutral[3]) {
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

    for (int i = 0; i < 3; ++i) {
        params.camera_white[i] = input.as_shot_neutral[i] > 0.0f
                                     ? input.as_shot_neutral[i]
                                     : 1.0f;
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
    const dng_matrix camera_to_rgb =
        dng_space_ProPhoto::Get().MatrixFromPCS() * camera_to_pcs;
    const dng_matrix rgb_to_final =
        dng_space_sRGB::Get().MatrixFromPCS() * dng_space_ProPhoto::Get().MatrixToPCS();
    params.camera_to_rgb_mat = camera_to_rgb;
    params.rgb_to_final_mat = rgb_to_final;

    matrixToRowMajor3x3(camera_to_rgb, params.camera_to_rgb);
    matrixToRowMajor3x3(rgb_to_final, params.rgb_to_final);
    normalizeRowsToNeutral(params.camera_to_rgb, params.camera_white);

    // Exposure / tone / gamma via the same SDK function objects the DNG path
    // feeds into dng_1d_table, so the Stage4 kernel sees identically shaped and
    // identically valued curves. A linear encode_gamma here would render every
    // generic-route image visibly dark, so the sRGB transfer function is taken
    // from the colour space rather than assumed.
    const real64 exposure = static_cast<real64>(develop.exposure_ev);
    const real64 white = 1.0 / std::pow(2.0, std::max<real64>(0.0, exposure));
    const dng_function_exposure_ramp ramp_fn(white, 0.0, 0.0);
    sampleFunction(params.exp_ramp, ramp_fn);

    const dng_function_exposure_tone exposure_tone(exposure);
    sampleFunction(params.tone_curve, exposure_tone);
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
