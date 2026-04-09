#include "HalidePipeline.h"
#include "HalideBuffer.h"
#include "dng_pipeline.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

/*
---
file_summary: "基於 Halide 的影像處理管線，負責去馬賽克、曝光補償與色彩校正"
functions:
  - name: "rgb2hsv"
    description: "將 RGB 色彩轉換為 HSV"
    lines: "65-88"
  - name: "hsv2rgb"
    description: "將 HSV 色彩轉換為 RGB"
    lines: "90-129"
  - name: "applyHueSatMapPixel"
    description: "CPU 端 3D LUT (HSV 空間) 三線性插值實作"
    lines: "131-200"
  - name: "evalToneCurve"
    description: "分段線性 ToneCurve 估值 (Phase 5.3)"
    lines: "202-223"
  - name: "evalAcrLUT"
    description: "使用 ACR3 預設的 1025-entry LUT 進行線性插值"
    lines: "376-388"
  - name: "CachedPipeline"
    description: "JIT-cached Halide pipeline 結構體，包含所有 Halide Func
宣告與建立" lines: "402-984"
  - name: "CachedPipeline::emit_rgb2hsv"
    description: "Halide Expr 版本的 RGB 轉 HSV"
    lines: "435-450"
  - name: "CachedPipeline::emit_hsv2rgb"
    description: "Halide Expr 版本的 HSV 轉 RGB"
    lines: "452-468"
  - name: "CachedPipeline::apply_3dlut"
    description: "Halide func 建立 3D LUT (HueSatMap/LookTable) 應用節點"
    lines: "470-544"
  - name: "CachedPipeline::apply_tone_curve"
    description: "Halide func 建立 Tone Curve 應用節點"
    lines: "546-556"
  - name: "CachedPipeline::apply_lr_params"
    description: "Halide func 建立 Exposure, Contrast, Saturation, Vibrance
應用節點" lines: "558-600"
  - name: "CachedPipeline::apply_gamma"
    description: "Halide func 建立 sRGB Gamma 轉換與打包為 uint8 節點"
    lines: "602-612"
  - name: "CachedPipeline::build"
    description: "建構 Halide graph，包含 GPU (Metal) target 偵測、排程與 JIT
編譯" lines: "614-983"
  - name: "getPipeline"
    description: "取得 CachedPipeline 的全域單例 (Singleton)"
    lines: "985-992"
  - name: "HalidePipeline::process"
    description: "管線主入口：綁定參數、執行 cached realize，並含分段效能計時
(Phase 6.4.1)" lines: "993-1250"
---
*/

// ============================================================================
// HalidePipeline::process — Phase 3+5 pipeline
//
// Pipeline stages:
//  1. Black-level subtraction & linearisation [0,1]
//  2. White balance — handled by CameraToPCS matrix (no explicit gains)
//  3. AHD demosaicing (RGGB CFA)
//  4. Camera -> sRGB (composite matrix)
//  5. BaselineExposure compensation
//  [Halide realize -> float RGB buffer]
//  5a. HueSatMap 3D LUT apply (CPU trilinear interp in HSV space)
//  5b. LookTable 3D LUT apply (CPU trilinear interp in HSV space)
//  5d. ProfileToneCurve (ACR3 default 1025-LUT or profile curve)
//  5c. Lightroom XMP params:
//      - Exposure2012 (EV gain on V channel)
//      - Contrast2012 (midpoint-pivot scaling on RGB, Phase 5.3)
//      - Saturation    (non-linear on S channel, Phase 5.3 refined)
//      - Vibrance      (quadratic-decay boost on S, Phase 5.3 refined)
//  6. sRGB gamma correction + pack to uint8 RGBA
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
// RGB <-> HSV helpers
// ---------------------------------------------------------------------------
inline void rgb2hsv(float r, float g, float b, float &h, float &s, float &v) {
  float maxc = std::max({r, g, b});
  float minc = std::min({r, g, b});
  float chroma = maxc - minc;
  v = maxc;
  s = (maxc > 1e-6f) ? (chroma / maxc) : 0.0f;
  if (chroma < 1e-6f) {
    h = 0.0f;
    return;
  }
  float hue6;
  if (maxc == r)
    hue6 = (g - b) / chroma;
  else if (maxc == g)
    hue6 = (b - r) / chroma + 2.0f;
  else
    hue6 = (r - g) / chroma + 4.0f;
  if (hue6 < 0.0f)
    hue6 += 6.0f;
  h = hue6 / 6.0f; // normalize to [0, 1)
}

inline void hsv2rgb(float h, float s, float v, float &r, float &g, float &b) {
  float h6 = h * 6.0f;
  int hi = (int)std::floor(h6) % 6;
  float f = h6 - std::floor(h6);
  float p = v * (1.0f - s);
  float q = v * (1.0f - s * f);
  float t = v * (1.0f - s * (1.0f - f));
  switch (hi) {
  case 0:
    r = v;
    g = t;
    b = p;
    break;
  case 1:
    r = q;
    g = v;
    b = p;
    break;
  case 2:
    r = p;
    g = v;
    b = t;
    break;
  case 3:
    r = p;
    g = q;
    b = v;
    break;
  case 4:
    r = t;
    g = p;
    b = v;
    break;
  default:
    r = v;
    g = p;
    b = q;
    break;
  }
}

