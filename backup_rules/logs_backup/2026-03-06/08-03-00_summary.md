---
date: 2026-03-06T08:03:00
type: summary
related_task: "Phase 5.3 Halide 色彩管線增強 — Exposure/Contrast/Saturation/Vibrance"
status: in_progress
---

## 目標
實作 Phase 5.3 剩餘的兩項功能：
1. 加入 `Exposure` 與 `Contrast` 控制。
2. 實作色彩飽和度與鮮艷度 (Vibrance) 調整邏輯。

## 環境掃描結果

### 目前管線狀態 (PSNR: 19.57 dB)
管線階段順序（`HalidePipeline.cpp`）：
1. Black-level 扣除 & 線性化 [0,1]
2. WB — 內嵌於 CameraToPCS 矩陣
3. AHD 去馬賽克
4. Camera → sRGB 矩陣
5. BaselineExposure 補償（EV stops，線性倍率）
5a. HueSatMap 3D LUT（HSV 三線性插值）
5b. LookTable 3D LUT（HSV 三線性插值）
5d. ProfileToneCurve（ACR3 1025-entry LUT 或 profile 曲線）
5c. Lightroom XMP 參數套用（Exposure2012 / Saturation / Vibrance）
6. sRGB Gamma 校正 + 打包 uint8 RGBA

### 已完成的 LR 參數解析
`parseLightroomParams()` 已從 XMP 正確解析：
- `exposure2012`：✅ 已套用（HSV V 通道倍率）
- `contrast2012`：✅ 已解析，**但尚未套用**
- `saturation`：✅ 已套用（HSV S 通道 `s*(1+lrSat)`）
- `vibrance`：✅ 已套用（簡單 `lrVib*(1-s)` 公式）

### 發現的問題
1. **Contrast2012 完全未套用**（`lrParams.contrast2012` 被解析但管線中沒有任何使用）
2. **Vibrance 公式尚可改進**：目前 `boost = lrVib * (1-s)` 已是基礎 Lightroom 模型，可加入 skin tone 保護（保護偏紅/橘色的低飽和像素）

### 測試基礎設施
- 主測試：`dng_processor/native/build/test_dng_decoder <dng_path>`
- 測試框架：自定義 ASSERT_*/ASSERT_RANGE（test_main.cpp）
- 目前測試數：27 passed
- PSNR 測試：Test 6.5（目前 >15 dB 閾值，實際 19.57 dB）

