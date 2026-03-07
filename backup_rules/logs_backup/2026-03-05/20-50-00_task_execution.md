---
date: 2026-03-05T20:50:00
type: task_execution
related_task: "Phase 5.3 Halide 色彩管線增強 — ProfileToneCurve 修正"
status: in_progress
---

## Modify Plan
修改 1 個檔案：
1. `HalidePipeline.cpp` — 
   - 刪除 lines 616-631 (Step 5d ToneCurve 在 HSM 與 LT 之間的錯誤位置)
   - 在 LookTable 之後（原 line 640 後）插入正確的 ToneCurve 區塊
   - 修正變數引用：使用 `nullptr, 0` 替代不存在的 `kAcrDefaultCurve`
   - 更新頂部管線階段註解

## ⏹️ 中斷點快照 (Breakpoint Snapshot)
- **已完成**: 修改 HalidePipeline.cpp（三處修正）、編譯成功、27 個測試全部 PASS
- **下一步**: 任務完成。下一階段：Exposure/Contrast 控制或 HSL 個別顏色調整
- **待確認**: 無
- **更新時間**: 2026-03-05T20:55:00

## Test Result
- **編譯**: ✅ 成功（無錯誤）
- **測試**: 27 passed, 0 failed
- **PSNR**: 19.57 dB（從 19.34 dB 提升 +0.23 dB）
- **ACR ToneCurve**: `[CPU] Applying ToneCurve: ACR default 1025-LUT`
- **管線順序**: HueSatMap → LookTable → ToneCurve → LR params ✅
- **平均 RGB**: (108.5, 106.5, 111.9) — 正常範圍，無暗部壓碎

## Modify Summary
修正了三個致命 Bug：
1. **kAcrDefaultLUT 陣列大小**：從 `[513]` 修正為 `[1025]`（DNG SDK 原始 ACR3 forward LUT 實際為 1025 個元素，非先前假設的 513）
2. **ToneCurve 套用順序**：從 HueSatMap → ToneCurve → LookTable 修正為 HueSatMap → LookTable → ToneCurve（符合 DNG SDK `dng_render_task::ProcessArea` 實際執行順序）
3. **變數引用修正**：移除不存在的 `kAcrDefaultCurve` / `kAcrDefaultCurveCount`，改用 `nullptr, 0` 觸發 `applyToneCurveBuffer` 內建的 ACR LUT fallback 路徑
