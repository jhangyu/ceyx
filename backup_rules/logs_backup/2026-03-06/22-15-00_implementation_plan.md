---
date: 2026-03-06T22:15:00
type: implementation_plan
related_task: "Phase 6.3: Halide GPU 加速"
status: in_progress
---

# Phase 6.3 Implementation Plan: Halide GPU Acceleration

## 目標
透過 Halide 的 GPU 後端（Metal/OpenCL/Vulkan）加速影像管線，進一步將運算時間壓縮至 500ms 以下。

## 預計變更

### 1. 修改 `CachedPipeline::build`
- **Target 偵測**：使用 `Halide::get_host_target()` 並嘗試附加 GPU 特性（如 `Target::Metal` 或 `Target::OpenCL`）。
- **GPU 排程實作**：
  - 在 `exposed` Func 上使用 `.gpu_tile(x, y, xi, yi, 16, 16)`。
  - 對中間 Funcs（如 `demosaic`）進行排程以適應 GPU 的平行架構（`compute_root` 或 `gpu_tile`）。
- **JIT 編譯更新**：確保編譯時指定了正確的 GPU Target。

### 2. 資料傳輸與同步 
- **Input Buffers**：在 `process` 函數中，呼叫 `realize` 前，需確保 `bayer_buffer`, `hsm_buf`, `lt_buf`, `tc_buf` 已透過 `set_host_dirty()` 或 `copy_to_device()` 使其 GPU 可見。
- **Output Buffer**：`realize(halide_out)` 後需呼叫 `halide_out.copy_to_host()` 確保結果回流至 CPU Memory 供 Flutter 使用。

### 3. 多平台相容性
- **降級機制**：若系統不支援 GPU，自動回退 (Fall-back) 至目前的 CPU Schedule，確保 App 穩定性。

## 驗證計畫
1. **編譯驗證**：確保啟用了 GPU 特性後能順利完成 JIT 編譯。
2. **效能驗證**：比較 GPU `realize()` 時間與目前 CPU 的 1.5s，目標為 **< 500ms**。
3. **精度驗證**：PSNR 必須保持在 **19 dB** 以上，確保 GPU 運算與 CPU 結果基本一致。

## ⚠️ 風險提示
- **冷啟動開銷**：GPU JIT 編譯與 Shader 加載可能在第一次執行時較慢，需觀察 `CachedPipeline` 是否能有效抵銷此開銷。
- **記憶體牆**：大圖 (24MP) 的 Host-to-Device 傳輸可能成為新的瓶頸。
