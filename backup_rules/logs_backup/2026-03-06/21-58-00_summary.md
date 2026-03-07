---
date: 2026-03-06T21:58:00
type: summary
related_task: "Phase 6.3: Halide GPU 加速"
status: in_progress
---

## 任務目標
啟用 Halide 的 GPU 加速功能（在 macOS 上為 Metal，並考慮後續跨平台的 Vulkan/OpenCL 支援），透過顯卡硬體並行處理影像管線，將 24MP 圖片的處理時間從目前的 1.5 秒進一步壓縮至 0.5 秒以下，達成實時瀏覽的極致體驗。

## 現狀總結
1. **CPU 優化完成**：Phase 6.1 與 6.2 已完成。影像管線目前完全在 Halide 內部執行，且使用了 CPU 排程（Parallel/Vectorize），2nd-call 時長約 **1.5s**。
2. **計算圖完備**：去馬賽克與所有後處理邏輯已全部轉化為 Halide `Func` 與 `Expr`，具備了搬移至 GPU 執行的基礎。
3. **環境掃描**：
   - 系統為 macOS，應優先測試 **Metal** 後端。
   - Halide 庫已導入，需確認編譯時是否包含了 GPU 特性支援。
   - 目前使用 JIT 模式。

## 待解決挑戰
- **GPU 數據傳輸**：需要處理 host memory 與 device memory 之間的拷貝開銷。
- **排程策略變更**：CPU 的 `vectorize` 與 GPU 的 `gpu_tile` 需要不同的邏輯分支或通用的排程描述。
- **測試驗證**：確保 GPU 運算精度（PSNR）與 CPU 對齊。

## 下一步
1. 撰寫 `implementation_plan.md`，定義 GPU 排程代碼與 Target 偵測邏輯。
