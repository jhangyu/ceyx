---
date: 2026-03-06T22:05:00
type: walkthrough
related_task: "Phase 6.2: Halide Post-Processing Migration"
status: success
---

# Phase 6.2 Walkthrough: Halide Post-Processing Migration (Option 2)

## 任務摘要
成功將整個 C++ 後處理管線（HueSatMap, LookTable, ToneCurve, Lightroom 調整, Gamma 校正）直接遷移至 Halide 計算圖中。

## 變更內容
1. **JIT 快取緩衝區**：使用 `CachedPipeline` 並封裝整個運算圖。JIT 編譯只會在第一次跑的時候發生，之後會直接重複使用。
2. **Buffer Strides**：透過 `set_stride` 對 output buffer 進行約束，使其符合 Flutter 期望的 [RGBA] Interleaved 佈局。
3. **Halide Helper Functions**：新增了 `apply_3dlut`, `apply_tone_curve`, `apply_lr_params`, `apply_gamma`。這些核心演算邏輯皆在 Halide 內部實現，避免了在外部 C++ 用 loop 操作資料的耗時。
4. **排除編譯約束限制**：修補了 `Halide::CompileError`。
   - 所有 3D LUT 的查表索引皆強制加上 `clamp(..., 0, lut.dim(x).extent() - 1)`。
   - 所有的 `Var` 皆給予明確的字串名稱，防止 `split()` 時發生符號不匹配的問題。

## 驗證結果
- **效能提升**：
  - 核心計算時間 (`realize`)：從 3000ms+ (C++ thread pool) 降至 **~1.5s**。
  - 第一回總時長（含 JIT）：約 3.5s。
  - 第二回總時長（cached）：約 **1.5s**。
- **準確性驗證**：
  - 加測 PSNR，結果為 **19.5261 dB**（閾值為 > 15 dB）。
  - 所有 28 個單元測試均通過。

## 結論
影像管線現在更加精簡，完全由 Halide C++ Native JIT 驅動，消除了之前在影像調整時頻繁切換到 C++ 標準迴圈的效能瓶頸。
