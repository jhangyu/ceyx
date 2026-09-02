// raw_render_eval.cpp - production RawRenderEvalFn, evaluating the pipeline's
// OWN render chain (via the DNG SDK's reference sampling functions) on ONE
// linear camera triple. See raw_render_eval.h and
// docs/logs/2026-09-03/raw_color_implementation_plan.md Round 1 Task 1.7.
//
// Deliberately calls the SDK's own DoBaseline*/dng_function_exposure_* /
// dng_tone_curve_acr3_default machinery rather than re-deriving the tone
// maths (exposure ramp shape, ACR3 contrast curve, hue-preserving RGBTone
// remap) by hand -- a second, hand-written copy would silently drift from
// the builder's and the estimator would then solve against a chain the
// pipeline does not actually run (review reject per the plan).
#include "raw_render_eval.h"

#include <algorithm>
#include <cmath>

#include "dng_1d_function.h"
#include "dng_1d_table.h"
#include "dng_bottlenecks.h"
#include "dng_color_space.h"
#include "dng_host.h"
#include "dng_matrix.h"
#include "dng_render.h"

namespace {

// exposure -> (white, black) for dng_function_exposure_ramp, transcribed
// unchanged from raw_render_params_builder.cpp's fold point (the SAME
// formulas the builder uses to build params.exp_ramp / params.tone_curve at
// build time). The estimator runs BEFORE develop.exposure_ev is known to
// compose with -- it solves for the auto contribution alone, so `exposure`
// here is exactly the candidate `ev` bisection is probing, base 0.
void exposureWhiteBlack(float exposure, double& white, double& black) {
    white = 1.0 / std::pow(2.0, std::max<double>(0.0, exposure));
    // Shadows(5.0) * ShadowScale(1.0) * Stage3Gain(1.0) * 0.001, capped at
    // 0.99*white -- dng_render_halide.cpp:794-797 / raw_render_params_builder.cpp.
    black = std::min<double>(5.0 * 1.0 * 1.0 * 0.001, 0.99 * white);
}

}  // namespace

float raw_render_eval_from_params(void* ctx, const float rgb[3], float ev) {
    const RenderParams* params = static_cast<const RenderParams*>(ctx);
    if (params == nullptr) return -1.0f;  // non-finite-free sentinel; caller treats <1.0 as "not there yet"

    // Step 1: clip vs camera_white -> camera_to_rgb. Matrices are already
    // WB-gain-folded (raw_render_params_builder.cpp), exposure-independent.
    real32 a = rgb[0], b = rgb[1], c = rgb[2];
    real32 p_r, p_g, p_b;
    DoBaselineABCtoRGB(&a, &b, &c, &p_r, &p_g, &p_b, 1,
                       params->camera_white_vec, params->camera_to_rgb_mat);

    // Step 2: exposure ramp at this candidate ev (no HueSatMap on the generic
    // route -- identity, matching params.huesat_has_table == 0).
    double white = 0.0, black = 0.0;
    exposureWhiteBlack(ev, white, black);
    const dng_function_exposure_ramp ramp_fn(white, black, black);

    dng_host host;
    dng_1d_table ramp_table;
    ramp_table.Initialize(host.Allocator(), ramp_fn);

    real32 e_r, e_g, e_b;
    DoBaseline1DTable(&p_r, &e_r, 1, ramp_table);
    DoBaseline1DTable(&p_g, &e_g, 1, ramp_table);
    DoBaseline1DTable(&p_b, &e_b, 1, ramp_table);

    // Step 3: baseline (ACR3) tone curve, hue-preserving RGBTone remap, same
    // composition order as dng_render_params_builder.cpp / dng_render.cpp:
    // acr3(exposureTone(x)) -- exposureTone first, contrast curve on top.
    const dng_function_exposure_tone exposure_tone(static_cast<real64>(ev));
    const dng_1d_function& acr3_tone = dng_tone_curve_acr3_default::Get();
    const dng_1d_concatenate total_tone(exposure_tone, acr3_tone);

    dng_1d_table tone_table;
    tone_table.Initialize(host.Allocator(), total_tone);

    real32 t_r, t_g, t_b;
    DoBaselineRGBTone(&e_r, &e_g, &e_b, &t_r, &t_g, &t_b, 1, tone_table);

    // Step 4: rgb_to_final matrix (ProPhoto -> sRGB primaries), then encode
    // gamma. RenderParams carries no encode_gamma dng_1d_function directly
    // (raw_render_params_builder.cpp samples dng_space_sRGB::Get().GammaFunction()
    // into params.encode_gamma), so build the table from the same function
    // object rather than resampling the already-sampled vector by hand.
    real32 f_r, f_g, f_b;
    DoBaselineRGBtoRGB(&t_r, &t_g, &t_b, &f_r, &f_g, &f_b, 1, params->rgb_to_final_mat);

    dng_1d_table gamma_table;
    gamma_table.Initialize(host.Allocator(), dng_space_sRGB::Get().GammaFunction());

    real32 g_r, g_g, g_b;
    DoBaseline1DTable(&f_r, &g_r, 1, gamma_table);
    DoBaseline1DTable(&f_g, &g_g, 1, gamma_table);
    DoBaseline1DTable(&f_b, &g_b, 1, gamma_table);

    // Luma: the estimator's bisection target is "does the rendered highlight
    // hit output white" -- max() of the three channels is the appropriate
    // scalar for a highlight/clip test (any channel saturating IS the clip),
    // not a perceptual luma weighting.
    return std::max(g_r, std::max(g_g, g_b));
}
