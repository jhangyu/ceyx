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
    if (!std::isfinite(white_level) || white_level <= max_black) {
        result.status = RawAutoExposureStatus::kDegenerateFrame;
        result.auto_ev = 0.0f;
        setReason(result, "white_level <= max(black) or non-finite");
        return result;
    }

    // Black-subtracted, white-normalised, WB-scaled histogram values. WB
    // scaling is applied to these bins only -- the pixel path is untouched --
    // matching LibRaw's post-white-balance auto-bright histogram.
    std::vector<float> values;
    values.reserve(static_cast<size_t>(num_sampled));
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
            v = std::max(0.0f, v) * wb_gain[ch];
            values.push_back(v);
        }
    }

    const float thr = auto_bright_thr <= 0.0f ? 0.01f : auto_bright_thr;
    const double keep_fraction = 1.0 - static_cast<double>(thr);
    size_t quantile_idx = static_cast<size_t>(
        std::min<double>(values.size() - 1,
                          std::max<double>(0.0, keep_fraction * static_cast<double>(values.size()))));
    std::nth_element(values.begin(), values.begin() + static_cast<long>(quantile_idx), values.end());
    const float clip_value = values[quantile_idx];

    if (!std::isfinite(clip_value) || clip_value <= 0.0f) {
        result.status = RawAutoExposureStatus::kDegenerateFrame;
        result.auto_ev = 0.0f;
        result.clip_value = std::isfinite(clip_value) ? clip_value : 0.0f;
        setReason(result, "clip_value non-finite or <= 0");
        return result;
    }

    result.clip_value = clip_value;
    result.status = RawAutoExposureStatus::kOk;

    float ev = std::log2(1.0f / clip_value);
    if (!std::isfinite(ev)) {
        result.status = RawAutoExposureStatus::kDegenerateFrame;
        result.auto_ev = 0.0f;
        setReason(result, "computed EV non-finite");
        return result;
    }

    if (ev < 0.0f) {
        // The reported defect is darkness; an auto-darkening result means the
        // estimator is wrong and must be visible, not applied.
        result.auto_ev = 0.0f;
        setReason(result, "computed gain below 1.0 (negative EV) clamped to 0");
    } else {
        result.auto_ev = std::min(ev, 2.0f);
    }

    // Guard: never let a non-finite value escape, whichever branch produced
    // auto_ev.
    if (!std::isfinite(result.auto_ev)) {
        result.status = RawAutoExposureStatus::kDegenerateFrame;
        result.auto_ev = 0.0f;
        setReason(result, "auto_ev non-finite after clamp");
    }

    return result;
}
