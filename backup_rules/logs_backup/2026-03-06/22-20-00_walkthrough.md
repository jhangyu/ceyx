---
date: 2026-03-06T22:00:00
type: walkthrough
related_task: "Phase 6.3: Halide GPU 加速"
status: success
---

# Phase 6.3 Walkthrough: Halide Metal GPU Acceleration

## 完成項目

### 1. 修改 `HalidePipeline.cpp`
- **GPU Target 偵測**：在 `build()` 開頭加入 `#ifdef __APPLE__` 區塊，使用 `host.with_feature(Target::Metal)` 建立 Metal target。
- **條件式排程**：
  - **GPU 路徑**：`exposed` + 所有後處理 Funcs (HSM/LT/TC/LR/Gamma) → `gpu_tile(x, y, 16, 16)`；AHD demosaic 中間 Funcs → `compute_root()`。
  - **CPU Fallback 路徑**：保留原有 `split/parallel/vectorize` 排程。
- **compile_jit with Target**：`exposed.compile_jit(compile_target)` 確保使用 GPU target。
- **copy_to_host()**：`realize()` 後加入 `halide_out.copy_to_host()`，確保 GPU 結果回流 CPU。

### 2. 修改 `CMakeLists.txt`
- 加入 `find_library(METAL_LIBRARY Metal)` + `find_library(FOUNDATION_LIBRARY Foundation)`。
- 連結至 `dng_decoder_native` 以支援 Halide Metal runtime。

## 測試結果

```
Results: 28 passed, 0 failed
```

| 指標 | 結果 |
|------|------|
| GPU Backend | Metal ✅ `arm-64-osx-arm_dot_prod-arm_fp16-metal` |
| PSNR | 19.53 dB ✅ (>15 dB threshold，與 CPU 一致) |
| 1st-call realize | 2144 ms (含 Metal shader 初始化) |
| 2nd-call realize | **1071 ms** (vs CPU ~1500ms，提升 ~30%) |
| 所有測試 | 28/28 通過 ✅ |

## 效能分析

**已加速的部分（GPU tile)**：
- `hsm_applied`, `lt_applied`, `tc_applied`, `lr_applied` (後處理 Funcs)
- `exp_gain_applied`, `color_corrected`, `exposed` (色彩矩陣 + Gamma)

**尚未 GPU 加速的部分（compute_root 在 CPU）**：
- AHD demosaic 系列 Funcs（`g_h`, `g_v`, `r_h`, `b_h`, `r_v`, `b_v`, `lum_h`, `lum_v`, `homo_h`, `homo_v`, `sum_homo_h`, `sum_homo_v`, `demosaic`, `refined_r`, `refined_b`, `diff_r`, `diff_b`）
- 這些 Funcs 因有 stencil 依賴（鄰域存取），目前以 `compute_root()` 全圖計算後才丟給 GPU 後處理。

## 技術決策與教訓
- **AHD demosaic 的 GPU tile 挑戰**：stencil 計算（homogeneity map）的鄰域存取（最大 ±2 pixel 半徑）在 gpu_tile 模式下需要處理 tile boundary halo，複雜度較高。暫時以 compute_root() 保持正確性優先。
- **< 500ms 目標**：尚未達到。主要瓶頸仍在 AHD demosaic 的 CPU 側。Phase 6.4 可考慮優化此區塊。
