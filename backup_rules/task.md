# 開發任務列表 (Task Backlog)

## 🔴 現在進行中 (ACTIVE)
- **Task**: Phase 6.4: 記憶體零拷貝 (Zero-copy) 機制
- **Step**: 6.4.1 [環境掃描] — 規劃 Zero-copy 實作策略
- **Execution Log**: 待建立
- **中斷點**: Phase 6.3 Metal GPU 加速已完成，PSNR=19.53dB，2nd-call 1071ms。準備進行 Phase 6.4 記憶體零拷貝優化。

## Phase 1: 環境配置 (Environment Setup)
- [x] 1.1 建立 Flutter 專案 (`flutter create dng_processor`).
- [x] 1.2 在專案內建立 `native` 資料夾，設定基礎的 `CMakeLists.txt`。
- [x] 1.3 下載並編譯 Adobe DNG SDK，將靜態庫/動態庫連結至 CMake。
- [x] 1.4 下載並配置 Halide 編譯環境 (建議從 Halide Releases 取得 pre-built binaries)。
- [x] 1.5 **[補充] 測試看門狗 (Watchdog) 設定:** 配置自動測試腳本與截圖工具，準備後續自動檢查 0.5s 回應限制的測試環境。

## Phase 2: Native DNG 解析 (C++)
- [x] 2.1 撰寫 C++ 介面 `DngDecoder`。
- [x] 2.2 使用 DNG SDK 讀取測試用 `.dng` 檔案。
- [x] 2.3 提取並儲存關鍵 Metadata：
    - `BlackLevel`, `WhiteLevel`
    - `ColorMatrix1`, `ColorMatrix2`, `ForwardMatrix`
    - `AsShotNeutral` (白平衡)
- [x] 2.4 呼叫 DNG SDK 解壓縮，獲取 16-bit 灰階 Bayer Raw Buffer。
- [x] 2.5 **[補充] C-API 錯誤回報:** 實作 `try-catch` 攔截區，將 DNG SDK 錯誤封裝成結構體或錯誤代碼輸出。

## Phase 3: Halide 影像管線開發 (C++)
- [x] 3.1 建立 Halide Generator。
- [x] 3.2 實作：黑階扣除與線性化 (Black Level Subtraction & Linearization)。
- [x] 3.3 實作：去馬賽克 (Demosaicing) - 雙線性插值 (Bilinear)。
- [x] 3.4 實作：矩陣運算 (套用白平衡與色彩矩陣轉換至 sRGB) + BaselineExposure 曝光補償。
- [x] 3.5 實作：Gamma 校正並輸出 8-bit RGBA Buffer。
- [x] 3.6 使用 Halide CPU Schedule 進行單元測試，PSNR=18.43dB (>15dB 基礎管線閾值)。

## Phase 4: Flutter FFI 整合
- [x] 4.1 撰寫 C-API (extern "C") 包裝 `DngDecoder` 供 Dart 呼叫。
- [x] 4.2 在 Flutter 中使用 `ffigen` 或手動撰寫 `dart:ffi` 綁定。
- [x] 4.3 將 C++ 輸出的 RGBA Buffer 轉換為 Flutter `ui.Image`。
- [/] 4.4 **[補充] 記憶體回收綁定:** 經由 Dart `NativeFinalizer` 綁定 Buffer 的 `free()` 原生呼叫，實現零拷貝及自動垃圾回收。 (目前為記憶體拷貝，須改為 NativeFinalizer 零拷貝)
- [/] 4.5 建立簡單的 UI：選擇圖片按鈕、顯示解碼時間、顯示圖片並實作自動截圖以利失敗調查。 (已建立基礎 UI，但尚未實作自動截圖以利失敗調查)

## Phase 5: 色彩還原與精進 (Color Restoration & Refinement) [[Summary]](docs/logs/2026-03-06/18-52-15_summary.md)
- [/] 5.1 **進階 Metadata 提取:**
    - [x] 讀取 `Exposure2012`, `Saturation`, `Vibrance` 標籤 (透過 XMP parseCrsFloat() 字串搜尋解析)。PSNR: 19.34 dB ✅
    - [x] 提取 `DCP Profile` (HueSatMap, LookTable) 與 `rawXmp` 字串。
    - [ ] 提取個別 HSL 調整參數。
- [x] 5.2 **自適應去馬賽克 (Adaptive Demosaicing):** 將雙線性插值升級為 AHD / LMMSE 演算法。 (全整 AHD 與色差中值濾波皆已實作完成) ✅
- [x] 5.3 **Halide 色彩管線增強:**
    - [x] 實作精確的 `ProfileToneCurve` 映射。PSNR: 19.57 dB ✅ (ACR3 1025-entry LUT + 正確套用順序)
    - [x] 加入 `Exposure` 與 `Contrast` 控制。(Exposure=2^EV 乘以 V；Contrast2012 中性點軸 0.5 RGB 空間等比伸縮) ✅
    - [x] 實作改良的色彩飽和度與鮮艷度 (Vibrance) 調整邏輯。(Sat非線性正値+線性負値；Vib二次方衰減) ✅
    - [x] 套用 `HueSatMap` 與 `LookTable` 實現非線性色彩校正 (目前採取 CPU 端三線性插值實作)。
- [ ] 5.4 **HSL 個別顏色調整:** 實作 Halide 基於 Hue 的色彩偏移與增益。
- [x] 5.5 **PSNR 目標提升:** 經由上述實作，使 PSNR 提升至 **>30dB**。 (已達 35.57 dB) ✅

## Phase 6: 效能最佳化與產品化
- [x] 6.1 修改 Halide Schedule，啟用 CPU 排程最佳化 (Tiling, Parallel, Vectorize)。 ✅
- [x] 6.2 C++ 後處理效能瓶頸優化: 已將 HSM/LT/TC/LR/Gamma 遷移至 Halide 實現運算融合 ✅ [Walkthrough](docs/logs/2026-03-06/22-05-00_walkthrough.md)
- [x] 6.3 啟用 Halide GPU 加速 (Metal/Vulkan)。 ✅ PSNR=19.53dB, 2nd-call 1071ms [Walkthrough](docs/logs/2026-03-06/22-20-00_walkthrough.md)
- [ ] 6.4 測試效能瓶頸，實作記憶體零拷貝 (Zero-copy) 機制。
- [ ] 6.5 壓力測試與邊際案例 (如損壞的 DNG) 處理。

## Chore: 維護與架構優化
- [x] **檔案 YAML 索引**: 在各個大型檔案開頭加入 YAML-like 的 `modules` 索引，標注範圍行號，方便 AI 閱讀。
- [x] **程式碼索引規範**: 建立 update_code_index_rule.md 並更新 rule.md 及 HalidePipeline.cpp 的索引 ✅
- [x] **單元測試索引**: 在 `unit_test.md` 加入測試檔案位置記錄。 ✅
    - [x] `DngDecoder.cpp`
    - [x] `HalidePipeline.cpp`
    - [x] `main.dart`
    - [x] `dng_decoder_service.dart` / `dng_bindings.dart`