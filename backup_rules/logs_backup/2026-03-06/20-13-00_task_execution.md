---
date: 2026-03-06T20:13:00
type: task_execution
related_task: "Phase 6.1: Verify Halide JIT Caching Performance"
status: success
---

## Modify Plan

**目標**: 驗證 JIT Cache 的表現，並確認為什麼管線整體處理時間依然長達 13 秒以上。

**執行內容**:
1. 在 `test_main.cpp` 新增第二次對 `HalidePipeline::process` 的呼叫 (Test 6.7) 來驗證快取是否觸發。
2. 在 `HalidePipeline.cpp` 中插入 `std::chrono::steady_clock::now()` 計時器，拆分 `pipe.exposed.realize()` 與 `C++ CPU 後處理 (HueSatMap/ToneCurve/Gamma 等)` 的執行時間。
3. 重新編譯並執行。

## ⏹️ 中斷點快照

- **已完成**: 加入精確內部計時器，執行第二次快取測試，並成功揪出真正的效能瓶頸。
- **下一步**: 向使用者匯報此重大發現，並決定要如何優化這 12.5 秒的 C++ 後處理瓶頸（例如：導入 OpenMP 或是移入 Halide）。
- **待確認**: 下一步優化策略。
- **更新時間**: 2026-03-06T20:13:00

## Test Result

```
--- Test 6.6 & 6.7: Halide Pipeline Performance ---
[Halide] JIT compiling pipeline (first call)...
[Halide Perf] pipe.exposed.realize took 510.929 ms
[Halide Perf] C++ gamma correction & packing took 523.635 ms
[Halide Perf] Total CPU post-processing took 12528 ms
  Halide process time (first call): 14544.6 ms

--- Test 6.7: Halide Pipeline (2nd call for cache check) ---
[Halide] WB: handled by CameraToPCS matrix (no explicit gains)
[Halide Perf] pipe.exposed.realize took 476.255 ms
[Halide Perf] C++ gamma correction & packing took 580.036 ms
[Halide Perf] Total CPU post-processing took 12730.4 ms
  Halide 2nd process time: 13381.2 ms
[FAIL] Test 6.7: Halide pipeline 2nd call < 2s (cached)
```

## Modify Summary

**重大發現 (Breakthrough)**:
1. **Halide 排程與快取非常成功**！`pipe.exposed.realize()` 對 24MP (6048x4024) 影像的執行時間只有 **476 ms** (不到 0.5 秒)！在有 AHD + Median 濾波的情況下這是極度優異的數字。
2. 第二次呼叫沒有觸發 `JIT compiling` 字樣，證明 Cache 完全生效。
3. **真正的瓶頸在於 C++ 原生後處理代碼**：`HueSatMap / ToneCurve / Lightroom 參數套用` 這個純 CPU 循序 for-loop 花費了高達 **12.5 秒**。因為 `applyHueSatMap` 和 main loop 內的 Trilinear 插值、RGB-HSV 轉換都是單執行緒 (Single-threaded) 且缺乏 SIMD 向量化，對兩千四百萬像素逐一運算造成了災難性的效能拖累。
