---
date: 2026-03-06T19:50:30
type: summary
---

## 1. 專案與環境現狀總結
- 由於加入 Phase 5.2 的 AHD 與十字中值濾波後，運算複雜度大幅提升，導致 CPU 運算的去馬賽克解碼時間上升了近五倍（目前測達約 5000ms 以上）。
- 為了確保畫面不失真，我們放棄了換用劣質演算法的念頭，轉向 Halide 的核心優勢：**Schedule (排程)**。

## 2. 待解決的問題與目標 (Phase 6.1)
- **問題**：缺乏 Schedule 定義的 Halide Pipeline 會自動採用最基礎的 Inline / 逐像素循序計算，導致 Cache 失誤率極高，且未發揮多核心優勢。
- **短期目標 (CPU 優化)**：
  - 放棄原訂的 Phase 5.4，優先進行 Phase 6.1 基礎提速。
  - 在 `HalidePipeline.cpp` 的最後，對於產出的 `processed_image` (或最後一層) 套用基於區塊 (`tile`)、多執行緒 (`parallel`) 與向量化 (`vectorize`) 的 Halide 基礎排程指令。
  - 使用 `compute_root()` 及 `compute_at()` 管理暫存 buffers (例如 `demosaic`, `refined_r`), 來達成記憶體運用與計算速度的完美平衡。
  - 編譯後確認效能回升程度。

## 3. 中期目標 (GPU 預備)
- 在證明 CPU Schedule 能大幅改善現狀後，我們將接續實行 GPU Schedule (Metal / Vulkan API 接軌)，真正將像素的繁重計算從 CPU 完全卸載。