// ---------------------------------------------------------------------------
// Apply one HueSatMap (or LookTable) to a single float RGB pixel in-place.
// Layout: stored in val-hue-sat order (outer->inner), consistent with
// dng_hue_sat_map::GetConstDeltas().
// ---------------------------------------------------------------------------
inline void applyHueSatMapPixel(float &r, float &g, float &b,
                                const std::vector<DngMetadata::HSBEntry> &table,
                                int hD, int sD, int vD) {
  const float hScale = (float)hD;
  const float sScale = (float)(sD - 1);
  const float vScale = (float)(vD - 1);

  float h, s, v;
  rgb2hsv(r, g, b, h, s, v);

  // Skip achromatic pixels
  if (s < 1e-6f)
    return;

  // Bilinear/trilinear index computation
  float hf = h * hScale;
  hf -= std::floor(hf / (float)hD) * (float)hD; // wrap into [0, hD)
  float sf = std::min(s * sScale, (float)(sD - 1));
  float vf = std::min(v * vScale, (float)(vD - 1));

  int h0 = (int)hf;
  int h1 = (h0 + 1) % hD;
  int s0 = (int)sf;
  int s1 = std::min(s0 + 1, sD - 1);
  int v0 = (int)vf;
  int v1 = std::min(v0 + 1, vD - 1);
  float hfrac = hf - (float)h0;
  float sfrac = sf - (float)s0;
  float vfrac = vf - (float)v0;

  // Helper to get entry at (vi, hi, si)
  auto entry = [&](int vi, int hi, int si) -> const DngMetadata::HSBEntry & {
    return table[(size_t)vi * hD * sD + (size_t)hi * sD + si];
  };

  // Trilinear interpolation for each component
  auto lerp3 = [&](auto fn) {
    float c000 = fn(entry(v0, h0, s0)), c001 = fn(entry(v0, h0, s1));
    float c010 = fn(entry(v0, h1, s0)), c011 = fn(entry(v0, h1, s1));
    float c100 = fn(entry(v1, h0, s0)), c101 = fn(entry(v1, h0, s1));
    float c110 = fn(entry(v1, h1, s0)), c111 = fn(entry(v1, h1, s1));
    float c00 = c000 + vfrac * (c100 - c000);
    float c01 = c001 + vfrac * (c101 - c001);
    float c10 = c010 + vfrac * (c110 - c010);
    float c11 = c011 + vfrac * (c111 - c011);
    float c0 = c00 + hfrac * (c10 - c00);
    float c1 = c01 + hfrac * (c11 - c01);
    return c0 + sfrac * (c1 - c0);
  };

  float hShift =
      lerp3([](const DngMetadata::HSBEntry &e) { return e.hueShift; });
  float sScale_v =
      lerp3([](const DngMetadata::HSBEntry &e) { return e.satScale; });
  float vScale_v =
      lerp3([](const DngMetadata::HSBEntry &e) { return e.valScale; });

  // Apply: hueShift in degrees (full circle=360)
  float newH = h + hShift / 360.0f;
  newH = newH - std::floor(newH); // wrap to [0,1)
  float newS = std::min(std::max(s * sScale_v, 0.0f), 1.0f);
  float newV = std::min(std::max(v * vScale_v, 0.0f), 1.0f);

  hsv2rgb(newH, newS, newV, r, g, b);
}

// ---------------------------------------------------------------------------
// Phase 5.3 — ProfileToneCurve: piecewise-linear channel-wise mapping.
// pts: interleaved (input, output) pairs in [0,1], count: number of pairs.
// Applied per-channel after LookTable (DNG SDK order).
// ---------------------------------------------------------------------------
static float evalToneCurve(const double *pts, int count, float x) {
  if (count == 0)
    return x; // identity fallback
  if (x <= (float)pts[0])
    return (float)pts[1];
  if (x >= (float)pts[(count - 1) * 2])
    return (float)pts[(count - 1) * 2 + 1];
  for (int i = 0; i < count - 1; i++) {
    float x0 = (float)pts[i * 2], y0 = (float)pts[i * 2 + 1];
    float x1 = (float)pts[(i + 1) * 2], y1 = (float)pts[(i + 1) * 2 + 1];
    if (x <= x1) {
      float t = (x - x0) / (x1 - x0);
      return y0 + t * (y1 - y0);
    }
  }
  return (float)pts[(count - 1) * 2 + 1]; // clamp to last
}

