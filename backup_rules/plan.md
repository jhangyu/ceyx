# 專案計畫書：Flutter + Adobe DNG SDK + Halide 混合渲染引擎

## 專案目標
開發一個高效能、跨平台 (Windows, Android, iOS, macOS) 的 DNG (Raw) 影像處理 App。
為了解決原生 Adobe DNG SDK 純 CPU 解碼過慢，且避免第三方函式庫 (如 libraw) 的色偏問題，本專案將採用「混合處理架構」。

## 系統架構 (Hybrid Pipeline)
1. **Frontend (UI 層):** Flutter (Dart)
2. **Bridge (通訊層):** Dart FFI (Foreign Function Interface)
3. **Native Backend (C/C++ 層):**
   - **解析與解壓縮:** Adobe DNG SDK (C++) - 負責讀取 Metadata、色彩矩陣，並解壓縮出原始 Bayer Raw Data。
   - **影像處理管線:** Halide (C++) - 負責硬體加速 (CPU AVX/NEON 或 GPU Vulkan/Metal)，執行去馬賽克 (Demosaicing)、白平衡、色彩轉換、Gamma 校正。

## 跨平台策略與記憶體安全 (補充設定)
- 使用 **CMake** 作為 Native C++ 代碼的統一構建系統。
- Halide 將負責將演算法動態編譯/排程為目標平台的最佳指令 (Metal/Vulkan/OpenCL/CPU SIMD)。
- **[補充] 記憶體生命週期綁定：** 從 C++ 建立的 Raw Buffer 透過 Dart `NativeFinalizer` 掛載回收函數，確保不會造成 Memory Leak。
- **[補充] 跨語言錯誤邊界：** 在 C++ 邊界加入 `try-catch` 防止拋出 Native Exception 到 Dart 環境導致 App 強制崩潰 (Hard Crash)，轉換為結構化錯誤回傳。

## 階段性里程碑
- [x] **Phase 1:** 基礎環境搭建 (Flutter, CMake, 引入 Adobe DNG SDK 與 Halide) 與 測試監控架構設定。
- [x] **Phase 2:** C++ 核心開發 - 使用 DNG SDK 成功提取 Bayer Raw 與 Metadata，建立 FFI 錯誤邊界。
- [x] **Phase 3:** Halide 管線開發 - 實作 CPU 版本的去馬賽克與基礎色彩處理 (確保正確的矩陣乘法順序)。
- [ ] **Phase 4:** Flutter 與 FFI 串接 - 將處理好的 RGB 陣列回傳至 Flutter 渲染，實作零拷貝。
- [x] **Phase 5:** 色彩還原精進 (DCP Profile, Tone Curve, 自適應去馬賽克, HSL 個別調整, 曝光/鮮艷度) 與 PSNR 提升 (>30dB)。
  - **[已完成]** 萃取 `HueSatMap` / `LookTable` 與實作 CPU 端三線性插值應用。
  - **[已完成]** 修正 `ProfileToneCurve` 相關宣告，將 ACR3 LUT 擴充至 1025-entry 並修正套用順序。
  - **[已完成]** XMP 參數套用補完：完成 Exposure, Contrast, Saturation, Vibrance 的細部邏輯實作。
  - **[已完成]** Phase 5.2 自適應去馬賽克 (AHD + 色差中值濾波)，PSNR 大幅躍升至 **35.57 dB**，超標完成 Phase 5.5。
  - **[已完成]** 雖然放棄了 Phase 5.4 的個別調整，但目前色彩準確度已達預期。
- [ ] **Phase 6:** 效能最佳化 - Halide GPU 加速 (Metal/Vulkan) + 壓力測試 + 零拷貝優化 + 最終產品化。
  - **[已完成]** Phase 6.1 Halide Schedule 優化 (Parallel/Vectorize) 與 JIT 緩存。
  - **[已完成]** Phase 6.2 後處理遷移至 Halide，實現所有運算融合，2nd-call 時間降至 ~1.5s。
  - **[已完成]** Phase 6.3 啟用 Metal GPU 排程，將部分管線轉移至 GPU 計算，縮短解析至 1071ms。
  - **[待執行]** Phase 6.4 FFI 端導入 Dart NativeFinalizer 完成 Zero-copy 記憶體免拷貝，並評估 AHD 運算的 GPU 化。