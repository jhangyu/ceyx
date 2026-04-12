/*
---
file_summary: >
  舊版端到端 Halide Generator。從 16-bit Bayer CFA 輸入一路完成線性化、
  AHD-like demosaic、ProPhoto/sRGB 色彩轉換、HueSat/Look/Tone/LR/HSL 調整，
  最後輸出 8-bit RGB；由 CMake AOT 產生 dng_pipeline。

notes:
  - `generate()` 定義完整數值鏈；`schedule()` 定義 CPU/GPU 排程。
  - GPU 路徑保留 AHD 中間節點 `compute_root()`，避免 bounds inference 組合爆炸。
  - 點對點色彩處理盡量 inline/fuse 到輸出，減少 VRAM/記憶體頻寬。

classes:
  - name: "DngPipeline"
    description: "Halide::Generator 主體；宣告所有輸入、輸出與中間 Func。"
    lines: "54-562"

functions:
  - name: "DngPipeline::emit_rgb2hsv"
    description: "RGB -> HSV helper，供 3D LUT / LR / HSL 調整共用。"
    lines: "103-115"
  - name: "DngPipeline::emit_hsv2rgb"
    description: "HSV -> RGB helper，將調整後色彩轉回 RGB。"
    lines: "117-131"
  - name: "DngPipeline::apply_3dlut"
    description: "套用 HueSatMap / LookTable 的 3D LUT 與 HSV 插值。"
    lines: "133-203"
  - name: "DngPipeline::apply_tone_curve"
    description: "套用 1D tone curve。"
    lines: "205-214"
  - name: "DngPipeline::apply_lr_params"
    description: "套用 Lightroom-style exposure / contrast / saturation / vibrance。"
    lines: "216-255"
  - name: "DngPipeline::apply_hsl_lut"
    description: "套用 CPU 預插值的 360-degree HSL LUT。"
    lines: "257-290"
  - name: "DngPipeline::apply_gamma"
    description: "sRGB gamma encode 並量化成 uint8。"
    lines: "292-300"
  - name: "DngPipeline::generate"
    description: "建立完整 Bayer -> RGB 管線與所有中間 Func。"
    lines: "303-498"
  - name: "DngPipeline::schedule"
    description: "設定 GPU tile / CPU vectorize / parallel 與 compute_at/root 策略。"
    lines: "500-561"
---
*/
#include "Halide.h"
#include <algorithm>
#include <cmath>

using namespace Halide;

class DngPipeline : public Halide::Generator<DngPipeline> {
public:
  Input<Buffer<uint16_t>> rawParam{"rawParam", 2}; // 16-bit CFA array
  Input<float> pBl{"pBl"};
  Input<float> pRange{"pRange"};
  
  // Phase 6.6: Camera to ProPhoto RGB Matrix
  Input<float> pC2P00{"pC2P00"}, pC2P01{"pC2P01"}, pC2P02{"pC2P02"};
  Input<float> pC2P10{"pC2P10"}, pC2P11{"pC2P11"}, pC2P12{"pC2P12"};
  Input<float> pC2P20{"pC2P20"}, pC2P21{"pC2P21"}, pC2P22{"pC2P22"};
  
  // Phase 6.6: ProPhoto RGB to sRGB Matrix
  Input<float> pP2S00{"pP2S00"}, pP2S01{"pP2S01"}, pP2S02{"pP2S02"};
  Input<float> pP2S10{"pP2S10"}, pP2S11{"pP2S11"}, pP2S12{"pP2S12"};
  Input<float> pP2S20{"pP2S20"}, pP2S21{"pP2S21"}, pP2S22{"pP2S22"};

  Input<float> pExpGain{"pExpGain"};

  // Phase 6.2 Option 2 Inputs
  Input<bool> hasHSM{"hasHSM"};
  Input<bool> hasLT{"hasLT"};
  Input<bool> hasTC{"hasTC"};
  Input<bool> hasLR{"hasLR"};

  Input<Buffer<float>> hsmParam{"hsmParam", 4}; // [c, s, h, v]
  Input<Buffer<float>> ltParam{"ltParam", 4};   // [c, s, h, v]
  Input<Buffer<float>> tcParam{"tcParam", 1};   // [x]

  Input<int> hsmHD{"hsmHD"}, hsmSD{"hsmSD"}, hsmVD{"hsmVD"};
  Input<int> ltHD{"ltHD"}, ltSD{"ltSD"}, ltVD{"ltVD"};