// Adobe ACR3 default tone curve — 1025-entry LUT from dng_render.cpp
// x = i/1024 (linear [0,1]) → output value
static const float kAcrDefaultLUT[1025] = {
    0.00000f, 0.00078f, 0.00160f, 0.00242f, 0.00314f, 0.00385f, 0.00460f,
    0.00539f, 0.00623f, 0.00712f, 0.00806f, 0.00906f, 0.01012f, 0.01122f,
    0.01238f, 0.01359f, 0.01485f, 0.01616f, 0.01751f, 0.01890f, 0.02033f,
    0.02180f, 0.02331f, 0.02485f, 0.02643f, 0.02804f, 0.02967f, 0.03134f,
    0.03303f, 0.03475f, 0.03648f, 0.03824f, 0.04002f, 0.04181f, 0.04362f,
    0.04545f, 0.04730f, 0.04916f, 0.05103f, 0.05292f, 0.05483f, 0.05675f,
    0.05868f, 0.06063f, 0.06259f, 0.06457f, 0.06655f, 0.06856f, 0.07057f,
    0.07259f, 0.07463f, 0.07668f, 0.07874f, 0.08081f, 0.08290f, 0.08499f,
    0.08710f, 0.08921f, 0.09134f, 0.09348f, 0.09563f, 0.09779f, 0.09996f,
    0.10214f, 0.10433f, 0.10652f, 0.10873f, 0.11095f, 0.11318f, 0.11541f,
    0.11766f, 0.11991f, 0.12218f, 0.12445f, 0.12673f, 0.12902f, 0.13132f,
    0.13363f, 0.13595f, 0.13827f, 0.14061f, 0.14295f, 0.14530f, 0.14765f,
    0.15002f, 0.15239f, 0.15477f, 0.15716f, 0.15956f, 0.16197f, 0.16438f,
    0.16680f, 0.16923f, 0.17166f, 0.17410f, 0.17655f, 0.17901f, 0.18148f,
    0.18395f, 0.18643f, 0.18891f, 0.19141f, 0.19391f, 0.19641f, 0.19893f,
    0.20145f, 0.20398f, 0.20651f, 0.20905f, 0.21160f, 0.21416f, 0.21672f,
    0.21929f, 0.22185f, 0.22440f, 0.22696f, 0.22950f, 0.23204f, 0.23458f,
    0.23711f, 0.23963f, 0.24215f, 0.24466f, 0.24717f, 0.24967f, 0.25216f,
    0.25465f, 0.25713f, 0.25961f, 0.26208f, 0.26454f, 0.26700f, 0.26945f,
    0.27189f, 0.27433f, 0.27676f, 0.27918f, 0.28160f, 0.28401f, 0.28641f,
    0.28881f, 0.29120f, 0.29358f, 0.29596f, 0.29833f, 0.30069f, 0.30305f,
    0.30540f, 0.30774f, 0.31008f, 0.31241f, 0.31473f, 0.31704f, 0.31935f,
    0.32165f, 0.32395f, 0.32623f, 0.32851f, 0.33079f, 0.33305f, 0.33531f,
    0.33756f, 0.33981f, 0.34205f, 0.34428f, 0.34650f, 0.34872f, 0.35093f,
    0.35313f, 0.35532f, 0.35751f, 0.35969f, 0.36187f, 0.36404f, 0.36620f,
    0.36835f, 0.37050f, 0.37264f, 0.37477f, 0.37689f, 0.37901f, 0.38112f,
    0.38323f, 0.38533f, 0.38742f, 0.38950f, 0.39158f, 0.39365f, 0.39571f,
    0.39777f, 0.39982f, 0.40186f, 0.40389f, 0.40592f, 0.40794f, 0.40996f,
    0.41197f, 0.41397f, 0.41596f, 0.41795f, 0.41993f, 0.42191f, 0.42388f,
    0.42584f, 0.42779f, 0.42974f, 0.43168f, 0.43362f, 0.43554f, 0.43747f,
    0.43938f, 0.44129f, 0.44319f, 0.44509f, 0.44698f, 0.44886f, 0.45073f,
    0.45260f, 0.45447f, 0.45632f, 0.45817f, 0.46002f, 0.46186f, 0.46369f,
    0.46551f, 0.46733f, 0.46914f, 0.47095f, 0.47275f, 0.47454f, 0.47633f,
    0.47811f, 0.47989f, 0.48166f, 0.48342f, 0.48518f, 0.48693f, 0.48867f,
    0.49041f, 0.49214f, 0.49387f, 0.49559f, 0.49730f, 0.49901f, 0.50072f,
    0.50241f, 0.50410f, 0.50579f, 0.50747f, 0.50914f, 0.51081f, 0.51247f,
    0.51413f, 0.51578f, 0.51742f, 0.51906f, 0.52069f, 0.52232f, 0.52394f,
    0.52556f, 0.52717f, 0.52878f, 0.53038f, 0.53197f, 0.53356f, 0.53514f,
    0.53672f, 0.53829f, 0.53986f, 0.54142f, 0.54297f, 0.54452f, 0.54607f,
    0.54761f, 0.54914f, 0.55067f, 0.55220f, 0.55371f, 0.55523f, 0.55673f,
    0.55824f, 0.55973f, 0.56123f, 0.56271f, 0.56420f, 0.56567f, 0.56715f,
    0.56861f, 0.57007f, 0.57153f, 0.57298f, 0.57443f, 0.57587f, 0.57731f,
    0.57874f, 0.58017f, 0.58159f, 0.58301f, 0.58443f, 0.58583f, 0.58724f,
    0.58864f, 0.59003f, 0.59142f, 0.59281f, 0.59419f, 0.59556f, 0.59694f,
    0.59830f, 0.59966f, 0.60102f, 0.60238f, 0.60373f, 0.60507f, 0.60641f,
    0.60775f, 0.60908f, 0.61040f, 0.61173f, 0.61305f, 0.61436f, 0.61567f,
    0.61698f, 0.61828f, 0.61957f, 0.62087f, 0.62216f, 0.62344f, 0.62472f,
    0.62600f, 0.62727f, 0.62854f, 0.62980f, 0.63106f, 0.63232f, 0.63357f,
    0.63482f, 0.63606f, 0.63730f, 0.63854f, 0.63977f, 0.64100f, 0.64222f,
    0.64344f, 0.64466f, 0.64587f, 0.64708f, 0.64829f, 0.64949f, 0.65069f,
    0.65188f, 0.65307f, 0.65426f, 0.65544f, 0.65662f, 0.65779f, 0.65897f,
    0.66013f, 0.66130f, 0.66246f, 0.66362f, 0.66477f, 0.66592f, 0.66707f,
    0.66821f, 0.66935f, 0.67048f, 0.67162f, 0.67275f, 0.67387f, 0.67499f,
    0.67611f, 0.67723f, 0.67834f, 0.67945f, 0.68055f, 0.68165f, 0.68275f,
    0.68385f, 0.68494f, 0.68603f, 0.68711f, 0.68819f, 0.68927f, 0.69035f,
    0.69142f, 0.69249f, 0.69355f, 0.69461f, 0.69567f, 0.69673f, 0.69778f,
    0.69883f, 0.69988f, 0.70092f, 0.70196f, 0.70300f, 0.70403f, 0.70506f,
    0.70609f, 0.70711f, 0.70813f, 0.70915f, 0.71017f, 0.71118f, 0.71219f,
    0.71319f, 0.71420f, 0.71520f, 0.71620f, 0.71719f, 0.71818f, 0.71917f,
    0.72016f, 0.72114f, 0.72212f, 0.72309f, 0.72407f, 0.72504f, 0.72601f,
    0.72697f, 0.72794f, 0.72890f, 0.72985f, 0.73081f, 0.73176f, 0.73271f,
    0.73365f, 0.73460f, 0.73554f, 0.73647f, 0.73741f, 0.73834f, 0.73927f,
    0.74020f, 0.74112f, 0.74204f, 0.74296f, 0.74388f, 0.74479f, 0.74570f,
    0.74661f, 0.74751f, 0.74842f, 0.74932f, 0.75021f, 0.75111f, 0.75200f,
    0.75289f, 0.75378f, 0.75466f, 0.75555f, 0.75643f, 0.75730f, 0.75818f,
    0.75905f, 0.75992f, 0.76079f, 0.76165f, 0.76251f, 0.76337f, 0.76423f,
    0.76508f, 0.76594f, 0.76679f, 0.76763f, 0.76848f, 0.76932f, 0.77016f,
    0.77100f, 0.77183f, 0.77267f, 0.77350f, 0.77432f, 0.77515f, 0.77597f,
    0.77680f, 0.77761f, 0.77843f, 0.77924f, 0.78006f, 0.78087f, 0.78167f,
    0.78248f, 0.78328f, 0.78408f, 0.78488f, 0.78568f, 0.78647f, 0.78726f,
    0.78805f, 0.78884f, 0.78962f, 0.79040f, 0.79118f, 0.79196f, 0.79274f,
    0.79351f, 0.79428f, 0.79505f, 0.79582f, 0.79658f, 0.79735f, 0.79811f,
    0.79887f, 0.79962f, 0.80038f, 0.80113f, 0.80188f, 0.80263f, 0.80337f,
    0.80412f, 0.80486f, 0.80560f, 0.80634f, 0.80707f, 0.80780f, 0.80854f,
    0.80926f, 0.80999f, 0.81072f, 0.81144f, 0.81216f, 0.81288f, 0.81360f,
    0.81431f, 0.81503f, 0.81574f, 0.81645f, 0.81715f, 0.81786f, 0.81856f,
    0.81926f, 0.81996f, 0.82066f, 0.82135f, 0.82205f, 0.82274f, 0.82343f,
    0.82412f, 0.82480f, 0.82549f, 0.82617f, 0.82685f, 0.82753f, 0.82820f,
    0.82888f, 0.82955f, 0.83022f, 0.83089f, 0.83155f, 0.83222f, 0.83288f,
    0.83354f, 0.83420f, 0.83486f, 0.83552f, 0.83617f, 0.83682f, 0.83747f,
    0.83812f, 0.83877f, 0.83941f, 0.84005f, 0.84069f, 0.84133f, 0.84197f,
    0.84261f, 0.84324f, 0.84387f, 0.84450f, 0.84513f, 0.84576f, 0.84639f,
    0.84701f, 0.84763f, 0.84825f, 0.84887f, 0.84949f, 0.85010f, 0.85071f,
    0.85132f, 0.85193f, 0.85254f, 0.85315f, 0.85375f, 0.85436f, 0.85496f,
    0.85556f, 0.85615f, 0.85675f, 0.85735f, 0.85794f, 0.85853f, 0.85912f,
    0.85971f, 0.86029f, 0.86088f, 0.86146f, 0.86204f, 0.86262f, 0.86320f,
    0.86378f, 0.86435f, 0.86493f, 0.86550f, 0.86607f, 0.86664f, 0.86720f,
    0.86777f, 0.86833f, 0.86889f, 0.86945f, 0.87001f, 0.87057f, 0.87113f,
    0.87168f, 0.87223f, 0.87278f, 0.87333f, 0.87388f, 0.87443f, 0.87497f,
    0.87552f, 0.87606f, 0.87660f, 0.87714f, 0.87768f, 0.87821f, 0.87875f,
    0.87928f, 0.87981f, 0.88034f, 0.88087f, 0.88140f, 0.88192f, 0.88244f,
    0.88297f, 0.88349f, 0.88401f, 0.88453f, 0.88504f, 0.88556f, 0.88607f,
    0.88658f, 0.88709f, 0.88760f, 0.88811f, 0.88862f, 0.88912f, 0.88963f,
    0.89013f, 0.89063f, 0.89113f, 0.89163f, 0.89212f, 0.89262f, 0.89311f,
    0.89360f, 0.89409f, 0.89458f, 0.89507f, 0.89556f, 0.89604f, 0.89653f,
    0.89701f, 0.89749f, 0.89797f, 0.89845f, 0.89892f, 0.89940f, 0.89987f,
    0.90035f, 0.90082f, 0.90129f, 0.90176f, 0.90222f, 0.90269f, 0.90316f,
    0.90362f, 0.90408f, 0.90454f, 0.90500f, 0.90546f, 0.90592f, 0.90637f,
    0.90683f, 0.90728f, 0.90773f, 0.90818f, 0.90863f, 0.90908f, 0.90952f,
    0.90997f, 0.91041f, 0.91085f, 0.91130f, 0.91173f, 0.91217f, 0.91261f,
    0.91305f, 0.91348f, 0.91392f, 0.91435f, 0.91478f, 0.91521f, 0.91564f,
    0.91606f, 0.91649f, 0.91691f, 0.91734f, 0.91776f, 0.91818f, 0.91860f,
    0.91902f, 0.91944f, 0.91985f, 0.92027f, 0.92068f, 0.92109f, 0.92150f,
    0.92191f, 0.92232f, 0.92273f, 0.92314f, 0.92354f, 0.92395f, 0.92435f,
    0.92475f, 0.92515f, 0.92555f, 0.92595f, 0.92634f, 0.92674f, 0.92713f,
    0.92753f, 0.92792f, 0.92831f, 0.92870f, 0.92909f, 0.92947f, 0.92986f,
    0.93025f, 0.93063f, 0.93101f, 0.93139f, 0.93177f, 0.93215f, 0.93253f,
    0.93291f, 0.93328f, 0.93366f, 0.93403f, 0.93440f, 0.93478f, 0.93515f,
    0.93551f, 0.93588f, 0.93625f, 0.93661f, 0.93698f, 0.93734f, 0.93770f,
    0.93807f, 0.93843f, 0.93878f, 0.93914f, 0.93950f, 0.93986f, 0.94021f,
    0.94056f, 0.94092f, 0.94127f, 0.94162f, 0.94197f, 0.94231f, 0.94266f,
    0.94301f, 0.94335f, 0.94369f, 0.94404f, 0.94438f, 0.94472f, 0.94506f,
    0.94540f, 0.94573f, 0.94607f, 0.94641f, 0.94674f, 0.94707f, 0.94740f,
    0.94774f, 0.94807f, 0.94839f, 0.94872f, 0.94905f, 0.94937f, 0.94970f,
    0.95002f, 0.95035f, 0.95067f, 0.95099f, 0.95131f, 0.95163f, 0.95194f,
    0.95226f, 0.95257f, 0.95289f, 0.95320f, 0.95351f, 0.95383f, 0.95414f,
    0.95445f, 0.95475f, 0.95506f, 0.95537f, 0.95567f, 0.95598f, 0.95628f,
    0.95658f, 0.95688f, 0.95718f, 0.95748f, 0.95778f, 0.95808f, 0.95838f,
    0.95867f, 0.95897f, 0.95926f, 0.95955f, 0.95984f, 0.96013f, 0.96042f,
    0.96071f, 0.96100f, 0.96129f, 0.96157f, 0.96186f, 0.96214f, 0.96242f,
    0.96271f, 0.96299f, 0.96327f, 0.96355f, 0.96382f, 0.96410f, 0.96438f,
    0.96465f, 0.96493f, 0.96520f, 0.96547f, 0.96574f, 0.96602f, 0.96629f,
    0.96655f, 0.96682f, 0.96709f, 0.96735f, 0.96762f, 0.96788f, 0.96815f,
    0.96841f, 0.96867f, 0.96893f, 0.96919f, 0.96945f, 0.96971f, 0.96996f,
    0.97022f, 0.97047f, 0.97073f, 0.97098f, 0.97123f, 0.97149f, 0.97174f,
    0.97199f, 0.97223f, 0.97248f, 0.97273f, 0.97297f, 0.97322f, 0.97346f,
    0.97371f, 0.97395f, 0.97419f, 0.97443f, 0.97467f, 0.97491f, 0.97515f,
    0.97539f, 0.97562f, 0.97586f, 0.97609f, 0.97633f, 0.97656f, 0.97679f,
    0.97702f, 0.97725f, 0.97748f, 0.97771f, 0.97794f, 0.97817f, 0.97839f,
    0.97862f, 0.97884f, 0.97907f, 0.97929f, 0.97951f, 0.97973f, 0.97995f,
    0.98017f, 0.98039f, 0.98061f, 0.98082f, 0.98104f, 0.98125f, 0.98147f,
    0.98168f, 0.98189f, 0.98211f, 0.98232f, 0.98253f, 0.98274f, 0.98295f,
    0.98315f, 0.98336f, 0.98357f, 0.98377f, 0.98398f, 0.98418f, 0.98438f,
    0.98458f, 0.98478f, 0.98498f, 0.98518f, 0.98538f, 0.98558f, 0.98578f,
    0.98597f, 0.98617f, 0.98636f, 0.98656f, 0.98675f, 0.98694f, 0.98714f,
    0.98733f, 0.98752f, 0.98771f, 0.98789f, 0.98808f, 0.98827f, 0.98845f,
    0.98864f, 0.98882f, 0.98901f, 0.98919f, 0.98937f, 0.98955f, 0.98973f,
    0.98991f, 0.99009f, 0.99027f, 0.99045f, 0.99063f, 0.99080f, 0.99098f,
    0.99115f, 0.99133f, 0.99150f, 0.99167f, 0.99184f, 0.99201f, 0.99218f,
    0.99235f, 0.99252f, 0.99269f, 0.99285f, 0.99302f, 0.99319f, 0.99335f,
    0.99351f, 0.99368f, 0.99384f, 0.99400f, 0.99416f, 0.99432f, 0.99448f,
    0.99464f, 0.99480f, 0.99495f, 0.99511f, 0.99527f, 0.99542f, 0.99558f,
    0.99573f, 0.99588f, 0.99603f, 0.99619f, 0.99634f, 0.99649f, 0.99664f,
    0.99678f, 0.99693f, 0.99708f, 0.99722f, 0.99737f, 0.99751f, 0.99766f,
    0.99780f, 0.99794f, 0.99809f, 0.99823f, 0.99837f, 0.99851f, 0.99865f,
    0.99879f, 0.99892f, 0.99906f, 0.99920f, 0.99933f, 0.99947f, 0.99960f,
    0.99974f, 0.99987f, 1.00000f};

