---
date: 2026-02-27T11:01:00
type: implementation_plan
related_task: "Phase 5.1 進階 Metadata 提取與色彩校正"
status: success
---

## 目標
實作從 `rawXmp` 解析 Lightroom 色彩參數，並且於 Native 影像管線的 HSV 色彩空間中，實作相應的對比、全局飽和度 (Saturation) 與鮮豔度 (Vibrance) 提昇機制。

## 預計修改清單 (Modify Plan)

### 1. `DngDecoder.h` & `DngDecoder.cpp`:
- 新增 `struct LightroomParams`，包含 `saturation`, `vibrance`, `exposure2012` 等浮點數欄位。
- 在萃出 `rawXmp` 後，實作原生的 Regex-free 字串搜尋函式 (例如 `parseCrsFloat`) 取得這些參數，存入 `metadata.lrParams`。

### 2. `HalidePipeline.h` & `HalidePipeline.cpp`:
- 在 `process()` 的函數標籤中，直接接入 `saturation` 與 `vibrance` 新引數。
- 於 CPU 端執行的 `applyHueSatMap` 轉換階段，同時結合全局 `saturation` 與 `vibrance` 演算法。
- **演算法核心邏輯**： 
  - 基礎飽和度提昇：`sat_new = clamp(sat * (1 + saturation/100), 0, 1)`
  - Lightroom 風格的鮮豔度提昇 (保護已飽和像素)：`boost = vibrance/100 * (1 - sat)`, `sat_final = clamp(sat_new + boost, 0, 1)`

### 3. `dng_ffi_api.cpp` / `test_main.cpp`:
- 將解讀出的 Lightroom 參數接上 `HalidePipeline::process()`。
- 單元測試將印出被解析到的數值，確保 XMP Parser 可靠地將 Lightroom 設定讀出。

## 預期檢驗方法
- **XMP Parsing**：測試腳本應能將 `sample.dng` 中的字串確切印出，例如 `crs:Exposure2012=+0.25` 等資訊。
- **Halide 執行**：編譯與執行時間應無明顯效能退化，並維持在低於 0.5s 的目標內。
- **Crash Rate**：不能發生 Out-of-bounds 等記憶體讀寫錯誤，需產出可視化的除錯圖片。
