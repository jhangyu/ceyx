---
date: 2026-03-05T19:52:00
type: implementation_plan
related_task: "Phase 5.1 進階 Metadata 提取 — XMP Lightroom 參數解析"
status: success
---

## 目標
解析 `rawXmp` 字串中的 `crs:Exposure2012`, `crs:Saturation`, `crs:Vibrance`，並在 HalidePipeline 的 Step 5c 套用。

## 修改計畫

### 1. `DngDecoder.h`
- 新增 `LightroomParams` struct (exposure2012, contrast2012, saturation, vibrance, parsed)
- 加入 `LightroomParams lrParams` 欄位到 `DngMetadata`

### 2. `DngDecoder.cpp`
- 新增 `parseCrsFloat(xmp, key)` — 支援 attribute 格式和 element 格式
- 新增 `parseLightroomParams(xmp)` — 解析並填充 LightroomParams
- 在 rawXmp 提取成功後呼叫 `parseLightroomParams`

### 3. `HalidePipeline.cpp`
- 新增 Step 5c: 在 LookTable 後，以 HSV 空間套用 LR Exposure2012 (乘法) / Saturation (%)/ Vibrance (智慧飽和)

### 4. `test_main.cpp`
- 新增 Test 2.5 (parsed==true), 2.6 (exposure range), 2.7 (saturation range)

## 預期結果
- `parseCrsFloat("crs:Exposure2012")` → `0.25`
- PSNR 不低於 18.15 dB（sample.dng sat=0, vib=0，只有 Exposure 有效）
