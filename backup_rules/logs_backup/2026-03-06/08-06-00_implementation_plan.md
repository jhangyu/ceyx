---
date: 2026-03-06T08:06:00
type: implementation_plan
related_task: "Phase 5.3 Halide 色彩管線增強 — Exposure/Contrast/Saturation/Vibrance"
status: in_progress
---

# Phase 5.3 Implementation Plan: Exposure, Contrast, Saturation, Vibrance

## 背景

Phase 5.3 剩餘兩項任務：
1. **加入 `Exposure` 與 `Contrast` 控制**
2. **實作色彩飽和度與鮮艷度 (Vibrance) 調整邏輯**

目前管線在 `HalidePipeline.cpp` 的 `section 5c` 中，已套用 Exposure2012、Saturation、Vibrance（基礎版本），但：
- ❌ **Contrast2012 完全未套用**（只解析了值，沒有套用）
- ⚠️ **Vibrance 公式**可加入 skin tone 保護邏輯

## 提議修改

### Lightroom 計算模型參考

#### Exposure（曝光）— 現有實作已正確
```
V_new = clamp(V * pow(2, exposure2012), 0, 1)
```
在 ToneCurve 之後的 HSV V 通道套用，符合 Lightroom 行為。
> **維持現有邏輯，不修改。**

#### Contrast（對比）— 待實作
Lightroom Contrast 是一條以中灰（約 0.18 線性 = 0.5 感知）為中心的 S 形曲線。
- contrast=0 → 恆等
- contrast=+25 → 高光提亮、暗部壓暗（S 曲線向外彎）
- contrast=-25 → 反向（壓縮）

Lightroom 的 Contrast 實作近似為「基於 logistic 函數的 S 曲線」。
我們用一個簡化的對稱 S-curve，以 ACR 計算的灰階點 (0.18 ≈ Pow(0.18, 1/2.4)+gamma ≈ 0.46 in sRGB but we're in linear) 為中心。

由於我們在 ToneCurve 之後已在線性空間處理（ToneCurve 後 V 已是線性的感知調整值），
Contrast 的套用應在 **ToneCurve 之後、gamma 之前**，對 `V`（HSV 空間）操作：

```cpp
// Lightroom Contrast S-curve (simplified)
// c = contrast2012 / 100.0  (normalized to [-1, +1])
// pivot = 0.18（線性空間中灰，Lightroom 預設）
//
// 公式：
//   t = V - pivot
//   S(t) = t / (1 - c * t / clamp(|t|, eps, infinity))
//        → 簡單 S-curve 近似
//
// 更精確的版本（從 Lightroom dng_render.cpp 類似實作推導）：
//   V_adjusted = pivot + sign(V - pivot) * pow(|V - pivot| / pivot, 1 - c)
//              (只在橢圓端點插值)
//
// 實務上使用 ACR 對比曲線的簡化版：
//   tan_half = tan((c * PI/4))   — c 歸一化至 [-1,1]
//   V_out = (V - pivot) / (1 + tan_half * (V - pivot)) + pivot
//   (限制在 c ∈ [-0.9, 0.9] 避免奇點)
```

考慮到精度與效能的平衡，採用以下實作：

```cpp
// Lightroom Contrast: S-curve centered at pivot (linear 0.18)
// contrast2012 in [-100, +100], normalized to c = contrast / 100.0
static float applyContrast(float v, float c) {
    const float pivot = 0.18f;
    if (c == 0.0f || std::abs(c) < 1e-4f) return v;
    // Scale control: map [-1,1] -> [-0.9, 0.9] to avoid extremes
    float s = c * 0.9f;
    // Parametric S-curve:
    // For v > pivot: compress towards 1.0 (brighter highlights)
    // For v < pivot: compress towards 0.0 (darker shadows)
    float t = v - pivot;
    float denom = 1.0f - s * t;
    if (std::abs(denom) < 1e-6f) denom = 1e-6f;
    return std::min(std::max(pivot + t / denom, 0.0f), 1.0f);
}
```

#### Saturation（飽和度）— 現有實作已正確
```
s_new = clamp(s * (1.0 + saturation/100), 0, 1)
```
> **維持現有邏輯，不修改。**

#### Vibrance（鮮艷度）— 改進 skin tone 保護
Lightroom Vibrance 的關鍵特性：
1. 低飽和像素獲得更多 boost（`boost = vibrance * (1-s)`）
2. **Skin tone 保護**：偏紅/橘色（Hue 約 15°-45°，即 h ∈ [0.042, 0.125] in [0,1]）的像素 boost 被減弱

目前實作：`boost = lrVib * (1.0f - s)` — 缺少 skin tone 保護

改進版本：
```cpp
// Vibrance with skin tone protection
// Skin tones: hue roughly in orange-red range (H ≈ 15°-45° i.e. [0.042,0.125])
float skinProtect = 1.0f;
if (s > 0.1f) {  // only for non-achromatic
    float hDeg = h * 360.0f;
    if (hDeg >= 10.0f && hDeg <= 50.0f) {
        // Reduce boost linearly towards center of range (30°)
        float dist = std::abs(hDeg - 30.0f) / 20.0f;  // 0 at center, 1 at edge
        skinProtect = dist;  // 0 boost at h=30°, full boost at edge
    }
}
float boost = lrVib * (1.0f - s) * skinProtect;
s = std::clamp(s + boost, 0.0f, 1.0f);
```

## 修改計畫

### 修改 1 個檔案

#### [MODIFY] `HalidePipeline.cpp`
**位置**：`dng_processor/native/src/HalidePipeline.cpp`

**修改點 1**：在 `section 5c`（Lines 642-701）的 for 迴圈中，**在 Exposure 之後、Saturation 之前**加入 Contrast 套用：
```cpp
// Contrast2012: S-curve on value channel
const float lrCon = static_cast<float>(metadata.lrParams.contrast2012 / 100.0);
const bool hasCon = std::abs(lrCon) > 1e-4f;

// ... in pixel loop ...
if (hasExp) { v = std::min(v * lrExpGain, 1.0f); }
if (hasCon) { v = applyContrast(v, lrCon); }  // NEW
if (hasSat && s > 1e-6f) { ... }
```

**修改點 2**：加入 `applyContrast()` helper 函數（在 anonymous namespace）

**修改點 3**：改善 Vibrance 迴圈以加入 skin tone 保護

**修改點 4**：更新 `hasXxx` 判斷，加入 `hasCon`

**修改點 5**：更新管線階段頂部的 pipeline 說明註解

## 驗證計畫

### 自動化測試

執行現有測試套件：

```bash
cd /Users/jhangyu/Documents/flutter_dng_decoder/dng_processor/native/build
cmake --build . 2>&1 | tail -20
./test_dng_decoder ../../tests/fixtures/sample.dng
```

**預期結果**：
- 編譯無錯誤
- 27 tests passed, 0 failed（維持現有測試通過）
- PSNR > 15 dB（維持現有閾值）
- 新增 log 輸出確認 Contrast 有被套用

### 日誌驗證

確認 stderr 輸出包含：
```
[CPU] Applying LR params: ExpGain=X.XX Con=Y.YY Sat=Z.ZZ Vib=W.WW
```

### PSNR 評估

記錄所有四個參數套用後的 PSNR，與當前 19.57 dB 比較：
- 若 DNG 的 contrast2012 ≠ 0，PSNR 應有變化（可能提升或降低，取決於 DNG 本身的值）
- 最重要的是 **測試全部 PASS，無退化**

