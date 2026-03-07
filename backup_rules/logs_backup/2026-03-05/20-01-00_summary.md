---
date: 2026-03-05T20:01:00
type: summary
related_task: "Phase 5.3 Halide 色彩管線增強 — ProfileToneCurve 映射"
status: in_progress
---

## 環境掃描現狀總結

### 1. 當前斷點
- **Phase 5.1 已完成**：`parseCrsFloat()` XMP 解析，Exposure2012=+0.25 已套用，PSNR = **19.34 dB**
- **ProfileToneCurve 資料已存在**：`DngMetadata.toneCurvePoints[256]` 與 `toneCurveCount` 已於 `DngDecoder.cpp` 中填充，但尚未在 `HalidePipeline.cpp` 中套用。
- **DNG SDK 渲染管線參考順序**（`dng_color_metadata_spec.md`）：
  ```
  Demosaic → Camera→sRGB → Exposure → HueSatMap → BaselineRGBTone(ProfileToneCurve) → LookTable → Gamma
  ```
  目前管線缺少 `ProfileToneCurve` 步驟。

### 2. 本次目標 (Phase 5.3)
實作 `ProfileToneCurve` 分段線性插值：
- 當 `toneCurveCount > 0`：使用 DCP Profile 內嵌的控制點做分段線性插值
- 當 `toneCurveCount == 0`：使用 Adobe ACR 預設曲線（Medium Contrast，已知的 9 個控制點）
- 套用位置：DCP HueSatMap 之後，LookTable 之前（遵循 DNG SDK 渲染順序）

### 3. PSNR 提升預期
根據 `unit_test.md` 的預期：套用 Tone Curve 後 PSNR 應提升 **3–6 dB**，目標值 ~22–25 dB。

### 4. 關聯檔案
- `dng_processor/native/include/DngDecoder.h` — `toneCurvePoints`, `toneCurveCount` 已定義
- `dng_processor/native/src/HalidePipeline.cpp` — 新增 Step 5d 在此
- `dng_processor/native/tests/test_main.cpp` — 新增 Tone Curve 驗證測試
