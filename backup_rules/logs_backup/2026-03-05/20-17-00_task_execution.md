---
date: 2026-03-05T20:17:00
type: task_execution
related_task: "Phase 5.3 Halide 色彩管線增強 — ProfileToneCurve 映射"
status: in_progress
---

## Modify Plan
修改 2 個檔案：
1. `HalidePipeline.cpp` — 加入 `evalToneCurve()`、`kAcrDefaultCurve`、`applyToneCurveBuffer()` 函式（anonymous namespace），並在 Step 5a (HueSatMap) 之後、Step 5b (LookTable) 之前插入 Step 5d 呼叫
2. `test_main.cpp` — 新增 Test 6.0b (toneCurveCount 印出) 及 Test 6.0c (確認 ACR fallback)

## ⏹️ 中斷點快照 (Breakpoint Snapshot)
- **已完成**: 讀取 rule.md、implementation_plan.md、HalidePipeline.cpp（確認插入位置 line 174 與 line 408-417）
- **下一步**: 修改 HalidePipeline.cpp（加入函式 + Step 5d 呼叫）
- **待確認**: 無
- **更新時間**: 2026-03-05T20:17:00

## Test Result
- **編譯失敗 (Compile Error)**:
  1. `HalidePipeline.cpp:273:25: error: excess elements in array initializer` — `kAcrDefaultLUT[513]` 宣告與實際填入的元素數量不符（貼上了過多的控制點）。
  2. `HalidePipeline.cpp:623:33: error: use of undeclared identifier 'kAcrDefaultCurve'` — 變數命名不一致（宣告了 `kAcrDefaultLUT` 卻呼叫 `kAcrDefaultCurve`）。

## Modify Summary
- **發現的錯誤與內容**：
  1. **程式碼錯誤**：陣列大小宣告與初始化的實際元素數量不相符，且使用了未宣告的變數 `kAcrDefaultCurve` 及 `kAcrDefaultCurveCount`。
  2. **色彩空間發現**：確認了 ToneCurve 的應用位置應依照 DNG SDK 標準：HueSatMap 之後、LookTable 之前，並且是在 Linear / Gamma 處理前的浮點緩衝區內 (channel-wise) 套用。
- **下一步行動**：需要修正 `HalidePipeline.cpp` 中的陣列定義（可回退為使用簡單的 14 控制點 `kAcrDefaultCurve` 進行分段線性插值，避免 513 點大陣列出錯），並統一變數命名，重新進行編譯與 PSNR 測試。