  Input<float> lrExpGain{"lrExpGain"};
  Input<float> lrContrastFactor{"lrContrastFactor"};
  Input<float> lrSat{"lrSat"};
  Input<float> lrVib{"lrVib"};
  // Phase 5.1/5.4: HSL 8-Channel LUT (pre-interpolated to 360 degrees on CPU)
  Input<bool> hasHSL{"hasHSL"};
  Input<Buffer<float>> hslLUT{"hslLUT", 2}; // dim0: channel (0=hue shift, 1=sat scale, 2=lum scale), dim1: 360 hue degrees

  Output<Buffer<uint8_t>> exposed{"exposed", 3};

  // Internal Funcs
  Func linearised, clamped;
  Func g_h, g_v, r_h, b_h, r_v, b_v;
  Func lum_h, lum_v, homo_h, homo_v, sum_homo_h, sum_homo_v;
  Func demosaic, diff_r, diff_b, refined_r, refined_b;
    Func prophoto_rgb, exp_gain_applied;
    Func hsm_applied, lt_applied, tc_applied, lr_applied, hsl_applied, final_srgb;

  void emit_rgb2hsv(Expr r, Expr g, Expr b, Expr &h, Expr &s, Expr &v) {
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

  void emit_hsv2rgb(Expr h, Expr s, Expr v, Expr &r, Expr &g, Expr &b) {
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

  Func apply_3dlut(Func input, Input<Buffer<float>> &lut, Expr hD, Expr sD,
                   Expr vD, Expr has_lut, std::string name) {
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

  Func apply_tone_curve(Func input, Input<Buffer<float>> &tc, Expr hasTC,
                        std::string name) {
    Var x("x"), y("y"), c("c");
    Func out(name);
    Expr val = input(x, y, c);
    Expr index = clamp(cast<int>(val * 4095.0f), 0, 4095);
    Expr safe_index = clamp(index, 0, tc.dim(0).extent() - 1);
    out(x, y, c) = select(hasTC, tc(safe_index), val);
    return out;
  }

  Func apply_lr_params(Func input, Expr hasLR, Expr lrExpGain,
                       Expr lrContrastFactor, Expr lrSat, Expr lrVib,
                       std::string name) {
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

    // Contrast
    auto apply_contrast = [&](Expr val) -> Expr {
      return clamp(0.5f + (val - 0.5f) * lrContrastFactor, 0.0f, 1.0f);
    };

    r_out = apply_contrast(r_out);
    g_out = apply_contrast(g_out);
    b_out = apply_contrast(b_out);

    out(x, y, c) = select(hasLR, select(c == 0, r_out, c == 1, g_out, b_out),
                          input(x, y, c));
    return out;
  }

  Func apply_hsl_lut(Func input, Input<Buffer<float>> &lut, Expr has_hsl,
                     std::string name) {
    Var x("x"), y("y"), c("c");
    Func out(name);

    Expr r = input(x, y, 0);
    Expr g = input(x, y, 1);
    Expr b = input(x, y, 2);

    Expr h, s, v;
    emit_rgb2hsv(r, g, b, h, s, v);

    Expr hue_deg = h * 360.0f;
    Expr h0 = clamp(cast<int>(hue_deg), 0, 359);
    Expr h1 = (h0 + 1) % 360;
    Expr frac = hue_deg - cast<float>(h0);

    Expr hue_shift = lerp(lut(0, h0), lut(0, h1), frac);
    Expr sat_scale = lerp(lut(1, h0), lut(1, h1), frac);
    Expr lum_scale = lerp(lut(2, h0), lut(2, h1), frac);

    Expr h_new = h + hue_shift / 360.0f;
    h_new = h_new - floor(h_new);
    Expr s_new = clamp(s * sat_scale, 0.0f, 1.0f);
    Expr v_new = clamp(v * lum_scale, 0.0f, 1.0f);

    Expr r_new, g_new, b_new;
    emit_hsv2rgb(h_new, s_new, v_new, r_new, g_new, b_new);

    Expr s_is_zero = (s < 1e-6f);
    out(x, y, c) = select(!has_hsl || s_is_zero, input(x, y, c),
                          c == 0, r_new, c == 1, g_new, b_new);
    return out;
  }

  Func apply_gamma(Func input, std::string name) {
    Var x("x"), y("y"), c("c");
    Func out(name);
    Expr val = input(x, y, c);
    Expr gamma = select(val < 0.0031308f, 12.92f * val,
                        1.055f * pow(val, 1.0f / 2.4f) - 0.055f);
    Expr final_val = clamp(gamma * 255.0f + 0.5f, 0.0f, 255.0f);
    out(x, y, c) = cast<uint8_t>(final_val);
    return out;
  }

  void generate() {
    Var x("x"), y("y"), c("c");

    linearised = Func("linearised");
    Expr raw_f = cast<float>(rawParam(x, y));
    linearised(x, y) = clamp((raw_f - pBl) / pRange, 0.0f, 1.0f);

    clamped = BoundaryConditions::repeat_edge(
        linearised, {{0, rawParam.width()}, {0, rawParam.height()}});

    Expr px = x % 2;
    Expr py = y % 2;

    g_h = Func("g_h");
    Expr is_G = ((px + py) == 1);
    Expr g_horiz =
        (clamped(x - 1, y) + clamped(x + 1, y)) / 2.0f +
        (clamped(x, y) - (clamped(x - 2, y) + clamped(x + 2, y)) / 2.0f) / 2.0f;
    g_h(x, y) = select(is_G, clamped(x, y), clamp(g_horiz, 0.0f, 1.0f));

    g_v = Func("g_v");
    Expr g_vert =
        (clamped(x, y - 1) + clamped(x, y + 1)) / 2.0f +
        (clamped(x, y) - (clamped(x, y - 2) + clamped(x, y + 2)) / 2.0f) / 2.0f;
    g_v(x, y) = select(is_G, clamped(x, y), clamp(g_vert, 0.0f, 1.0f));

    r_h = Func("r_h");
    b_h = Func("b_h");
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

    r_v = Func("r_v");
    b_v = Func("b_v");
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

    lum_h = Func("lum_h");
    lum_v = Func("lum_v");
    lum_h(x, y) = 0.299f * r_h(x, y) + 0.587f * g_h(x, y) + 0.114f * b_h(x, y);
    lum_v(x, y) = 0.299f * r_v(x, y) + 0.587f * g_v(x, y) + 0.114f * b_v(x, y);

    homo_h = Func("homo_h");
    homo_v = Func("homo_v");
    Expr lh_c = lum_h(x, y);
    Expr lv_c = lum_v(x, y);
    auto countH = [&](int dx, int dy) -> Expr {
      return cast<float>(abs(lh_c - lum_h(x + dx, y + dy)) <
                         abs(lv_c - lum_v(x + dx, y + dy)));
    };
    auto countV = [&](int dx, int dy) -> Expr {
      return cast<float>(abs(lv_c - lum_v(x + dx, y + dy)) <
                         abs(lh_c - lum_h(x + dx, y + dy)));
    };
    homo_h(x, y) = countH(-1, 0) + countH(1, 0) + countH(0, -1) + countH(0, 1) +
                   countH(-1, -1) + countH(1, -1) + countH(-1, 1) +
                   countH(1, 1);
    homo_v(x, y) = countV(-1, 0) + countV(1, 0) + countV(0, -1) + countV(0, 1) +
                   countV(-1, -1) + countV(1, -1) + countV(-1, 1) +
                   countV(1, 1);

    sum_homo_h = Func("sum_homo_h");
    sum_homo_v = Func("sum_homo_v");
    Expr sh = cast<float>(0), sv = cast<float>(0);
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        sh += homo_h(x + dx, y + dy);
        sv += homo_v(x + dx, y + dy);
      }
    sum_homo_h(x, y) = sh;
    sum_homo_v(x, y) = sv;

    demosaic = Func("demosaic");
    demosaic(x, y, c) =
        select(sum_homo_h(x, y) > sum_homo_v(x, y),
               select(c == 0, r_h(x, y), c == 1, g_h(x, y), b_h(x, y)),
               select(c == 0, r_v(x, y), c == 1, g_v(x, y), b_v(x, y)));

    diff_r = Func("diff_r");
    diff_b = Func("diff_b");
    diff_r(x, y) = demosaic(x, y, 0) - demosaic(x, y, 1);
    diff_b(x, y) = demosaic(x, y, 2) - demosaic(x, y, 1);

    auto med3 = [](Expr a, Expr b, Expr c) {
      return max(min(a, b), min(max(a, b), c));
    };
    auto med5 = [&](Expr a, Expr b, Expr c, Expr d, Expr e) {
      return med3(max(min(a, b), min(c, d)), min(max(a, b), max(c, d)), e);
    };

    refined_r = Func("refined_r");
    refined_b = Func("refined_b");
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

    prophoto_rgb = Func("prophoto_rgb");
    Expr dr = refined_r(x, y);
    Expr dg = demosaic(x, y, 1);
    Expr db = refined_b(x, y);
    prophoto_rgb(x, y, c) =
        clamp(select(c == 0, dr * pC2P00 + dg * pC2P01 + db * pC2P02,
                     c == 1, dr * pC2P10 + dg * pC2P11 + db * pC2P12,
                     dr * pC2P20 + dg * pC2P21 + db * pC2P22),
              0.0f, 1.0f);

    // Sequence must match DNG SDK: HueSatMap -> Exposure -> LookTable -> ToneCurve -> RGBtoFinal
    hsm_applied = apply_3dlut(prophoto_rgb, hsmParam, hsmHD, hsmSD, hsmVD,
                              hasHSM, "hsm_applied");

    exp_gain_applied = Func("exp_gain_applied");
    exp_gain_applied(x, y, c) =
        clamp(hsm_applied(x, y, c) * pExpGain, 0.0f, 1.0f);

    lt_applied = apply_3dlut(exp_gain_applied, ltParam, ltHD, ltSD, ltVD, hasLT,
                             "lt_applied");
    tc_applied = apply_tone_curve(lt_applied, tcParam, hasTC, "tc_applied");
    lr_applied = apply_lr_params(tc_applied, hasLR, lrExpGain, lrContrastFactor,
                                 lrSat, lrVib, "lr_applied");

    hsl_applied = apply_hsl_lut(lr_applied, hslLUT, hasHSL, "hsl_applied");

    final_srgb = Func("final_srgb");
    Expr pr = hsl_applied(x, y, 0);
    Expr pg = hsl_applied(x, y, 1);
    Expr pb = hsl_applied(x, y, 2);
    final_srgb(x, y, c) =
        clamp(select(c == 0, pr * pP2S00 + pg * pP2S01 + pb * pP2S02,
                     c == 1, pr * pP2S10 + pg * pP2S11 + pb * pP2S12,
                     pr * pP2S20 + pg * pP2S21 + pb * pP2S22),
              0.0f, 1.0f);

    exposed = apply_gamma(final_srgb, "exposed");
  }

  void schedule() {
    Var x("x"), y("y"), c("c");

    exposed.dim(0).set_stride(4);
    exposed.dim(2).set_stride(1);

    if (get_target().has_gpu_feature()) {
      Var xo("xo"), xi("xi"), yo("yo"), yi("yi");

      exposed.bound(c, 0, 3).reorder(c, x, y).gpu_tile(x, y, xo, yo, xi, yi, 16, 16).unroll(c);

      // Point-wise color operations are left as compute_inline() to fuse into `exposed`
      // This saves massive VRAM bandwidth and kernel launch overhead on Metal.

      // AHD Intermediate stages - keeping demosaic and refined stages in global memory
      // to avoid combinatorial explosion of math during Halide bounds inference.
      g_h.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
      g_v.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
      r_h.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
      b_h.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
      r_v.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
      b_v.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
      demosaic.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
      refined_r.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
      refined_b.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 16, 16);
    } else {
      Var yo("yo"), yi("yi");
      exposed.split(y, yo, yi, 64).parallel(yo).vectorize(x, 8);

      exp_gain_applied.compute_at(exposed, yo).vectorize(x, 8);
      hsm_applied.compute_at(exposed, yo).vectorize(x, 8);
      lt_applied.compute_at(exposed, yo).vectorize(x, 8);
      tc_applied.compute_at(exposed, yo).vectorize(x, 8);
      lr_applied.compute_at(exposed, yo).vectorize(x, 8);
      hsl_applied.compute_inline();

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

      prophoto_rgb.compute_at(exposed, yo).vectorize(x, 8);
      final_srgb.compute_at(exposed, yo).vectorize(x, 8);
      linearised.compute_at(exposed, yo).vectorize(x, 8);
    }
  }
};

HALIDE_REGISTER_GENERATOR(DngPipeline, dng_pipeline)
