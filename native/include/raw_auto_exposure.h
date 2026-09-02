// raw_auto_exposure.h - histogram-based automatic exposure gain estimator.
//
// Plan: docs/logs/2026-09-03/raw_color_implementation_plan.md, Round 1 Task 1.2.
// Pure math over a caller-supplied buffer view: no LibRaw, Halide, or DNG SDK
// header may be pulled in here, and none is -- this unit is testable on a
// synthetic buffer with no raw file. The scan mode (full vs strided) is the
// caller's policy choice via stride_x/stride_y; this unit does not decide it.
#ifndef RAW_AUTO_EXPOSURE_H_
#define RAW_AUTO_EXPOSURE_H_

#include <cstdint>

enum class RawAutoExposureStatus {
    kOk = 0,
    kDegenerateFrame,
    kInsufficientSamples,
    kUnsupportedLayout,  // Revision 2.1: a layout we cannot classify says so,
                         // rather than returning 0 EV that reads as "this
                         // frame needed no gain".
    kNoRenderEval,       // Revision 2.2: no render-chain callback supplied.
                         // Never falls back to the withdrawn raw-domain
                         // formula (log2(1.0 / clip_value)).
};

// Revision 2.2 -- the render-chain evaluator. The estimator solves in the
// OUTPUT domain (post camera->RGB matrix, post tone/gamma) and needs the
// pipeline's own tone/matrix math to do that, but this header must stay free
// of DNG-SDK includes, so the chain arrives as a callback rather than a
// direct call. Returns the OUTPUT-domain luma in [0,1] that the pipeline
// would produce for the linear camera triple `rgb` at exposure `ev`.
// Implemented for production in raw_render_eval.cpp; tests pass an analytic
// stub instead.
using RawRenderEvalFn = float (*)(void* ctx, const float rgb[3], float ev);

struct RawAutoExposureResult {
    float auto_ev = 0.0f;        // clamped to [0, +2]; 0 means "no gain applied"
    float clip_value = 0.0f;     // Revision 2.2: diagnostics only -- the green-class
                                  // (1 - thr) quantile. No longer determines auto_ev
                                  // directly; the bisection solve does.
    RawAutoExposureStatus status = RawAutoExposureStatus::kOk;
    char reason[96] = {0};       // empty iff status == kOk
};

// samples: unpacked u16 raw, row-major, `stride_x`/`stride_y` >= 1 subsampling.
// black/white: per-CFA-channel black level and white level, already known to
//   the adapter.
// wb_gain: the gains implied by as_shot_neutral, applied to the HISTOGRAM BINS
//   only -- LibRaw builds its histogram after white balance, and a strongly
//   tinted frame would otherwise clip on a single channel. The pixel path is
//   NOT touched.
// colour_of_site: per-SITE colour class (0..2, R/G/B), not CFA geometry.
//   colour_of_site[(y % pattern_h) * pattern_w + (x % pattern_w)] gives the
//   class of the sample at (x, y).
//     Bayer:             pattern 2x2.
//     X-Trans:           pattern 6x6, fed straight from
//                        LibRawRawView::xtrans_pattern (36 bytes row-major).
//     Foveon/linear RGB: pattern_w == 0 means interleaved components; class =
//                        sample_index % components_per_pixel.
//   colour_of_site == nullptr with pattern_w != 0 => kUnsupportedLayout, as
//   does any table entry >= 3.
// render_eval/render_eval_ctx: Revision 2.2 -- solves for the exposure whose
//   RENDERED highlight triple lands on output white, by bisection through
//   this callback (30 iterations on the one (q_R,q_G,q_B) triple, not an
//   image). nullptr => kNoRenderEval.
RawAutoExposureResult raw_auto_exposure_estimate(const uint16_t* samples,
                                                  uint32_t width, uint32_t height,
                                                  uint32_t row_pitch_samples,
                                                  uint32_t stride_x, uint32_t stride_y,
                                                  const float black[4], float white_level,
                                                  const float wb_gain[4],
                                                  const uint8_t* colour_of_site,
                                                  uint32_t pattern_w, uint32_t pattern_h,
                                                  // Revision 2.2 -- the render-chain evaluator that
                                                  // resolves the circular dependency between ev and
                                                  // the tone curve by bisection. nullptr =>
                                                  // kNoRenderEval, auto_ev = 0 -- never a silent
                                                  // fallback to the withdrawn raw-domain formula.
                                                  RawRenderEvalFn render_eval, void* render_eval_ctx,
                                                  float auto_bright_thr = 0.01f);

#endif  // RAW_AUTO_EXPOSURE_H_
