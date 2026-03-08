#pragma once

#include "DngDecoder.h" // DngMetadata::HSBEntry
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

/// ---------------------------------------------------------------------------
/// Lightroom 8-channel HSL adjustment LUT generator.
///
/// DESIGN DECISION — 1D hue-based map (with sD=2) vs 3D RGB LUT:
///
///   Lightroom HSL adjustments are fundamentally hue-dependent with
///   saturation-proportional falloff. A 1D map indexed by hue (with sD=2
///   for achromatic falloff) exactly represents this semantic, requiring
///   only 180 entries vs 4913 for a 17^3 3D LUT.
///
///   Advantages:
///     - Exact: no RGB->HSV->RGB roundtrip approximation at grid points
///     - Tiny:  90*2*1 = 180 floats vs 17*17*17*3 = ~15K floats
///     - Reuses existing apply_3dlut Halide function unchanged
///     - Matching DngMetadata::HSBEntry format for trivial integration
///
///   The LUT is consumed by the same apply_3dlut() Halide stage used for
///   HueSatMap and LookTable, just at a different point in the pipeline
///   (after ToneCurve, before Saturation/Vibrance — matching Lightroom's
///   Develop module order).
/// ---------------------------------------------------------------------------

namespace hsl {

// ---- Channel definitions ---------------------------------------------------

enum Channel : int {
  kRed = 0,
  kOrange = 1,
  kYellow = 2,
  kGreen = 3,
  kAqua = 4,
  kBlue = 5,
  kPurple = 6,
  kMagenta = 7,
  kNumChannels = 8
};

/// Center hues (degrees) for each Lightroom HSL channel.
/// Non-uniform spacing: warm tones (Red/Orange/Yellow) and cool accents
/// (Purple/Magenta) are 30 deg apart; mid-range colors are 60 deg apart.
///
///   Red(0) -30-> Orange(30) -30-> Yellow(60) -60-> Green(120)
///   -60-> Aqua(180) -60-> Blue(240) -30-> Purple(270) -30-> Magenta(300)
///   -60-> Red(360/0)
static constexpr float kChannelCenters[kNumChannels] = {
    0.0f,   // Red
    30.0f,  // Orange
    60.0f,  // Yellow
    120.0f, // Green
    180.0f, // Aqua
    240.0f, // Blue
    270.0f, // Purple
    300.0f  // Magenta
};

/// XMP tag names for each channel (used during parsing).
static constexpr const char *kChannelNames[kNumChannels] = {
    "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};

// ---- Parameters ------------------------------------------------------------

/// Raw HSL adjustment parameters from Lightroom XMP (crs:*Adjustment*).
/// All values in [-100, +100] range.
struct HslParams {
  float hueAdj[kNumChannels];
  float satAdj[kNumChannels];
  float lumAdj[kNumChannels];

  HslParams() {
    for (int i = 0; i < kNumChannels; i++)
      hueAdj[i] = satAdj[i] = lumAdj[i] = 0.0f;
  }

  /// Returns true if all 24 adjustments are zero (no LUT needed).
  bool isIdentity() const {
    for (int i = 0; i < kNumChannels; i++) {
      if (hueAdj[i] != 0.0f || satAdj[i] != 0.0f || lumAdj[i] != 0.0f)
        return false;
    }
    return true;
  }
};

// ---- Weight computation ----------------------------------------------------

/// Compute piecewise-linear channel weights at a given hue angle.
///
/// For any hue falling between two adjacent channel centers, the weights
/// form a linear blend (partition of unity). Exactly two channels have
/// non-zero weights at any point, producing smooth transitions identical
/// to Lightroom's documented behavior.
///
/// @param hueDeg  Hue angle in degrees [0, 360)
/// @param weights Output array of 8 weights, summing to 1.0
inline void computeChannelWeights(float hueDeg,
                                  float weights[kNumChannels]) {
  for (int i = 0; i < kNumChannels; i++)
    weights[i] = 0.0f;

  // Normalize to [0, 360)
  hueDeg = std::fmod(hueDeg, 360.0f);
  if (hueDeg < 0.0f)
    hueDeg += 360.0f;

  // Find enclosing channel pair on the hue circle
  for (int i = 0; i < kNumChannels; i++) {
    int next = (i + 1) % kNumChannels;
    float left = kChannelCenters[i];
    float right = kChannelCenters[next];

    // Handle wrap: Magenta(300) -> Red(0/360)
    if (right <= left)
      right += 360.0f;

    float h = hueDeg;
    if (h < left)
      h += 360.0f;

    if (h >= left && h < right) {
      float span = right - left;
      float t = (h - left) / span;
      weights[i] = 1.0f - t;
      weights[next] = t;
      return;
    }
  }

  // Fallback (should not reach here for valid input)
  weights[kRed] = 1.0f;
}

// ---- LUT generation --------------------------------------------------------

/// Generate a HueSatMap-compatible LUT from Lightroom HSL adjustments.
///
/// Output layout matches DngMetadata conventions (val-hue-sat order,
/// outer -> inner), so it can be directly consumed by the existing
/// apply_3dlut Halide function.
///
/// Dimensions:
///   hD = hueDivisions (default 90, i.e., 4 deg per step)
///   sD = 2 (identity at s=0, full effect at s=1)
///   vD = 1 (uniform across brightness)
///
/// Slider-to-HSBModify conversion:
///   HueAdj   [-100,+100] -> hueShift: +/-30 deg  (value * 0.30)
///   SatAdj   [-100,+100] -> satScale: [0.0, 2.0]  (1.0 + value/100)
///   LumAdj   [-100,+100] -> valScale: [0.0, 2.0]  (1.0 + value/100)
///
/// @param params        The 24 HSL adjustment values
/// @param outHD         [out] Hue divisions
/// @param outSD         [out] Saturation divisions
/// @param outVD         [out] Value divisions
/// @param outData       [out] LUT entries (size = outHD * outSD * outVD)
/// @param hueDivisions  Resolution of the hue axis (default: 90)
inline void generateHslLut(const HslParams &params, uint32_t &outHD,
                           uint32_t &outSD, uint32_t &outVD,
                           std::vector<DngMetadata::HSBEntry> &outData,
                           int hueDivisions = 90) {
  outHD = static_cast<uint32_t>(hueDivisions);
  outSD = 2; // identity at s=0, full effect at s=1
  outVD = 1;

  size_t total = static_cast<size_t>(outHD) * outSD * outVD;
  outData.resize(total);

  for (int hi = 0; hi < hueDivisions; hi++) {
    float hueDeg =
        static_cast<float>(hi) * 360.0f / static_cast<float>(hueDivisions);

    float weights[kNumChannels];
    computeChannelWeights(hueDeg, weights);

    // Weighted blend of all channel adjustments
    float hueShiftDeg = 0.0f;
    float satAdjBlend = 0.0f;
    float lumAdjBlend = 0.0f;

    for (int ch = 0; ch < kNumChannels; ch++) {
      // Hue: slider value * 0.30 gives +/-30 deg at +/-100
      hueShiftDeg += weights[ch] * params.hueAdj[ch] * 0.30f;
      // Sat/Lum: blend raw slider values, convert to scale below
      satAdjBlend += weights[ch] * params.satAdj[ch];
      lumAdjBlend += weights[ch] * params.lumAdj[ch];
    }

    // Convert blended slider values to multiplicative scale factors
    float satScale = 1.0f + satAdjBlend / 100.0f;
    satScale = std::max(0.0f, std::min(satScale, 2.0f));

    float valScale = 1.0f + lumAdjBlend / 100.0f;
    valScale = std::max(0.0f, std::min(valScale, 2.0f));

    // Layout: val-hue-sat (outer -> inner), matching DNG SDK convention.
    // table[v * hD * sD + h * sD + s]

    // s=0 (achromatic): identity — no HSL adjustment for grayscale pixels
    size_t idx_s0 = static_cast<size_t>(hi) * outSD + 0;
    outData[idx_s0] = {0.0f, 1.0f, 1.0f};

    // s=1 (fully saturated): full adjustment
    size_t idx_s1 = static_cast<size_t>(hi) * outSD + 1;
    outData[idx_s1] = {hueShiftDeg, satScale, valScale};
  }
}

} // namespace hsl