// Evaluate ACR3 LUT via linear interpolation
static inline float evalAcrLUT(float x) {
  if (x <= 0.0f)
    return 0.0f;
  if (x >= 1.0f)
    return 1.0f;
  float y = x * 1024.0f; // 1025 entries, index [0..1024]
  int idx = (int)y;
  if (idx >= 1024)
    return kAcrDefaultLUT[1024];
  float frac = y - (float)idx;
  return kAcrDefaultLUT[idx] * (1.0f - frac) + kAcrDefaultLUT[idx + 1] * frac;
}

// removed applyToneCurveBuffer

} // anonymous namespace

// ============================================================================
// Main pipeline entry (AOT)
// ============================================================================
uint8_t *HalidePipeline::process(
    const uint16_t *bayerData, int width, int height, uint32_t blackLevel,
    uint32_t whiteLevel, const double asShotNeutral[3],
    const double camToSrgb[9], double baselineExposure,
    const DngMetadata &metadata, int &outWidth, int &outHeight) {

  outWidth = width;
  outHeight = height;

  auto t_total_start = std::chrono::steady_clock::now();

  const bool hasHSM =
      metadata.hsmHueDivisions > 0 && metadata.hsmSatDivisions > 1;
  const bool hasLT = metadata.ltHueDivisions > 0 && metadata.ltSatDivisions > 1;

  std::cerr << "[Halide AOT] Processing " << width << "x" << height
            << " BaselineExposure=" << baselineExposure << " EV"
            << " HueSatMap=" << (hasHSM ? "YES" : "NO")
            << " LookTable=" << (hasLT ? "YES" : "NO") << "\n";

  auto t_bind_start = std::chrono::steady_clock::now();

  float range = static_cast<float>(whiteLevel - blackLevel);
  float bl = static_cast<float>(blackLevel);
  Halide::Runtime::Buffer<uint16_t> bayerBuf(const_cast<uint16_t *>(bayerData), width, height);
  bayerBuf.set_host_dirty();
  float expGain = (float)std::pow(2.0, baselineExposure);

  Halide::Runtime::Buffer<float> hsmBuf(3, std::max((int)metadata.hsmSatDivisions, 1),
                                        std::max((int)metadata.hsmHueDivisions, 1),
                                        std::max((int)metadata.hsmValDivisions, 1));
  if (hasHSM) {
    for (int v = 0; v < metadata.hsmValDivisions; v++) {
      for (int h = 0; h < metadata.hsmHueDivisions; h++) {
        for (int s = 0; s < metadata.hsmSatDivisions; s++) {
          int idx = v * metadata.hsmHueDivisions * metadata.hsmSatDivisions +
                    h * metadata.hsmSatDivisions + s;
          hsmBuf(0, s, h, v) = metadata.hsmData[idx].hueShift;
          hsmBuf(1, s, h, v) = metadata.hsmData[idx].satScale;
          hsmBuf(2, s, h, v) = metadata.hsmData[idx].valScale;
        }
      }
    }
  }
  hsmBuf.set_host_dirty();

  Halide::Runtime::Buffer<float> ltBuf(3, std::max((int)metadata.ltSatDivisions, 1),
                                       std::max((int)metadata.ltHueDivisions, 1),
                                       std::max((int)metadata.ltValDivisions, 1));
  if (hasLT) {
    for (int v = 0; v < metadata.ltValDivisions; v++) {
      for (int h = 0; h < metadata.ltHueDivisions; h++) {
        for (int s = 0; s < metadata.ltSatDivisions; s++) {
          int idx = v * metadata.ltHueDivisions * metadata.ltSatDivisions +
                    h * metadata.ltSatDivisions + s;
          ltBuf(0, s, h, v) = metadata.ltData[idx].hueShift;
          ltBuf(1, s, h, v) = metadata.ltData[idx].satScale;
          ltBuf(2, s, h, v) = metadata.ltData[idx].valScale;
        }
      }
    }
  }
  ltBuf.set_host_dirty();

  const double *tcPts =
      (metadata.toneCurveCount > 0) ? metadata.toneCurvePoints : nullptr;
  const int tcCount =
      (metadata.toneCurveCount > 0) ? (int)metadata.toneCurveCount : 0;
  bool hasTC = true;

  Halide::Runtime::Buffer<float> tcBuf(4096);
  for (int i = 0; i < 4096; i++) {
    float x = (float)i / 4095.0f;
    if (tcCount > 0 && tcPts) {
      tcBuf(i) =
          std::min(std::max(evalToneCurve(tcPts, tcCount, x), 0.0f), 1.0f);
    } else {
      tcBuf(i) = evalAcrLUT(x);
    }
  }
  tcBuf.set_host_dirty();

  // Phase 5.1/5.4: Create HSL LUT buffer (dim0: 3 for H/S/L, dim1: 360 for hues)
  Halide::Runtime::Buffer<float> hslBuf(3, 360);

  // Base hue angles for the 8 Lightroom HSL channels
  const float baseHues[8] = {0.0f, 30.0f, 60.0f, 120.0f, 180.0f, 240.0f, 270.0f, 300.0f};

  // Fill the 360-degree LUT using linear interpolation between the 8 points
  for (int h = 0; h < 360; ++h) {
    float h_deg = static_cast<float>(h);
    // Find surrounding hue anchor points
    int i0 = 7, i1 = 0; // Default to wrap-around (Magenta to Red)
    for (int i = 0; i < 7; ++i) {
      if (h_deg >= baseHues[i] && h_deg < baseHues[i + 1]) {
        i0 = i;
        i1 = i + 1;
        break;
      }
    }

    float h0 = baseHues[i0];
    float h1 = baseHues[i1];
    float t = 0.0f;

    if (i0 == 7 && i1 == 0) {
      // Wrap-around case
      if (h_deg >= baseHues[7]) {
        t = (h_deg - baseHues[7]) / (360.0f - baseHues[7]);
      } else {
        t = (h_deg + 360.0f - baseHues[7]) / (360.0f - baseHues[7]);
      }
    } else {
      t = (h_deg - h0) / (h1 - h0);
    }

    // Interpolate values
    // hslHue is direct delta in degrees [-100, 100] maps to [-100, 100] hue shift
    float hueShift = metadata.lrParams.hslHue[i0] + t * (metadata.lrParams.hslHue[i1] - metadata.lrParams.hslHue[i0]);

    // Saturation and Luminance are scales [-100, 100] maps to [0, 2] where 0 is -100, 1 is 0, 2 is +100
    auto mapScale = [](double val) { return static_cast<float>((val + 100.0) / 100.0); };
    float sat0 = mapScale(metadata.lrParams.hslSat[i0]);
    float sat1 = mapScale(metadata.lrParams.hslSat[i1]);
    float satScale = sat0 + t * (sat1 - sat0);

    float lum0 = mapScale(metadata.lrParams.hslLum[i0]);
    float lum1 = mapScale(metadata.lrParams.hslLum[i1]);
    float lumScale = lum0 + t * (lum1 - lum0);

    hslBuf(0, h) = hueShift;
    hslBuf(1, h) = satScale;
    hslBuf(2, h) = lumScale;
  }
  hslBuf.set_host_dirty();

  const bool hasLR = metadata.lrParams.parsed;
  bool hasHSL = false;
  if (hasLR) {
    for (int i = 0; i < 8; ++i) {
      if (metadata.lrParams.hslHue[i] != 0.0 ||
          metadata.lrParams.hslSat[i] != 0.0 ||
          metadata.lrParams.hslLum[i] != 0.0) {
        hasHSL = true;
        break;
      }
    }
  }
  const float lrExpGainVal =
      hasLR ? static_cast<float>(std::pow(2.0, metadata.lrParams.exposure2012))
            : 1.0f;
  const float lrContrastVal =
      hasLR ? static_cast<float>(metadata.lrParams.contrast2012 / 100.0) : 0.0f;
  const float lrSatVal =
      hasLR ? static_cast<float>(metadata.lrParams.saturation / 100.0) : 0.0f;
  const float lrVibVal =
      hasLR ? static_cast<float>(metadata.lrParams.vibrance / 100.0) : 0.0f;

  auto t_bind_end = std::chrono::steady_clock::now();
  double bindMs =
      std::chrono::duration<double, std::milli>(t_bind_end - t_bind_start)
          .count();
  std::cerr << "[Halide Perf 7.1] bind_params (buffers): " << bindMs << " ms\n";

  auto t_alloc_start = std::chrono::steady_clock::now();

  uint8_t *out = new (std::nothrow) uint8_t[(size_t)width * height * 4];
  if (!out)
    return nullptr;

  for (int i = 0; i < width * height; i++) {
    out[i * 4 + 3] = 255;
  }

  auto t_alloc_end = std::chrono::steady_clock::now();
  double allocMs =
      std::chrono::duration<double, std::milli>(t_alloc_end - t_alloc_start)
          .count();
  std::cerr << "[Halide Perf 7.1] alloc+alpha_fill: " << allocMs << " ms\n";

  auto t_buf_start = std::chrono::steady_clock::now();

  Halide::Runtime::Buffer<uint8_t> halide_out =
      Halide::Runtime::Buffer<uint8_t>::make_interleaved(out, width, height, 3);
  halide_out.raw_buffer()->dim[0].stride = 4;
  halide_out.raw_buffer()->dim[1].stride = width * 4;
  halide_out.raw_buffer()->dim[2].stride = 1;

  auto t_buf_end = std::chrono::steady_clock::now();
  double bufMs =
      std::chrono::duration<double, std::milli>(t_buf_end - t_buf_start)
          .count();
  std::cerr << "[Halide Perf 7.1] buffer_setup: " << bufMs << " ms\n";

  auto t_realize_start = std::chrono::steady_clock::now();

  int result = dng_pipeline(
      bayerBuf.raw_buffer(),
      bl, range,
      (float)metadata.cameraToProPhoto[0], (float)metadata.cameraToProPhoto[1], (float)metadata.cameraToProPhoto[2],
      (float)metadata.cameraToProPhoto[3], (float)metadata.cameraToProPhoto[4], (float)metadata.cameraToProPhoto[5],
      (float)metadata.cameraToProPhoto[6], (float)metadata.cameraToProPhoto[7], (float)metadata.cameraToProPhoto[8],
      (float)metadata.proPhotoToSrgb[0], (float)metadata.proPhotoToSrgb[1], (float)metadata.proPhotoToSrgb[2],
      (float)metadata.proPhotoToSrgb[3], (float)metadata.proPhotoToSrgb[4], (float)metadata.proPhotoToSrgb[5],
      (float)metadata.proPhotoToSrgb[6], (float)metadata.proPhotoToSrgb[7], (float)metadata.proPhotoToSrgb[8],
      expGain,
      hasHSM, hasLT, hasTC, hasLR,
      hsmBuf.raw_buffer(), ltBuf.raw_buffer(), tcBuf.raw_buffer(),
      std::max((int)metadata.hsmHueDivisions, 1),
      std::max((int)metadata.hsmSatDivisions, 1),
      std::max((int)metadata.hsmValDivisions, 1),
      std::max((int)metadata.ltHueDivisions, 1),
      std::max((int)metadata.ltSatDivisions, 1),
      std::max((int)metadata.ltValDivisions, 1),
      lrExpGainVal, 1.0f + lrContrastVal, lrSatVal, lrVibVal,
      hasHSL, hslBuf.raw_buffer(),
      halide_out.raw_buffer()
  );

  if (result != 0) {
      std::cerr << "[Halide AOT] ERROR in dng_pipeline: " << result << "\n";
      delete[] out;
      return nullptr;
  }

  auto t_copy_start = std::chrono::steady_clock::now();
  halide_out.copy_to_host();
  auto t_copy_end = std::chrono::steady_clock::now();
  double copyMs =
      std::chrono::duration<double, std::milli>(t_copy_end - t_copy_start)
          .count();
  std::cerr << "[Halide Perf 7.1] copy_to_host (GPU→CPU): " << copyMs << " ms\n";

  auto t_realize_end = std::chrono::steady_clock::now();
  double realizeMs =
      std::chrono::duration<double, std::milli>(t_realize_end - t_realize_start)
          .count();
  std::cerr << "[Halide Perf 7.1] AOT execution (incl copy_to_host): " << realizeMs
            << " ms\n";

  auto t_total_end = std::chrono::steady_clock::now();
  double totalMs =
      std::chrono::duration<double, std::milli>(t_total_end - t_total_start)
          .count();
  std::cerr << "[Halide Perf 7.1] === TOTAL process(): " << totalMs << " ms ===\n";

  return out;
}

