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
};

struct RawAutoExposureResult {
    float auto_ev = 0.0f;        // clamped to [0, +2]; 0 means "no gain applied"
    float clip_value = 0.0f;     // the normalised value at the (1 - thr) quantile
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
// cfa_channel_of: maps a 2x2 CFA cell position (row%2)*2 + (col%2) to one of
//   the four black[]/wb_gain[] channel indices.
RawAutoExposureResult raw_auto_exposure_estimate(const uint16_t* samples,
                                                  uint32_t width, uint32_t height,
                                                  uint32_t row_pitch_samples,
                                                  uint32_t stride_x, uint32_t stride_y,
                                                  const float black[4], float white_level,
                                                  const float wb_gain[4],
                                                  uint32_t cfa_channel_of[4],
                                                  float auto_bright_thr = 0.01f);

#endif  // RAW_AUTO_EXPOSURE_H_
