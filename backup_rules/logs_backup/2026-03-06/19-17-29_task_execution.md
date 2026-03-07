---
date: 2026-03-06T19:17:29
type: task_execution
related_task: "Phase 5.3 剩餘：Contrast2012 + Saturation/Vibrance 精進"
status: in_progress
---

## Modify Plan

修改 `HalidePipeline.cpp` 第 661~720 行 (Phase 5.1/5c LR params block)：
1. **加入 Contrast2012 套用**：在 Exposure2012 後，對 RGB 通道以中性點 0.5 為軸套用對比度縮放。
2. **精進 Saturation**：改為非線性模型，正值時加入 `s*(1-s)` 非線性項，防止高飽和過衝。
3. **精進 Vibrance**：改為二次衰減 `lrVib * (1 - s^2)`，更符合 Lightroom 行為。
4. **更新 YAML 索引**：更新 HalidePipeline.cpp 開頭的 YAML modules 描述。

## ⧍️ 中斷點快照 (Breakpoint Snapshot)
- **已完成**: 修改 HalidePipeline.cpp ，實作 Contrast2012 + 精進 Saturation/Vibrance，編譯成功
- **下一步**: 執行測試、更新文件、提交 git
- **待確認**: 無
- **更新時間**: 2026-03-06T19:20:00

## Test Result

```
執行命令: ./test_dng_decoder sample.dng
日期: 2026-03-06T19:20

Results: 27 passed, 0 failed

PSNR: 19.5661 dB (threshold: 15 dB) ✅ (vs 之前 19.57 dB，基本相同，無回退)

LR Params 日誌瑪: [CPU] LR params: ExpGain=1.18921 Contrast=0 Sat=0 Vib=0
(樣本 DNG 中 Contrast2012=0，Saturation=0，Vibrance=0——驗證新逻輯正確跳過)
```

## Modify Summary

成功實作 Phase 5.3 剩餘兩項：

1. **Contrast2012**: 新增中性點軸心 (0.5) RGB 空間套用。公式：`out = 0.5 + (in - 0.5) * (1 + contrast/100)`。
   加入 `hasContrast` 判斷，當 contrast 接近 0 時跳過處理。

2. **Saturation 精進**: 正値改用非線性標法 `s += lrSat * s * (1-s)`，
   與原線性公式相比，高飽和像素獲得更少的提升（更接近 Lightroom）。
   負値保持原線性公式不變。

3. **Vibrance 精進**: 改為二次方衰減 `boost = lrVib * (1 - s^2)`，
   和原 `1 - s` 相比更平滑地保護高飽和像素。

4. **YAML 索引**: 更新檔案開頭的 modules 行號與描述。
5. **Pipeline 注解**: 更新管線注解對 5c 階段的說明。

編譯與測試結果: 27/27 tests PASSED，PSNR = 19.57 dB (無回退) ✅
