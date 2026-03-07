---
date: 2026-03-05T20:50:00
type: summary
related_task: "Phase 5.3 Halide 色彩管線增強 — ProfileToneCurve 修正"
status: in_progress
---

## 現狀確認

### 斷點
Phase 5.3 的 ToneCurve 實作在上一次對話中遭遇上下文超限，程式修改到一半中斷。

### 已完成
1. 513-entry ACR3 LUT (`kAcrDefaultLUT[513]`) 已正確嵌入 `HalidePipeline.cpp` (lines 199-346)
2. `evalAcrLUT()` 函式已實作 (lines 348-360)
3. `applyToneCurveBuffer()` 已更新，支援 profile curve 與 ACR LUT 雙路徑 (lines 365-378)
4. `20-45-00_memory.md` 已記錄兩個致命錯誤的發現與推導過程

### 尚未完成（兩個 Bug）
1. **編譯錯誤**：Lines 621-626 仍引用不存在的 `kAcrDefaultCurve` / `kAcrDefaultCurveCount`，需改為使用 `nullptr, 0` 觸發 `applyToneCurveBuffer` 內的 ACR LUT 路徑。
2. **套用順序錯誤**：ToneCurve (Step 5d, lines 616-631) 目前位於 HueSatMap 與 LookTable 之間，但 DNG SDK 實際順序為 HueSatMap → LookTable → ToneCurve。

### 下一步
撰寫 implementation_plan，修正上述兩個 Bug，重新編譯並進行 PSNR 測試。