// ============================================================================
// HalidePipeline::processYCbCr — Phase 10: Lossy DNG YCbCr -> RGB
//
// YCbCr (YUV420) -> RGB conversion using BT.601 matrix, then apply
// the same color processing pipeline as Bayer input.
// ============================================================================
uint8_t *HalidePipeline::processYCbCr(const uint8_t *yPlane, const uint8_t *uPlane,
                                       const uint8_t *vPlane, int width, int height,
                                       const double camToSrgb[9], double baselineExposure,
                                       const DngMetadata &metadata, int &outWidth,
                                       int &outHeight) {
  std::cerr << "[YCbCr] Starting YCbCr->RGB processing: " << width << "x" << height << "\n";
  auto t_start = std::chrono::steady_clock::now();

  outWidth = width;
  outHeight = height;

  // Phase 10.5.2.1: YCbCr matrix correction
  // DNG SDK uses cameraToRGB matrix for ALL 3-channel data including YCbCr.
  // cameraToRGB = ProPhoto::MatrixFromPCS() * CameraToPCS()
  // This is NOT standard BT.601 - it's the camera profile matrix applied to YCbCr.
  // We must replicate this "wrong" behavior to match DNG SDK output exactly.

  auto t_ycbcr_start = std::chrono::steady_clock::now();
  uint8_t *out = new (std::nothrow) uint8_t[(size_t)width * height * 4];
  if (!out) {
    return nullptr;
  }

  // Extract cameraToRGB matrix (passed as camToSrgb but actually cameraToRGB)
  // Matrix layout: [r0 r1 r2; g0 g1 g2; b0 b1 b2] stored row-major
  double camToRgb[9];
  for (int i = 0; i < 9; i++) {
    camToRgb[i] = camToSrgb[i];
  }

  int uvWidth = (width + 1) / 2;
  int uvHeight = (height + 1) / 2;

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int uvX = std::min(x / 2, uvWidth - 1);
      int uvY = std::min(y / 2, uvHeight - 1);
      int uvIdx = uvY * uvWidth + uvX;

      // YCbCr planes (8-bit, standard JPEG encoding)
      float Y  = static_cast<float>(yPlane[y * width + x]);
      // YCbCr Cb/Cr are centered at 128 - need to subtract to get signed values
      float Cb = static_cast<float>(uPlane[uvIdx]) - 128.0f;
      float Cr = static_cast<float>(vPlane[uvIdx]) - 128.0f;

      // Apply cameraToRGB matrix: [r0 r1 r2; g0 g1 g2; b0 b1 b2] * [Y; Cb; Cr]
      // DNG SDK treats YCbCr as camera RGB input and applies the same matrix
      // Note: We still need the -128 offset because YCbCr encoding uses 128 as neutral
      float r = camToRgb[0] * Y + camToRgb[1] * Cb + camToRgb[2] * Cr;
      float g = camToRgb[3] * Y + camToRgb[4] * Cb + camToRgb[5] * Cr;
      float b = camToRgb[6] * Y + camToRgb[7] * Cb + camToRgb[8] * Cr;

      int idx = (y * width + x) * 4;
      out[idx + 0] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, r)));
      out[idx + 1] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, g)));
      out[idx + 2] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, b)));
      out[idx + 3] = 255;
    }
  }

  auto t_ycbcr_end = std::chrono::steady_clock::now();
  double ycbcrMs = std::chrono::duration<double, std::milli>(t_ycbcr_end - t_ycbcr_start).count();
  std::cerr << "[YCbCr] cameraToRGB conversion: " << ycbcrMs << " ms\n";

  auto t_total_end = std::chrono::steady_clock::now();
  double totalMs = std::chrono::duration<double, std::milli>(t_total_end - t_start).count();
  std::cerr << "[YCbCr] === TOTAL processYCbCr(): " << totalMs << " ms ===\n";

  return out;
}
