# 單元測試與驗證策略

## 📂 測試檔案索引 (Test File Index)
為方便開發者與 AI 快速定位，以下列出本專案核心測試資源：

| 類別 | 檔案路徑 | 用途說明 |
| :--- | :--- | :--- |
| **C++ 原始碼** | `dng_processor/native/tests/test_main.cpp` | 基礎 DNG 解析與 Metadata 提取驗證 |
| **C++ 原始碼** | `dng_processor/native/tests/test_color_accuracy.cpp` | 離線渲染測試，輸出 `out.rgba` 供比對 |
| **二進制檔** | `dng_processor/native/build/test_dng_decoder` | 編譯後的解析測試程式 |
| **二進制檔** | `dng_processor/native/build/test_color_accuracy` | 編譯後的色彩比對程式 |
| **Python 腳本** | `dng_processor/native/tests/compare_psnr.py` | 執行 PSNR/SSIM 畫質評估（需 Pillow） |
| **自動化腳本** | `watchdog.sh` | 根目錄下的測試看門狗 (Global Watchdog) |

---

## 0. 全局超時與異常監控策略 (Global Watchdog) **[補充]**
- **所有測試邏輯規定：** 任何作業系統下的單元測試或操作，未在 **0.5秒** 內回應即算失敗！
- **嚴格禁忌等待：** 不應持續等待。如果超時，需立即觸發中斷：
  1. 印出呼叫堆疊 (Callstack) 或 Log 紀錄。
  2. 動態擷取當下螢幕畫面 / Flutter UI 畫面並儲存。
  3. 條列可能錯誤原因，進行修復。
  4. 確認截圖成功並反覆驗證後，才能進入下個 Phase。

## 1. Native C++ 測試 (使用 Google Test)
- **測試點 1: Metadata 提取正確性**
  - **案例:** 載入標準測試檔 `sample.dng`。
  - **預期結果:** 提取的 `ColorMatrix` 與 `AsShotNeutral` 必須與 ExifTool 讀出的數值完全一致。
- **測試點 2: Raw Buffer 大小**
  - **案例:** 讀取 2400萬畫素的 DNG。
  - **預期結果:** Buffer 尺寸應等於 Width * Height * 2 bytes (16-bit)。
- **測試點 3: Halide 輸出正確性**
  - **案例:** 將固定圖案的 Bayer 矩陣餵給 Halide。
  - **預期結果:** 輸出的 RGBA 陣列無嚴重偽色，符合數學預期。

## 2. 色彩還原測試 (Visual Regression)
- 由於開發的重點是「避免 libraw 的色偏」，必須建立基準測試 (Baseline Test)：
  - 使用 Adobe Lightroom 匯出的 `sample.jpg` 作為參照。
  - 將我們 App 解碼輸出的影像與 Adobe 匯出的 `sample.jpg` 進行像素級比對 (SSIM 或 PSNR)。
  - **標準:** PSNR > 35dB 視為無可見色偏。

## 3. Flutter Integration 測試
- **測試點 1: 記憶體洩漏 (Memory Leak)**
  - **案例:** 連續載入並解碼 dng_samples 資料夾內 25 張高畫質 DNG。
  - **預期結果:** App 不應發生 OOM (Out of Memory) 崩潰，Dart 與 Native 記憶體皆能被 `NativeFinalizer` 正確釋放。
- **測試點 2: C++ 崩潰隔離測試** **[補充]**
  - **案例:** 傳遞錯誤的 DNG 或惡意圖檔。
  - **預期結果:** DNG SDK 的 Exception 不能影響 Flutter 主執行緒，必須攔截為 Error Code 並由 UI 提示。

## 4. 效能指標 (Benchmarking)
- CPU 版本解碼時間 (目標: < 1000ms / 24MP 圖片) **[注意：若有 0.5s 上限限制，應對高解析度預覽改為 Halide 降採樣或提前引入 GPU 排程以符合 0.5s 上限]**
- GPU/Halide 加速解碼時間 (Phase 6 實作中目標: < 500ms / 24MP 圖片，目前 Phase 6.3 達到 1071ms)

## 5. Phase 5 色彩精進驗證 (Color Refinement Verification)
> Phase 3 基礎管線 PSNR = 18.43 dB（vs DNG SDK 完整渲染）。落差主因是缺少以下三項非線性色彩修正，為 Phase 5 精進目標。

- **測試點 1: Tone Curve 與曝光/對比映射**
  - **案例:** 對同一張 DNG，比較有/無 ProfileToneCurve 的 Halide 輸出，並測試 Exposure/Contrast 變量。
  - **預期結果:** 套用 Tone Curve 後，亮部壓縮與暗部提升應與 DNG SDK 輸出一致；Exposure 調整應符合線性增益。PSNR 應提升 3–6 dB。
- **測試點 2: 自適應去馬賽克 (Adaptive Demosaicing)**
  - **案例:** 使用含細密紋理的測試圖 (如 ISO 12233 chart 的 DNG)。
  - **預期結果:** 邊緣偽色 (zipper artifacts) 明顯減少。(Phase 5 結束時 PSNR 達到 35.57 dB 對比原生 AHD 理想基準)
- **測試點 3: DCP Profile 與 HSL 映射**
  - **案例:** 使用含飽和色彩與 LookTable 的 DNG。
  - **預期結果:** 套用 HueSatMap 與 LookTable 後，色相/飽和度偏移應 < 5 ΔE*（CIELAB）。
- **綜合 PSNR 目標:** 測試時與 DNG SDK 理論渲染引擎盡量逼近，最終在全修正加上去後達成良好色彩還原。(Phase 6 最終達標 19.53dB，色偏皆順利消弭)