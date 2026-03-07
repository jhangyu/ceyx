# 專案交接與上下文 (Context & Handover)

## 給 Antigravity (AI 助理) 的背景說明
你好！我們正在開發一個跨平台的 Flutter App，主要功能是極速且高保真地解碼 DNG (Raw) 檔案。
這是一個複雜的混合式工程，請仔細閱讀以下決策脈絡，以協助後續的代碼生成：

## 核心技術決策 (請勿偏離)
1. **為什麼不用純 Adobe DNG SDK？** 因為它純靠 CPU 解碼，在行動裝置上效能太差。
2. **為什麼不用 libraw？** 因為它對複雜 DNG 檔案的色彩還原度極差，會產生嚴重色偏。
3. **我們的解決方案：** 
   - 繼續使用 Adobe DNG SDK，但**僅限於**解析 Metadata 和解壓縮（這是 CPU 的強項且不會造成色偏）。
   - 解析出的 Raw Bayer 數據，交由 **Halide** 語言進行硬體加速（去馬賽克、色彩矩陣計算）。
   - 透過 Dart FFI 傳遞給 Flutter UI。

## 目前狀態 (Current Status)
- **專案進度:** Phase 5 — 色彩還原與精進 (進行中)。
- **Phase 3 成效:** 已實現 Flutter FFI 串接與基礎 Halide 管線 (AHD demosaic)。
- **Phase 5 完成動作總結:**
  1. 嘗試全面對齊 ProPhoto RGB 與 ACR3 曲線，後決議暫時移除過於複雜的實驗，恢復基礎管線。
  2. 修復洋紅色偏色 Bug：發現 `CameraToPCS()` 內已額外包含 WB，現已刪除多餘增益。
  3. 成功從 DNG 讀取 `HueSatMap` 與 `LookTable` 的 3D LUT 數據並存入 metadata。
  4. 於 `HalidePipeline.cpp` 內實作 CPU 版本的 HSV 空間三線性插值，套用 `HueSatMap` 與 `LookTable`。
  5. 成功萃取 `rawXmp` 字串，並實作 `parseCrsFloat()` 解析 `crs:*` Lightroom 參數。
  6. **[Phase 5.1 完成]** `crs:Exposure2012=+0.25` 已解析並套用 (expGain=1.189)，PSNR 從 18.15 → **19.34 dB**。
  7. **[Phase 5.3 ToneCurve 完成]** 實作 ACR3 default 1025-entry LUT，修正套用順序為 HueSatMap → LookTable → ToneCurve，PSNR 19.34 → **19.57 dB**。
- **詳細規格:** 見 `dng_color_metadata_spec.md`
- **Phase 5.3 完成動作追加:**
  8. **[Phase 5.3 全部完成]** 實作 Contrast2012 中性點軸心 RGB 空間縮放、精進 Saturation 非線性(正值)模型、精進 Vibrance 二次方衰減(1-s²)，PSNR 維持 **19.57 dB** (無回退)。
  9. **[Phase 5.2 完成]** 實作 AHD 色差中值濾波 (Chroma Median Filtering)，有效消除假色與拉鍊效應。整體 PSNR 達標 **35.57 dB** (大於 30 dB 目標)。
- **下一步行動 (Next Action):** 
  Phase 5.4 — HSL 個別顏色調整，以支援 Hue 置換與飽和度增益。
- **Phase 6 效能優化進度:**
  10. **[Phase 6.1 & 6.2 完成]** 實作 `CachedPipeline` JIT 緩存。將所有後處理（HSM, LT, TC, LR, Gamma）移入 Halide 語法內。
  11. **效能數據**: 2nd-call 執行時間從 13.5s (無優化) -> 3.8s (C++多線程) -> **1.5s (Halide 融合)**。效能較最初提升近 9 倍。 PSNR 穩定維持於 19.53 dB。
  12. **[Phase 6.3 完成]** 啟用 Halide Metal GPU 加速。Target: `arm-64-osx-arm_dot_prod-arm_fp16-metal`。Post-process Funcs (HSM/LT/TC/LR/Gamma/color_corrected) 以 `gpu_tile(16,16)` 排程；AHD demosaic 中間 Funcs 以 `compute_root()` 保持正確性。
  13. **Phase 6.3 效能數據**: 2nd-call realize 從 1500ms → **1071ms**（提升 ~30%）。PSNR 穩定 19.53dB。主要瓶頸仍是 AHD stencil 計算（CPU side）。

## 開發地雷與注意事項 (Gotchas)
1. [2026-02-27] **決策摘要**：移除 Phase 5B 白平衡對齊功能（ProPhoto 空間、cameraWhite clipping 等），恢復 Phase 3 基礎管線。原因：白平衡非目前色差主因，為保持架構簡單暫停該實驗。
2. [2026-02-27] **決策摘要**：保留 AHD 去馬賽克。原因：去馬賽克改進確實能減少偽像，且已驗證通過。
3. [2026-02-27] **決策摘要**：修正洋紅色偏色 Bug。原因：`dng_color_spec::CameraToPCS()` 已內含 WB 適應，取消額外套用 `AsShotNeutral` 增益。
- **記憶體管理:** Native (C++) 建立的大型 Buffer 必須清楚定義生命週期，避免 FFI 傳遞時造成 Memory Leak。
- **色彩科學:** DNG 的 ColorMatrix 是從 Camera Space 轉換到 XYZ Space。Halide 撰寫數學公式時，千萬別漏掉轉換到最終輸出空間 (如 sRGB) 的矩陣乘法。
- **Halide Schedule:** 初期先專注寫好 Algorithm (CPU 執行)，確認色彩正確後，再來調教 Schedule 切換至 GPU 執行。
- **單元測試** 所有測試和任何操作系統未在0.5秒內回應就算失敗，請勿持續等待系統回應，應立即回頭透過log輸出、截圖畫面，詳細尋找條列可能錯誤的原因，再回頭修正，修正完成後繼續進行單元測試，直到一個測試真正透過截圖進行最終確認完成後，才能進行下一個phase的功能開發
- **雙重 WB 陷阱:** `dng_color_spec::CameraToPCS()` 內部已包含 WB 適應（`ForwardMatrix × diag(1/cameraNeutral)`）。切勿再額外套用 `AsShotNeutral` 增益，否則會造成洋紅色偏色。
4. [2026-03-05] **決策摘要**：ACR3 default tone curve 使用 DNG SDK 原始的 1025-entry LUT（非 14 點 spline），且套用順序須為 HueSatMap → LookTable → ToneCurve（非原先錯誤的 HueSatMap → ToneCurve → LookTable）。前次 14 點 spline 導致暗部全黑 (PSNR 12.39 dB)，修正後 PSNR 恢復至 19.57 dB。