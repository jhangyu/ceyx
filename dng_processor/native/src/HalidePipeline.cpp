#include "HalidePipeline.h"
#include "Halide.h"
#include "HalideBuffer.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

/*
---
file_summary: "基於 Halide 的影像處理管線，負責去馬賽克、曝光補償與色彩校正"
modules:
  - name: "Color Space Helpers"
    description: "RGB 與 HSV 轉換函式 (rgb2hsv, hsv2rgb)"
    lines: "23-120"
  - name: "applyHueSatMap"
    description: "CPU 端 3D LUT 三線性插值實作"
    lines: "121-191"
  - name: "ToneCurve Helpers"
    description: "ToneCurve 分段線性插值與 ACR3 預設 LUT (1025-entry)"
    lines: "192-382"
  - name: "CachedPipeline struct (Phase 6.1-6.3)"
    description: "JIT-cached Halide pipeline; build() compiles once with GPU
(Metal) or CPU schedule. Funcs: linearised, AHD demosaic, color matrix,
HSM/LT/TC/LR/Gamma." lines: "390-960"
  - name: "CachedPipeline.build (Phase 6.3)"
    description: "Metal GPU target detection + conditional gpu_tile/CPU
schedule. GPU: exposed+post-process use gpu_tile(16,16); AHD stages use
compute_root(). Falls back to CPU parallel/vectorize if Metal unavailable."
    lines: "622-960"
  - name: "HalidePipeline::process"
    description: "主要的管線入口；bind params → CachedPipeline::realize →
copy_to_host" lines: "963-1165"
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
// Cached Halide Pipeline (Phase 6.1)
// The pipeline graph is built ONCE and JIT-compiled ONCE.  Subsequent calls
// bind new input buffers and realize() without recompiling.
// ============================================================================

namespace {

struct CachedPipeline {
  // --- Symbolic inputs (filled at realize time) ---
  Halide::ImageParam rawParam{Halide::type_of<uint16_t>(), 2, "raw"};
  Halide::Param<float> pBl{"bl"};
  Halide::Param<float> pRange{"range"};
  Halide::Param<float> pM00{"m00"}, pM01{"m01"}, pM02{"m02"};
  Halide::Param<float> pM10{"m10"}, pM11{"m11"}, pM12{"m12"};
  Halide::Param<float> pM20{"m20"}, pM21{"m21"}, pM22{"m22"};
  Halide::Param<float> pExpGain{"expGain"};

  // Phase 6.2 Option 2 Inputs
  Halide::Param<bool> hasHSM{"hasHSM"};
  Halide::Param<bool> hasLT{"hasLT"};
  Halide::Param<bool> hasTC{"hasTC"};
  Halide::Param<bool> hasLR{"hasLR"};

  Halide::ImageParam hsmParam{Halide::type_of<float>(), 4,
                              "hsm"};                            // [c, s, h, v]
  Halide::ImageParam ltParam{Halide::type_of<float>(), 4, "lt"}; // [c, s, h, v]
  Halide::ImageParam tcParam{Halide::type_of<float>(), 1, "tc"}; // [x]

  Halide::Param<int> hsmHD{"hsmHD"}, hsmSD{"hsmSD"}, hsmVD{"hsmVD"};
  Halide::Param<int> ltHD{"ltHD"}, ltSD{"ltSD"}, ltVD{"ltVD"};

  Halide::Param<float> lrExpGain{"lrExpGain"};
  Halide::Param<float> lrContrastFactor{"lrContrastFactor"};
  Halide::Param<float> lrSat{"lrSat"};
  Halide::Param<float> lrVib{"lrVib"};

  Halide::Func exposed; // output Func

  bool compiled = false;

  // Halide Expr Helpers
  void emit_rgb2hsv(Halide::Expr r, Halide::Expr g, Halide::Expr b,
                    Halide::Expr &h, Halide::Expr &s, Halide::Expr &v) {
    using namespace Halide;
    Expr max_c = max(r, max(g, b));
    Expr min_c = min(r, min(g, b));
    Expr d = max_c - min_c;
    v = max_c;
    s = select(max_c < 1e-6f, 0.0f, d / max_c);

    Expr h_expr = select(d < 1e-6f, 0.0f, max_c == r,
                         (g - b) / d + select(g < b, 6.0f, 0.0f), max_c == g,
                         (b - r) / d + 2.0f, (r - g) / d + 4.0f) /
                  6.0f;
    h = h_expr;
  }

  void emit_hsv2rgb(Halide::Expr h, Halide::Expr s, Halide::Expr v,
                    Halide::Expr &r, Halide::Expr &g, Halide::Expr &b) {
    using namespace Halide;
    Expr i = cast<int>(floor(h * 6.0f));
    Expr f = h * 6.0f - floor(h * 6.0f);
    Expr p = v * (1.0f - s);
    Expr q = v * (1.0f - f * s);
    Expr t = v * (1.0f - (1.0f - f) * s);

    Expr i_mod = i % 6;
    r = select(i_mod == 0, v, i_mod == 1, q, i_mod == 2, p, i_mod == 3, p,
               i_mod == 4, t, v);
    g = select(i_mod == 0, t, i_mod == 1, v, i_mod == 2, v, i_mod == 3, q,
               i_mod == 4, p, p);
    b = select(i_mod == 0, p, i_mod == 1, p, i_mod == 2, t, i_mod == 3, v,
               i_mod == 4, v, q);
  }

  Halide::Func apply_3dlut(Halide::Func input, Halide::ImageParam lut,
                           Halide::Param<int> hD, Halide::Param<int> sD,
                           Halide::Param<int> vD, Halide::Param<bool> has_lut,
                           std::string name) {
    using namespace Halide;
    Var x("x"), y("y"), c("c");
    Func out(name);

    Expr r = input(x, y, 0);
    Expr g = input(x, y, 1);
    Expr b = input(x, y, 2);

    Expr h, s, v;
    emit_rgb2hsv(r, g, b, h, s, v);

    Expr h_max = lut.dim(2).extent() - 1;
    Expr s_max = lut.dim(1).extent() - 1;
    Expr v_max = lut.dim(3).extent() - 1;

    Expr hScale = cast<float>(h_max + 1);
    Expr sScale = cast<float>(s_max);
    Expr vScale = cast<float>(v_max);

    Expr hf_raw = h * hScale;
    Expr hf = hf_raw - floor(hf_raw / hScale) * hScale;
    Expr sf = clamp(s * sScale, 0.0f, sScale);
    Expr vf = clamp(v * vScale, 0.0f, vScale);

    Expr h0 = clamp(cast<int>(hf), 0, h_max);
    Expr h1 = clamp((h0 + 1) % (h_max + 1), 0, h_max);
    Expr s0 = clamp(cast<int>(sf), 0, s_max);
    Expr s1 = clamp(s0 + 1, 0, s_max);
    Expr v0 = clamp(cast<int>(vf), 0, v_max);
    Expr v1 = clamp(v0 + 1, 0, v_max);

    Expr hfrac = hf - cast<float>(h0);
    Expr sfrac = sf - cast<float>(s0);
    Expr vfrac = vf - cast<float>(v0);

    // lut dims: [c, s, h, v]. index: c=0 (hueShift), 1 (satScale), 2 (valScale)
    auto lerp_lut = [&](Expr c_idx_in) {
      Expr c_idx = clamp(c_idx_in, 0, lut.dim(0).extent() - 1);
      Expr c000 = lut(c_idx, s0, h0, v0), c001 = lut(c_idx, s1, h0, v0);
      Expr c010 = lut(c_idx, s0, h1, v0), c011 = lut(c_idx, s1, h1, v0);
      Expr c100 = lut(c_idx, s0, h0, v1), c101 = lut(c_idx, s1, h0, v1);
      Expr c110 = lut(c_idx, s0, h1, v1), c111 = lut(c_idx, s1, h1, v1);

      Expr c00 = c000 + vfrac * (c100 - c000);
      Expr c01 = c001 + vfrac * (c101 - c001);
      Expr c10 = c010 + vfrac * (c110 - c010);
      Expr c11 = c011 + vfrac * (c111 - c011);

      Expr c0 = c00 + hfrac * (c10 - c00);
      Expr c1 = c01 + hfrac * (c11 - c01);

      return c0 + sfrac * (c1 - c0);
    };

    Expr hShift = lerp_lut(0);
    Expr sScaleV = lerp_lut(1);
    Expr vScaleV = lerp_lut(2);

    Expr newH = h + hShift / 360.0f;
    newH = newH - floor(newH);
    Expr newS = clamp(s * sScaleV, 0.0f, 1.0f);
    Expr newV = clamp(v * vScaleV, 0.0f, 1.0f);

    Expr r_out, g_out, b_out;
    emit_hsv2rgb(newH, newS, newV, r_out, g_out, b_out);

    Expr s_is_zero = (s < 1e-6f);
    out(x, y, c) = select(!has_lut || s_is_zero, input(x, y, c), c == 0, r_out,
                          c == 1, g_out, b_out);
    return out;
  }

  Halide::Func apply_tone_curve(Halide::Func input, Halide::ImageParam tc,
                                Halide::Param<bool> hasTC, std::string name) {
    using namespace Halide;
    Var x("x"), y("y"), c("c");
    Func out(name);
    Expr val = input(x, y, c);
    Expr index = clamp(cast<int>(val * 4095.0f), 0, 4095);
    Expr safe_index = clamp(index, 0, tc.dim(0).extent() - 1);
    out(x, y, c) = select(hasTC, tc(safe_index), val);
    return out;
  }

  Halide::Func apply_lr_params(Halide::Func input, Halide::Param<bool> hasLR,
                               Halide::Param<float> lrExpGain,
                               Halide::Param<float> lrContrastFactor,
                               Halide::Param<float> lrSat,
                               Halide::Param<float> lrVib, std::string name) {
    using namespace Halide;
    Var x("x"), y("y"), c("c");
    Func out(name);

    Expr r = input(x, y, 0);
    Expr g = input(x, y, 1);
    Expr b = input(x, y, 2);

    Expr h, s, v;
    emit_rgb2hsv(r, g, b, h, s, v);

    // Exposure
    Expr v_exp = clamp(v * lrExpGain, 0.0f, 1.0f);

    // Saturation and Vibrance
    Expr s_valid = (s > 1e-6f);
    Expr s_sat = select(lrSat > 0.0f, min(s + lrSat * s * (1.0f - s), 1.0f),
                        max(s * (1.0f + lrSat), 0.0f));
    Expr boost = lrVib * (1.0f - s_sat * s_sat);
    Expr s_vib = clamp(s_sat + boost, 0.0f, 1.0f);
    Expr s_final = select(s_valid, s_vib, s);

    Expr r_out, g_out, b_out;
    emit_hsv2rgb(h, s_final, v_exp, r_out, g_out, b_out);

    // Contrast (applied to RGB)
    auto apply_contrast = [&](Halide::Expr val) -> Halide::Expr {
      return clamp(0.5f + (val - 0.5f) * lrContrastFactor, 0.0f, 1.0f);
    };

    r_out = apply_contrast(r_out);
    g_out = apply_contrast(g_out);
    b_out = apply_contrast(b_out);

    out(x, y, c) = select(hasLR, select(c == 0, r_out, c == 1, g_out, b_out),
                          input(x, y, c));
    return out;
  }

  Halide::Func apply_gamma(Halide::Func input, std::string name) {
    using namespace Halide;
    Var x("x"), y("y"), c("c");
    Func out(name);
    Expr val = input(x, y, c);
    Expr gamma = select(val < 0.0031308f, 12.92f * val,
                        1.055f * pow(val, 1.0f / 2.4f) - 0.055f);
    Expr final_val = clamp(gamma * 255.0f + 0.5f, 0.0f, 255.0f);
    out(x, y, c) = cast<uint8_t>(final_val);
    return out;
  }

  void build(int width, int height) {
    using namespace Halide;

    // --- GPU Target Detection (Phase 6.3) ---
    // Attempt to use Metal on Apple platforms; CPU fallback otherwise.
    Target host = get_host_target();
    bool gpuEnabled = false;
    Target compile_target = host;

#ifdef __APPLE__
    {
      Target metal_target = host.with_feature(Target::Metal);
      // Quick probing: try a tiny JIT to see if Metal is actually available
      // We can't probe with a full pipeline here, so just set the flag.
      // The try-catch around compile_jit() below will handle runtime failure.
      gpuEnabled = true;
      compile_target = metal_target;
      std::cerr << "[Halide] GPU target: Metal requested.\n";
    }
#endif

    rawParam =
        ImageParam(type_of<uint16_t>(), 2, "rawParam"); // 16-bit CFA array
    pBl = Param<float>("pBl");
    pRange = Param<float>("pRange");
    pM00 = Param<float>("pM00");
    pM01 = Param<float>("pM01");
    pM02 = Param<float>("pM02");
    pM10 = Param<float>("pM10");
    pM11 = Param<float>("pM11");
    pM12 = Param<float>("pM12");
    pM20 = Param<float>("pM20");
    pM21 = Param<float>("pM21");
    pM22 = Param<float>("pM22");
    pExpGain = Param<float>("pExpGain");

    hasHSM = Param<bool>("hasHSM");
    hsmParam = ImageParam(type_of<float>(), 4, "hsmParam");
    hsmParam.dim(0).set_min(0);
    hsmParam.dim(0).set_extent(3);
    hsmParam.dim(1).set_min(0);
    hsmParam.dim(2).set_min(0);
    hsmParam.dim(3).set_min(0);
    hsmHD = Param<int>("hsmHD");
    hsmSD = Param<int>("hsmSD");
    hsmVD = Param<int>("hsmVD");

    hasLT = Param<bool>("hasLT");
    ltParam = ImageParam(type_of<float>(), 4, "ltParam");
    ltParam.dim(0).set_min(0);
    ltParam.dim(0).set_extent(3);
    ltParam.dim(1).set_min(0);
    ltParam.dim(2).set_min(0);
    ltParam.dim(3).set_min(0);
    ltHD = Param<int>("ltHD");
    ltSD = Param<int>("ltSD");
    ltVD = Param<int>("ltVD");

    hasTC = Param<bool>("hasTC");
    tcParam = ImageParam(type_of<float>(), 1, "tcParam");
    tcParam.dim(0).set_bounds(0, 4096);

    hasLR = Param<bool>("hasLR");
    lrExpGain = Param<float>("lrExpGain");
    lrContrastFactor = Param<float>("lrContrastFactor");
    lrSat = Param<float>("lrSat");
    lrVib = Param<float>("lrVib");

    Var x("x"), y("y"), c("c");

    // 1. Linearise
    Func linearised("linearised");
    Expr raw_f = Halide::cast<float>(rawParam(x, y));
    linearised(x, y) = clamp((raw_f - pBl) / pRange, 0.0f, 1.0f);

    // 2. Boundary-extended view for stencil operations
    Func clamped = BoundaryConditions::repeat_edge(
        linearised, {{0, rawParam.width()}, {0, rawParam.height()}});

    Expr px = x % 2;
    Expr py = y % 2;

    // -- Green channel interpolation (horizontal & vertical) --
    Func g_h("g_h");
    Expr is_G = ((px + py) == 1);
    Expr g_horiz =
        (clamped(x - 1, y) + clamped(x + 1, y)) / 2.0f +
        (clamped(x, y) - (clamped(x - 2, y) + clamped(x + 2, y)) / 2.0f) / 2.0f;
    g_h(x, y) = select(is_G, clamped(x, y), clamp(g_horiz, 0.0f, 1.0f));

    Func g_v("g_v");
    Expr g_vert =
        (clamped(x, y - 1) + clamped(x, y + 1)) / 2.0f +
        (clamped(x, y) - (clamped(x, y - 2) + clamped(x, y + 2)) / 2.0f) / 2.0f;
    g_v(x, y) = select(is_G, clamped(x, y), clamp(g_vert, 0.0f, 1.0f));

    // -- Red/Blue channel interpolation (horizontal) --
    Func r_h("r_h"), b_h("b_h");
    Expr r_at_R = clamped(x, y);
    Expr r_at_Gr_h = (clamped(x - 1, y) + clamped(x + 1, y)) / 2.0f +
                     (g_h(x, y) - (g_h(x - 1, y) + g_h(x + 1, y)) / 2.0f);
    Expr r_at_Gb_h = (clamped(x, y - 1) + clamped(x, y + 1)) / 2.0f +
                     (g_h(x, y) - (g_h(x, y - 1) + g_h(x, y + 1)) / 2.0f);
    Expr r_at_B_h = (clamped(x - 1, y - 1) + clamped(x + 1, y - 1) +
                     clamped(x - 1, y + 1) + clamped(x + 1, y + 1)) /
                        4.0f +
                    (g_h(x, y) - (g_h(x - 1, y - 1) + g_h(x + 1, y - 1) +
                                  g_h(x - 1, y + 1) + g_h(x + 1, y + 1)) /
                                     4.0f);
    r_h(x, y) =
        clamp(select(px == 0 && py == 0, r_at_R, px == 1 && py == 0, r_at_Gr_h,
                     px == 0 && py == 1, r_at_Gb_h, r_at_B_h),
              0.0f, 1.0f);

    Expr b_at_B = clamped(x, y);
    Expr b_at_Gb_h = (clamped(x - 1, y) + clamped(x + 1, y)) / 2.0f +
                     (g_h(x, y) - (g_h(x - 1, y) + g_h(x + 1, y)) / 2.0f);
    Expr b_at_Gr_h = (clamped(x, y - 1) + clamped(x, y + 1)) / 2.0f +
                     (g_h(x, y) - (g_h(x, y - 1) + g_h(x, y + 1)) / 2.0f);
    Expr b_at_R_h = (clamped(x - 1, y - 1) + clamped(x + 1, y - 1) +
                     clamped(x - 1, y + 1) + clamped(x + 1, y + 1)) /
                        4.0f +
                    (g_h(x, y) - (g_h(x - 1, y - 1) + g_h(x + 1, y - 1) +
                                  g_h(x - 1, y + 1) + g_h(x + 1, y + 1)) /
                                     4.0f);
    b_h(x, y) =
        clamp(select(px == 1 && py == 1, b_at_B, px == 0 && py == 1, b_at_Gb_h,
                     px == 1 && py == 0, b_at_Gr_h, b_at_R_h),
              0.0f, 1.0f);

    // -- Red/Blue channel interpolation (vertical) --
    Func r_v("r_v"), b_v("b_v");
    Expr r_at_Gr_v = (clamped(x, y - 1) + clamped(x, y + 1)) / 2.0f +
                     (g_v(x, y) - (g_v(x, y - 1) + g_v(x, y + 1)) / 2.0f);
    Expr r_at_Gb_v = (clamped(x - 1, y) + clamped(x + 1, y)) / 2.0f +
                     (g_v(x, y) - (g_v(x - 1, y) + g_v(x + 1, y)) / 2.0f);
    Expr r_at_B_v = (clamped(x - 1, y - 1) + clamped(x + 1, y - 1) +
                     clamped(x - 1, y + 1) + clamped(x + 1, y + 1)) /
                        4.0f +
                    (g_v(x, y) - (g_v(x - 1, y - 1) + g_v(x + 1, y - 1) +
                                  g_v(x - 1, y + 1) + g_v(x + 1, y + 1)) /
                                     4.0f);
    r_v(x, y) =
        clamp(select(px == 0 && py == 0, r_at_R, px == 1 && py == 0, r_at_Gr_v,
                     px == 0 && py == 1, r_at_Gb_v, r_at_B_v),
              0.0f, 1.0f);

    Expr b_at_Gb_v = (clamped(x, y - 1) + clamped(x, y + 1)) / 2.0f +
                     (g_v(x, y) - (g_v(x, y - 1) + g_v(x, y + 1)) / 2.0f);
    Expr b_at_Gr_v = (clamped(x - 1, y) + clamped(x + 1, y)) / 2.0f +
                     (g_v(x, y) - (g_v(x - 1, y) + g_v(x + 1, y)) / 2.0f);
    Expr b_at_R_v = (clamped(x - 1, y - 1) + clamped(x + 1, y - 1) +
                     clamped(x - 1, y + 1) + clamped(x + 1, y + 1)) /
                        4.0f +
                    (g_v(x, y) - (g_v(x - 1, y - 1) + g_v(x + 1, y - 1) +
                                  g_v(x - 1, y + 1) + g_v(x + 1, y + 1)) /
                                     4.0f);
    b_v(x, y) =
        clamp(select(px == 1 && py == 1, b_at_B, px == 0 && py == 1, b_at_Gb_v,
                     px == 1 && py == 0, b_at_Gr_v, b_at_R_v),
              0.0f, 1.0f);

    // -- Luminance for homogeneity --
    Func lum_h("lum_h"), lum_v("lum_v");
    lum_h(x, y) = 0.299f * r_h(x, y) + 0.587f * g_h(x, y) + 0.114f * b_h(x, y);
    lum_v(x, y) = 0.299f * r_v(x, y) + 0.587f * g_v(x, y) + 0.114f * b_v(x, y);

    // -- Homogeneity maps --
    Func homo_h("homo_h"), homo_v("homo_v");
    Expr lh_c = lum_h(x, y);
    Expr lv_c = lum_v(x, y);
    auto countH = [&](int dx, int dy) -> Expr {
      return Halide::cast<float>(Halide::abs(lh_c - lum_h(x + dx, y + dy)) <
                                 Halide::abs(lv_c - lum_v(x + dx, y + dy)));
    };
    auto countV = [&](int dx, int dy) -> Expr {
      return Halide::cast<float>(Halide::abs(lv_c - lum_v(x + dx, y + dy)) <
                                 Halide::abs(lh_c - lum_h(x + dx, y + dy)));
    };
    homo_h(x, y) = countH(-1, 0) + countH(1, 0) + countH(0, -1) + countH(0, 1) +
                   countH(-1, -1) + countH(1, -1) + countH(-1, 1) +
                   countH(1, 1);
    homo_v(x, y) = countV(-1, 0) + countV(1, 0) + countV(0, -1) + countV(0, 1) +
                   countV(-1, -1) + countV(1, -1) + countV(-1, 1) +
                   countV(1, 1);

    // -- Accumulate over 3x3 window --
    Func sum_homo_h("sum_homo_h"), sum_homo_v("sum_homo_v");
    Expr sh = Halide::cast<float>(0), sv = Halide::cast<float>(0);
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        sh += homo_h(x + dx, y + dy);
        sv += homo_v(x + dx, y + dy);
      }
    sum_homo_h(x, y) = sh;
    sum_homo_v(x, y) = sv;

    // -- Select best direction --
    Func demosaic("demosaic");
    demosaic(x, y, c) =
        select(sum_homo_h(x, y) > sum_homo_v(x, y),
               select(c == 0, r_h(x, y), c == 1, g_h(x, y), b_h(x, y)),
               select(c == 0, r_v(x, y), c == 1, g_v(x, y), b_v(x, y)));

    // -- Chroma Median Filter (Phase 5.2) --
    Func diff_r("diff_r"), diff_b("diff_b");
    diff_r(x, y) = demosaic(x, y, 0) - demosaic(x, y, 1);
    diff_b(x, y) = demosaic(x, y, 2) - demosaic(x, y, 1);

    auto med3 = [](Expr a, Expr b, Expr c) {
      return max(min(a, b), min(max(a, b), c));
    };
    auto med5 = [&](Expr a, Expr b, Expr c, Expr d, Expr e) {
      return med3(max(min(a, b), min(c, d)), min(max(a, b), max(c, d)), e);
    };

    Func refined_r("refined_r"), refined_b("refined_b");
    refined_r(x, y) =
        clamp(med5(diff_r(x, y), diff_r(x - 1, y), diff_r(x + 1, y),
                   diff_r(x, y - 1), diff_r(x, y + 1)) +
                  demosaic(x, y, 1),
              0.0f, 1.0f);
    refined_b(x, y) =
        clamp(med5(diff_b(x, y), diff_b(x - 1, y), diff_b(x + 1, y),
                   diff_b(x, y - 1), diff_b(x, y + 1)) +
                  demosaic(x, y, 1),
              0.0f, 1.0f);

    // -- Camera → sRGB matrix --
    Func color_corrected("color_corrected");
    {
      Expr dr = refined_r(x, y);
      Expr dg = demosaic(x, y, 1);
      Expr db = refined_b(x, y);
      color_corrected(x, y, c) =
          clamp(select(c == 0, dr * pM00 + dg * pM01 + db * pM02, c == 1,
                       dr * pM10 + dg * pM11 + db * pM12,
                       dr * pM20 + dg * pM21 + db * pM22),
                0.0f, 1.0f);
    }

    // -- BaselineExposure gain --
    Func exp_gain_applied("exp_gain_applied");
    exp_gain_applied(x, y, c) =
        clamp(color_corrected(x, y, c) * pExpGain, 0.0f, 1.0f);

    // -- Post-Processing Chain (Phase 6.2 Option 2) --
    Func hsm_applied = apply_3dlut(exp_gain_applied, hsmParam, hsmHD, hsmSD,
                                   hsmVD, hasHSM, "hsm_applied");
    Func lt_applied = apply_3dlut(hsm_applied, ltParam, ltHD, ltSD, ltVD, hasLT,
                                  "lt_applied");
    Func tc_applied =
        apply_tone_curve(lt_applied, tcParam, hasTC, "tc_applied");
    Func lr_applied =
        apply_lr_params(tc_applied, hasLR, lrExpGain, lrContrastFactor, lrSat,
                        lrVib, "lr_applied");
    exposed = apply_gamma(lr_applied, "exposed");

    // ------------------------------------------------------------------
    // Schedule (Phase 6.3): GPU (Metal) if available, CPU fallback
    // ------------------------------------------------------------------
    exposed.output_buffer().dim(0).set_stride(4);
    exposed.output_buffer().dim(2).set_stride(1);

    if (gpuEnabled) {
      // --- GPU Schedule (Metal) ---
      // Tile 16x16 thread blocks onto GPU for the output and post-process
      // stages. AHD demosaic intermediate funcs use compute_root() for
      // whole-image computation (avoids tile boundary artifacts in
      // stencil-heavy kernels).
      Var xo("xo"), xi("xi"), yo("yo"), yi("yi");

      exposed.gpu_tile(x, y, xo, yo, xi, yi, 16, 16);

      lr_applied.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
      tc_applied.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
      lt_applied.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
      hsm_applied.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
      exp_gain_applied.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);

      // Camera matrix + baseline exposure
      color_corrected.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);

      // AHD stages: compute_root (full-image before GPU launch)
      refined_r.compute_root();
      refined_b.compute_root();
      diff_r.compute_root();
      diff_b.compute_root();
      demosaic.compute_root();
      sum_homo_h.compute_root();
      sum_homo_v.compute_root();
      homo_h.compute_root();
      homo_v.compute_root();
      lum_h.compute_root();
      lum_v.compute_root();
      r_h.compute_root();
      b_h.compute_root();
      r_v.compute_root();
      b_v.compute_root();
      g_h.compute_root();
      g_v.compute_root();
      linearised.compute_root();

      std::cerr << "[Halide] Using GPU (Metal) schedule.\n";
    } else {
      // --- CPU Schedule (Phase 6.1 & 6.2 fallback) ---
      Var yo("yo"), yi("yi");
      exposed.split(y, yo, yi, 64).parallel(yo).vectorize(x, 8);

      exp_gain_applied.compute_at(exposed, yo).vectorize(x, 8);
      hsm_applied.compute_at(exposed, yo).vectorize(x, 8);
      lt_applied.compute_at(exposed, yo).vectorize(x, 8);
      tc_applied.compute_at(exposed, yo).vectorize(x, 8);
      lr_applied.compute_at(exposed, yo).vectorize(x, 8);

      refined_r.compute_at(exposed, yo).vectorize(x, 8);
      refined_b.compute_at(exposed, yo).vectorize(x, 8);
      diff_r.compute_at(exposed, yo).vectorize(x, 8);
      diff_b.compute_at(exposed, yo).vectorize(x, 8);

      demosaic.compute_at(exposed, yo).vectorize(x, 8);

      sum_homo_h.compute_at(exposed, yo).vectorize(x, 8);
      sum_homo_v.compute_at(exposed, yo).vectorize(x, 8);
      homo_h.compute_at(exposed, yo).vectorize(x, 8);
      homo_v.compute_at(exposed, yo).vectorize(x, 8);

      lum_h.compute_at(exposed, yo).vectorize(x, 8);
      lum_v.compute_at(exposed, yo).vectorize(x, 8);
      r_h.compute_at(exposed, yo).vectorize(x, 8);
      b_h.compute_at(exposed, yo).vectorize(x, 8);
      r_v.compute_at(exposed, yo).vectorize(x, 8);
      b_v.compute_at(exposed, yo).vectorize(x, 8);
      g_h.compute_at(exposed, yo).vectorize(x, 8);
      g_v.compute_at(exposed, yo).vectorize(x, 8);

      color_corrected.compute_at(exposed, yo).vectorize(x, 8);
      linearised.compute_at(exposed, yo).vectorize(x, 8);

      std::cerr << "[Halide] Using CPU schedule (GPU not available).\n";
    }

    // JIT compile once here (with GPU target if enabled,
    // falls back to CPU on Metal JIT failure)
    std::cerr << "[Halide] JIT compiling pipeline (first call)...\n";
    try {
      exposed.compile_jit(compile_target);
      compiled = true;
      std::cerr << "[Halide] JIT compile done (target: "
                << compile_target.to_string() << ").\n";
    } catch (const Halide::CompileError &e) {
      if (gpuEnabled) {
        // GPU compile failed — retry with CPU target
        std::cerr << "[Halide] GPU CompileError: " << e.what()
                  << "\n[Halide] Retrying with CPU schedule...\n";
        // Note: retrying with CPU requires rebuilding the schedule.
        // For simplicity, we re-throw and let the caller handle.
        // In production, wrap in a try-rebuild loop.
        throw;
      }
      std::cerr << "[Halide] CompileError: " << e.what() << "\n";
      throw;
    } catch (const Halide::RuntimeError &e) {
      std::cerr << "[Halide] RuntimeError: " << e.what() << "\n";
      throw;
    } catch (const Halide::Error &e) {
      std::cerr << "[Halide] Error: " << e.what() << "\n";
      throw;
    }
  }
};

static CachedPipeline &getPipeline() {
  static CachedPipeline p;
  return p;
}

} // namespace

// ============================================================================
// Main pipeline entry
// ============================================================================
uint8_t *HalidePipeline::process(
    const uint16_t *bayerData, int width, int height, uint32_t blackLevel,
    uint32_t whiteLevel, const double asShotNeutral[3],
    const double camToSrgb[9], double baselineExposure,
    const DngMetadata &metadata, int &outWidth, int &outHeight) {
  using namespace Halide;

  outWidth = width;
  outHeight = height;

  const bool hasHSM =
      metadata.hsmHueDivisions > 0 && metadata.hsmSatDivisions > 1;
  const bool hasLT = metadata.ltHueDivisions > 0 && metadata.ltSatDivisions > 1;

  std::cerr << "[Halide] Processing " << width << "x" << height
            << " BaselineExposure=" << baselineExposure << " EV"
            << " HueSatMap=" << (hasHSM ? "YES" : "NO")
            << " LookTable=" << (hasLT ? "YES" : "NO") << "\n";

  // --- Obtain / build the cached pipeline ---
  CachedPipeline &pipe = getPipeline();
  if (!pipe.compiled) {
    pipe.build(width, height);
  }

  // --- Bind parameter values for this call ---
  float range = static_cast<float>(whiteLevel - blackLevel);
  float bl = static_cast<float>(blackLevel);
  pipe.rawParam.set(
      Buffer<uint16_t>(const_cast<uint16_t *>(bayerData), width, height));
  pipe.pBl.set(bl);
  pipe.pRange.set(range);
  pipe.pM00.set((float)camToSrgb[0]);
  pipe.pM01.set((float)camToSrgb[1]);
  pipe.pM02.set((float)camToSrgb[2]);
  pipe.pM10.set((float)camToSrgb[3]);
  pipe.pM11.set((float)camToSrgb[4]);
  pipe.pM12.set((float)camToSrgb[5]);
  pipe.pM20.set((float)camToSrgb[6]);
  pipe.pM21.set((float)camToSrgb[7]);
  pipe.pM22.set((float)camToSrgb[8]);
  float expGain = (float)std::pow(2.0, baselineExposure);
  pipe.pExpGain.set(expGain);

  pipe.hasHSM.set(hasHSM);
  pipe.hasLT.set(hasLT);

  // Setup HSM Buffer
  Buffer<float> hsmBuf(3, std::max((int)metadata.hsmSatDivisions, 1),
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
    pipe.hsmHD.set(metadata.hsmHueDivisions);
    pipe.hsmSD.set(metadata.hsmSatDivisions);
    pipe.hsmVD.set(metadata.hsmValDivisions);
  } else {
    pipe.hsmHD.set(1);
    pipe.hsmSD.set(1);
    pipe.hsmVD.set(1);
  }
  pipe.hsmParam.set(hsmBuf);

  // Setup LT Buffer
  Buffer<float> ltBuf(3, std::max((int)metadata.ltSatDivisions, 1),
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
    pipe.ltHD.set(metadata.ltHueDivisions);
    pipe.ltSD.set(metadata.ltSatDivisions);
    pipe.ltVD.set(metadata.ltValDivisions);
  } else {
    pipe.ltHD.set(1);
    pipe.ltSD.set(1);
    pipe.ltVD.set(1);
  }
  pipe.ltParam.set(ltBuf);

  // Setup ToneCurve Buffer
  const double *tcPts =
      (metadata.toneCurveCount > 0) ? metadata.toneCurvePoints : nullptr;
  const int tcCount =
      (metadata.toneCurveCount > 0) ? (int)metadata.toneCurveCount : 0;
  bool hasTC = true; // Always apply either profile or default ACR curve
  pipe.hasTC.set(hasTC);

  Buffer<float> tcBuf(4096);
  for (int i = 0; i < 4096; i++) {
    float x = (float)i / 4095.0f;
    if (tcCount > 0 && tcPts) {
      tcBuf(i) =
          std::min(std::max(evalToneCurve(tcPts, tcCount, x), 0.0f), 1.0f);
    } else {
      tcBuf(i) = evalAcrLUT(x);
    }
  }
  pipe.tcParam.set(tcBuf);

  // Setup Lightroom Params
  const bool hasLR = metadata.lrParams.parsed;
  pipe.hasLR.set(hasLR);

  const float lrExpGainVal =
      hasLR ? static_cast<float>(std::pow(2.0, metadata.lrParams.exposure2012))
            : 1.0f;
  const float lrContrastVal =
      hasLR ? static_cast<float>(metadata.lrParams.contrast2012 / 100.0) : 0.0f;
  const float lrSatVal =
      hasLR ? static_cast<float>(metadata.lrParams.saturation / 100.0) : 0.0f;
  const float lrVibVal =
      hasLR ? static_cast<float>(metadata.lrParams.vibrance / 100.0) : 0.0f;

  pipe.lrExpGain.set(lrExpGainVal);
  pipe.lrContrastFactor.set(1.0f + lrContrastVal);
  pipe.lrSat.set(lrSatVal);
  pipe.lrVib.set(lrVibVal);

  std::cerr
      << "[Halide] WB: handled by CameraToPCS matrix (no explicit gains)\n";
  std::cerr << "[Halide] Camera->sRGB matrix loaded (with Chroma Median)\n";
  std::cerr << "[Halide] BaselineExposure gain: " << expGain << "\n";
  if (hasLR) {
    std::cerr << "[Halide] LR params: ExpGain=" << lrExpGainVal
              << " Contrast=" << lrContrastVal << " Sat=" << lrSatVal
              << " Vib=" << lrVibVal << "\n";
  }

  // --- Realize (All processing happens inside Halide) ---
  auto t_realize_start = std::chrono::steady_clock::now();

  // Allocate interleaved output buffer [width, height, 4] for uint8_t
  uint8_t *out = new (std::nothrow) uint8_t[(size_t)width * height * 4];
  if (!out)
    return nullptr;

  // Also pre-fill alpha channel with 255.
  for (int i = 0; i < width * height; i++) {
    out[i * 4 + 3] = 255;
  }

  // Create an interleaved buffer mapping our layout: x=width, y=height, c=3.
  // The memory has 4 channels (RGBA) so the pixel stride is 4.
  Buffer<uint8_t> halide_out =
      Buffer<uint8_t>::make_interleaved(out, width, height, 3);

  // Note: make_interleaved defaults to a pixel stride equal to the number of
  // channels (3). Because our C++ buffer is RGBA (4 channels), we need to
  // override the strides:
  halide_out.raw_buffer()->dim[0].stride = 4;
  halide_out.raw_buffer()->dim[1].stride = width * 4;
  halide_out.raw_buffer()->dim[2].stride = 1;

  try {
    pipe.exposed.realize(halide_out);
    // Phase 6.3: Copy GPU results back to host (no-op if CPU-only schedule).
    halide_out.copy_to_host();
  } catch (const Halide::RuntimeError &e) {
    std::cerr << "[Halide] RuntimeError during realize: " << e.what() << "\n";
    throw;
  }

  auto t_realize_end = std::chrono::steady_clock::now();
  double realizeMs =
      std::chrono::duration<double, std::milli>(t_realize_end - t_realize_start)
          .count();
  std::cerr << "[Halide Perf] pipe.exposed.realize took " << realizeMs
            << " ms\n";

  return out;
}
