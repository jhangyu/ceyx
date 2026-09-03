// raw_auto_exposure.cpp - see raw_auto_exposure.h for the interface contract
// and docs/logs/2026-09-03/raw_color_implementation_plan.md Round 1 Task 1.2
// for the design. No LibRaw/Halide/DNG SDK header included.
#include "raw_auto_exposure.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

void setReason(RawAutoExposureResult& r, const char* msg) {
    std::snprintf(r.reason, sizeof(r.reason), "%s", msg);
}

}  // namespace

RawAutoExposureResult raw_auto_exposure_estimate(const uint16_t* samples,
                                                  uint32_t width, uint32_t height,
                                                  uint32_t row_pitch_samples,
                                                  uint32_t stride_x, uint32_t stride_y,
                                                  const float black[4], float white_level,
                                                  const float wb_gain[4],
                                                  const uint8_t* colour_of_site,
                                                  uint32_t pattern_w, uint32_t pattern_h,
                                                  RawRenderEvalFn render_eval,
                                                  void* render_eval_ctx,
                                                  float auto_bright_thr) {
    RawAutoExposureResult result;

    // Revision 2.1: pattern_w == 0 means interleaved components (Foveon/
    // linear RGB) -- pattern_h is reused to carry components_per_pixel in
    // that mode; colour_of_site is unused. Otherwise colour_of_site is a
    // pattern_w x pattern_h lookup table and must be present and valid.
    const bool interleaved = (pattern_w == 0);
    if (!interleaved) {
        if (colour_of_site == nullptr || pattern_w == 0 || pattern_h == 0) {
            result.status = RawAutoExposureStatus::kUnsupportedLayout;
            result.auto_ev = 0.0f;
            setReason(result, "colour_of_site missing for a patterned layout");
            return result;
        }
        const size_t table_len = static_cast<size_t>(pattern_w) * pattern_h;
        for (size_t i = 0; i < table_len; ++i) {
            if (colour_of_site[i] >= 3) {
                result.status = RawAutoExposureStatus::kUnsupportedLayout;
                result.auto_ev = 0.0f;
                setReason(result, "colour_of_site entry out of range (>= 3)");
                return result;
            }
        }
    } else if (pattern_h == 0) {
        result.status = RawAutoExposureStatus::kUnsupportedLayout;
        result.auto_ev = 0.0f;
        setReason(result, "components_per_pixel is zero in interleaved mode");
        return result;
    }
    const uint32_t components_per_pixel = pattern_h;  // only meaningful when interleaved

    const uint32_t sx = stride_x < 1 ? 1 : stride_x;
    const uint32_t sy = stride_y < 1 ? 1 : stride_y;

    // Sample count is decided from geometry alone, before touching the
    // buffer, so this guard is cheap and does not depend on black/white
    // being sane.
    const uint64_t num_x = (static_cast<uint64_t>(width) + sx - 1) / sx;
    const uint64_t num_y = (static_cast<uint64_t>(height) + sy - 1) / sy;
    const uint64_t num_sampled = num_x * num_y;
    if (num_sampled < 4096) {
        result.status = RawAutoExposureStatus::kInsufficientSamples;
        result.auto_ev = 0.0f;
        setReason(result, "fewer than 4096 sampled pixels");
        return result;
    }

    float max_black = black[0];
    for (int c = 1; c < 4; ++c) max_black = std::max(max_black, black[c]);

    // Revision 2.2: no render-chain callback -> kNoRenderEval, auto_ev = 0.
    // Checked before the (otherwise wasted) histogram scan and before the
    // white/black geometry guard -- a missing callback is a caller-config
    // error, not a frame-content one, and must never silently fall back to
    // the withdrawn raw-domain formula.
    if (render_eval == nullptr) {
        result.status = RawAutoExposureStatus::kNoRenderEval;
        result.auto_ev = 0.0f;
        setReason(result, "no render_eval callback supplied");
        return result;
    }

    if (!std::isfinite(white_level) || white_level <= max_black) {
        result.status = RawAutoExposureStatus::kDegenerateFrame;
        result.auto_ev = 0.0f;
        setReason(result, "white_level <= max(black) or non-finite");
        return result;
    }

    // Black-subtracted, white-normalised histogram values, kept PER COLOUR
    // CLASS (Revision 2.2's Behavior block, corrected by Revision 2.4): the
    // estimator needs the highlight triple h = (q_R, q_G, q_B) -- exactly the
    // triple Stage 4 receives -- not one pooled quantile, so a strongly
    // tinted frame cannot have its clipped channel averaged away by the other
    // two. Revision 2.4: NOT WB-scaled here. render_eval already folds white
    // balance through camera_to_rgb * diag(gain); scaling these bins too
    // would stack two WB architectures. wb_gain is accepted for signature
    // compatibility / diagnostics only and is unused in this computation.
    (void)wb_gain;
    std::vector<float> values[3];
    for (int c = 0; c < 3; ++c) values[c].reserve(static_cast<size_t>(num_sampled) / 2 + 1);
    for (uint64_t row = 0; row < height; row += sy) {
        const uint16_t* row_ptr = samples + static_cast<size_t>(row) * row_pitch_samples;
        for (uint64_t col = 0; col < width; col += sx) {
            uint32_t ch;
            if (interleaved) {
                ch = static_cast<uint32_t>(col) % components_per_pixel;
                if (ch >= 4) ch = ch % 4;  // clamp into the black[]/wb_gain[] range
            } else {
                const uint32_t site_row = static_cast<uint32_t>(row) % pattern_h;
                const uint32_t site_col = static_cast<uint32_t>(col) % pattern_w;
                ch = colour_of_site[site_row * pattern_w + site_col];
            }
            const float black_ch = black[ch];
            const float denom = white_level - black_ch;
            float v = (static_cast<float>(row_ptr[col]) - black_ch) / denom;
            v = std::max(0.0f, v);
            const uint32_t colour_class = ch < 3 ? ch : (ch % 3);
            values[colour_class].push_back(v);
        }
    }

    const float thr = auto_bright_thr <= 0.0f ? 0.01f : auto_bright_thr;
    const double keep_fraction = 1.0 - static_cast<double>(thr);
    float h[3] = {0.0f, 0.0f, 0.0f};
    for (int c = 0; c < 3; ++c) {
        if (values[c].empty()) continue;  // a class this layout never emits
        size_t quantile_idx = static_cast<size_t>(std::min<double>(
            values[c].size() - 1,
            std::max<double>(0.0, keep_fraction * static_cast<double>(values[c].size()))));
        std::nth_element(values[c].begin(), values[c].begin() + static_cast<long>(quantile_idx),
                          values[c].end());
        h[c] = values[c][quantile_idx];
    }

    // clip_value stays the GREEN-class quantile for diagnostics (Revision
    // 2.2's Behavior block); it no longer determines auto_ev directly.
    const float clip_value = h[1];
    if (!std::isfinite(h[0]) || !std::isfinite(h[1]) || !std::isfinite(h[2]) ||
        (h[0] <= 0.0f && h[1] <= 0.0f && h[2] <= 0.0f)) {
        result.status = RawAutoExposureStatus::kDegenerateFrame;
        result.auto_ev = 0.0f;
        result.clip_value = std::isfinite(clip_value) ? clip_value : 0.0f;
        setReason(result, "highlight triple non-finite or all <= 0");
        return result;
    }

    result.clip_value = clip_value;
    result.status = RawAutoExposureStatus::kOk;

    // Bisection solve (Revision 2.2): find ev in [0, 2] such that
    // render_eval(ctx, h, ev) == 1.0. There is no closed form -- render_eval
    // samples a tone curve that itself depends on ev, so the dependency is
    // circular. 30 iterations on the ONE triple h, not an image.
    const float f_lo = render_eval(render_eval_ctx, h, 0.0f);
    if (!std::isfinite(f_lo)) {
        result.status = RawAutoExposureStatus::kDegenerateFrame;
        result.auto_ev = 0.0f;
        setReason(result, "render_eval returned non-finite at ev=0");
        return result;
    }
    if (f_lo >= 1.0f) {
        // Already at or above output white with no gain: the reported defect
        // is darkness, so a would-be-negative EV is clamped to 0, not applied
        // -- and that clamp is observable, not silent.
        result.auto_ev = 0.0f;
        setReason(result, "already at or above output white at ev=0; no gain applied");
        return result;
    }

    float lo = 0.0f, hi = 2.0f;
    const float f_hi = render_eval(render_eval_ctx, h, hi);
    if (!std::isfinite(f_hi)) {
        result.status = RawAutoExposureStatus::kDegenerateFrame;
        result.auto_ev = 0.0f;
        setReason(result, "render_eval returned non-finite at ev=2");
        return result;
    }
    if (f_hi < 1.0f) {
        // The render never reaches output white inside the [0, 2] ceiling:
        // clamp to the ceiling rather than extrapolate past the contract's
        // range.
        result.auto_ev = hi;
    } else {
        for (int i = 0; i < 30; ++i) {
            const float mid = 0.5f * (lo + hi);
            const float f_mid = render_eval(render_eval_ctx, h, mid);
            if (!std::isfinite(f_mid) || f_mid < 1.0f) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        result.auto_ev = 0.5f * (lo + hi);
    }

    // Guard: never let a non-finite value escape, whichever branch produced
    // auto_ev.
    if (!std::isfinite(result.auto_ev)) {
        result.status = RawAutoExposureStatus::kDegenerateFrame;
        result.auto_ev = 0.0f;
        setReason(result, "auto_ev non-finite after bisection");
    }

    return result;
}
