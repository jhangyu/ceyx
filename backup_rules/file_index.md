# 專案檔案索引目錄 (Project File Index)

這是一份專案的檔案索引目錄，旨在幫助 AI 模型快速了解各重要檔案的位置與功能，避免在執行任務或測試時浪費 Token 進行全局搜索。

## 📍 核心文件與規範 (Documentation & Rules)
這些文件包含了專案的狀態、開發規範與歷史紀錄。
- **`rule.md`**: 開發標準作業程序 (SOP)，包含模型行為準則與日誌格式要求。
- **`task.md`**: 任務清單與待辦事項 (Backlog)，紀錄當前進行中的任務。
- **`handover.md`**: 當前狀態與技術決策，用於交接前後對話的重點摘要。
- **`plan.md`**: 大階段里程碑規畫與概述。
- **`update_code_index_rule.md`**: 大型程式碼檔案的開頭 YAML 索引更新規範。
- **`unit_test.md`**: 單元測試紀錄與說明，包含測試指令與測試目標。
- **`dng_color_metadata_spec.md`**: 色彩與 Metadata 解析相關規格與實驗記錄。

## 📍 開發紀錄與測試資源 (Logs & Testing Assets)
- **`docs/logs/`**: 存放所有依據日期分類的開發紀錄檔 (包含 `summary`, `implementation_plan`, `task_execution`, `walkthrough` 等文件)。
- **`dng_samples/`**: 提供測試用的其他 DNG 與圖檔樣本目錄。
- **`sample.arw`**: 專案根目錄下的原始相機 RAW 檔，作為處理與轉碼的最初始來源。
- **`sample.dng`**: 主要測試用的 DNG 檔案（通常由原始 RAW 檔轉換而來供程式讀取驗證）。
- **`sample.jpg`**: 經過 Lightroom 參數修改並輸出的 JPG 檔案，作為色彩與處理結果的測試參照。
- **`sample2.jpg`**: 完全未套用任何 Lightroom 參數修正直接輸出的 JPG 檔案，作為預設輸出參照。

## 📍 Native C++ 核心模組 (DNG 解析與 Halide 處理)
位於 `dng_processor/native/src/`
- **`DngDecoder.cpp` / `.h`**: 讀取 DNG 檔案，封裝 DNG SDK 進行解壓縮與 Metadata (如矩陣、白平衡) 提取的核心類別。
- **`HalidePipeline.cpp` / `.h`**: 包含所有 Halide 算子，執行去馬賽克、曝光調整、色彩矩陣與非線性 Tone Curve / LUT 校正。
- **`dng_ffi_api.cpp` / `.h`**: 導出 `extern "C"` 的 C-API 介面，供 Flutter 的 ffigen / FFI 綁定呼叫。
- **`dng_xmp_stub.cpp`**: 補齊 DNG SDK 連結中遺失或簡化的 XMP 實作，負責解析 XMP 字串中的 LightRoom 參數 (如 Exposure2012)。

## 📍 Native C++ 測試檔案 (Unit Tests)
位於 `dng_processor/native/tests/`
- **`test_main.cpp`**: C++ 基礎測試進入點。
- **`test_color_accuracy.cpp`**: 用於驗證色彩準確度、PSNR 與解碼流程的測試程式。

> *備註: 測試編譯通常透過 `dng_processor/native/build` 內的 CMake 設定進行。*

## 📍 Flutter & Dart 模組 (UI 與 FFI 綁定)
位於 `dng_processor/lib/` 與 `dng_processor/test/`
- **`dng_processor/lib/main.dart`**: Flutter App 主程式與 UI 進入點。
- **`dng_processor/lib/src/dng_bindings.dart`**: 透過 ffigen 工具生成的 C-API 綁定代碼（通常不需要手動修改）。
- **`dng_processor/lib/src/dng_decoder_service.dart`**: 呼叫 FFI 介面的 Dart Serivce，封裝異步任務、隔離區 (Isolate) 邏輯與錯誤處理。
- **`dng_processor/lib/src/dng_image_widget.dart`**: 將解碼後的 RGBA Buffer 渲染為 Flutter Widget 的 UI 元件。
- **`dng_processor/test/widget_test.dart`**: Flutter 基礎的 Widget 測試。

## 📍 第三方依賴 (Third Party)
位於 `dng_processor/native/third_party/`
- **`dng_sdk/`**: Adobe DNG SDK 原始碼與標頭檔。
- **`halide/`** (若有): Halide 編譯器與 static library 等。

---
> **模型行為規範提示**: 除非絕對必要，請勿在未參考本索引的狀況下盲目執行全局搜索檔案功能 (`find_by_name`, `ls` 等)。請善加利用此檔案進行定位。
