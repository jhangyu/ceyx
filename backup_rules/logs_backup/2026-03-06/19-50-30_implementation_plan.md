---
date: 2026-03-06T19:50:30
type: implementation_plan
---

## 1. 實作目標
藉由套用 Halide 強大的 Schedule 機制（主要包含 `.parallel()` 與 `.vectorize()`，以及記憶體快取階層 `.compute_at()`），將現階段 C++ `demosaic` 與色彩校正管線的處理時間從 5000ms 大幅降至 1000ms 左右的合理區間。

## 2. 設計邏輯 (CPU 排程優化)
Halide 預設是一次展開所有的迴圈，這在處理像 AHD 這樣需要讀取高頻相鄰像素的演算法時極端沒有效率。我們將為最後的 Output Func (`processed_image`) 及其依存的重度運算 Func 加入以下排程設定：

1. **分塊與平行運算 (Tiling & Parallelization)**
   針對 2400 萬畫素，將最終 `processed_image` 做區塊化：
   - 使用 `.tile(x, y, xi, yi, 256, 256)` 或依序 `.split()`，讓每個 CPU 核心去負責一個 Tile。
   - 套用 `.parallel(y)` 或 `.parallel` 至區塊的外層迴圈，讓所有 CPU 核心滿載運作。

2. **向量化 (Vectorization)**
   在最內層的 X 軸向計算上套用向量指令 (SIMD, 如 NEON)：
   - 對最內層變數套用 `.vectorize(x, 8)`。這裡 `8` 是一個適合多數手機或 Mac ARM 架構的最佳常數。

3. **計算粒度 (Compute Granularity)**
   `demosaic`, `refined_r`, `refined_b` 因為需要被多次參照，若每次參照都重新計算會慢到懷疑人生。我們需使用：
   - `demosaic.compute_at(processed_image, y)`: 指定這些階段要在輸出的 Tile 處理前就算好並暫存於 Cache 中。
   - 這能最大幅度減少記憶體從 RAM 重複載入的時間。

## 3. 預計修改檔案
- **`dng_processor/native/src/HalidePipeline.cpp`**
  - 確認加入 `Var xi, yi;` 與 `processed_image.tile(...)` 等。
  - 編譯 `.parallel` 和 `.vectorize`。
- **`task.md` / `handover.md`** 
  - 更換任務優先級：Phase 5.4 置後，立刻推進至 Phase 6.1 GPU 加速/效能優化。

## 4. 驗證方式
1. 執行 `make` / `cmake` 確保 Halide 排程語法正確 (未發生 Out of bounds 等計算順序錯誤)。
2. 從終端機觀察 `./test_color_accuracy` 給出的 `render_time`：目標從目前的 `5051.87ms` 有感縮小至 `1500ms` 或更低。
