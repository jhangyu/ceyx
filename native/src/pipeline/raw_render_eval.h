// raw_render_eval.h - the production RawRenderEvalFn implementation.
//
// Plan: docs/logs/2026-09-03/raw_color_implementation_plan.md Round 1 Task
// 1.7. Unlike raw_auto_exposure.h, this header (and its .cpp) MAY include the
// DNG SDK freely -- it is the one deliberate hole in the estimator's
// SDK-free constraint, kept in its own translation unit so the estimator
// itself stays testable with no raw file and no SDK.
#ifndef RAW_RENDER_EVAL_H_
#define RAW_RENDER_EVAL_H_

#include "dng_render_params.h"
#include "raw_auto_exposure.h"  // RawRenderEvalFn

// ctx must be a `const RenderParams*` (matrices already folded with the WB
// gain, per raw_render_params_builder.cpp). Evaluates the SAME composition
// the builder configures, on ONE linear camera triple `rgb`, at exposure
// `ev`: clip vs camera_white -> camera_to_rgb -> rgb_to_final -> exp_ramp /
// tone_curve resampled at ev -> encode_gamma -> luma. Calls the SDK's own
// dng_function_exposure_ramp / dng_function_exposure_tone /
// dng_tone_curve_acr3_default / dng_1d_concatenate and dng_bottlenecks
// reference sampling functions rather than re-deriving the tone maths by
// hand (a review reject per the plan).
float raw_render_eval_from_params(void* ctx, const float rgb[3], float ev);

#endif  // RAW_RENDER_EVAL_H_
