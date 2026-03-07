---
date: 2026-03-06T19:13:51
type: summary
related_task: "Phase 5.3 剩餘：Exposure+Contrast 控制與 Saturation/Vibrance 精進"
status: in_progress
---

## 現狀確認

### 環境掃描結果

| 文件 | 狀態 |
|------|------|
| `rule.md` | ✅ 已閱讀，嚴格遵守 6 步驟 SOP |
| `task.md` | ✅ 已讀，定位斷點 |
| `handover.md` | ✅ 已讀，了解技術背景 |
| `HalidePipeline.cpp` | ✅ 已讀，分析現有實作 |
| `DngDecoder.h` | ✅ 已讀，確認 LightroomParams 欄位 |
| `test_main.cpp` | ✅ 已讀，了解測試流程 |

### Phase 5.3 當前進度

已完成：
- ✅ `ProfileToneCurve`：ACR3 1025-entry LUT（HalidePipeline.cpp L382-398）
- ✅ `HueSatMap` + `LookTable`：CPU 三線性插值（L91-192）
- ✅ `Exposure2012`：以 2^EV 乘以 V 通道（L692-694）
- ✅ `crs:Saturation`：縮放 S 通道（L699-700，`s * (1 + lrSat)`）
- ✅ `crs:Vibrance`：簡單 `lrVib * (1 - s)` boost（L704-706）

**尚未完成（Phase 5.3 殘餘）：**
1. ❌ `Contrast2012`：程式碼中雖已解析（DngDecoder.cpp L82），但 HalidePipeline.cpp 中完全未套用。
2. ❌ Saturation/Vibrance 精進：Vibrance 實作邏輯過於簡化，未考慮 Lightroom 的色域保護機制（防止膚色過飽和）。

### 技術分析

**Contrast2012 的 Lightroom 行為：**
Lightroom 的 `Contrast2012` 以中性點 0.5 為軸心，非線性拉伸對比度。
數學公式：`output = 0.5 + (input - 0.5) * contrast_factor`
其中 `contrast_factor = 1.0 + contrast2012 / 100.0`（正值增加對比，負值減少）。
由於此為線性近似，實際 LR 使用的是更複雜的 S 型曲線。

**Vibrance 精進：**
更精確的 Lightroom Vibrance 行為：
- 正 Vibrance 時：低飽和度像素獲得更大 boost，已飽和像素幾乎不受影響。
- 同時保護膚色色調（約 hue = 0~50° 即紅至橙範圍），避免過度飽和。
- 精確公式：`boost = vibrance * (1 - s^2)` — 二次方衰減比一次方更接近 LR 行為。

目前對 Saturation 的診斷：現有 `s * (1 + lrSat)` 對於負值能工作，
但對於正值在 s 接近 1 時過衝。建議改為：
`s_new = clamp(s + (lrSat/100) * s * (1 - s), 0, 1)` — 這讓高飽和像素漸漸飽和。

### 測試方法
既有測試：`dng_processor/native/build/test_dng_decoder`
- 執行後會輸出 PSNR 分數，目標 > 19.57 dB（不應回退）
- 預期加入 Contrast 後略微改善或維持（因為我們對準的是 DNG SDK 的 Contrast=0 預設）
