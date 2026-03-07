---
date: 2026-03-06T19:30:00
type: implementation_plan
---

## 1. 實作目標
為現況「部分完成的 AHD 去馬賽克」加上**防偽像的色差中值過濾 (Color Artifact Removal via Chroma Median Filter)**，消除因像素插值產生的錯誤色彩 (Color Moiré / Zipper artifacts) 並提升整體影像品質。

## 2. 設計邏輯 (十字型 5 點中值濾波)
在使用 `sum_homo_h` 與 `sum_homo_v` 選擇最佳的 R, G, B 插值結果(`demosaic` Function)之後：
1. 提取色差訊號：
   `diff_r(x, y) = demosaic(x, y, 0) - demosaic(x, y, 1)`
   `diff_b(x, y) = demosaic(x, y, 2) - demosaic(x, y, 1)`
2. 在 Halide 定義快速的 5 點十字中值函數 `med5(a, b, c, d, e)` 以取代會造成分支的排序寫法。可以用如下組合：
   ```cpp
   inline Halide::Expr med3(Halide::Expr a, Halide::Expr b, Halide::Expr c) {
       return max(min(a,b), min(max(a,b), c));
   }
   inline Halide::Expr med5(Halide::Expr a, Halide::Expr b, Halide::Expr c, Halide::Expr d, Halide::Expr e) {
       return med3(max(min(a,b), min(c,d)), min(max(a,b), max(c,d)), e);
   }
   ```
3. 取得中心點加上下左右共 5 點的色差值對 `diff_r` 與 `diff_b` 做過濾。
4. 重建 R, B：
   `refined_r(x, y) = clamp(med5_diff_r(x, y) + demosaic(x, y, 1), 0.0f, 1.5f)`
   `refined_b(x, y) = clamp(med5_diff_b(x, y) + demosaic(x, y, 1), 0.0f, 1.5f)`
5. 再將 (`refined_r`, `demosaic(x, y, 1)`, `refined_b`) 傳遞給下一個色彩空間轉換矩陣 `color_corrected`。

## 3. 預計修改檔案
- **`dng_processor/native/src/HalidePipeline.cpp`**
  - 新增 `med3`, `med5` 輔助函式。
  - 在管線階段 `4. Camera -> sRGB matrix` 前插入 `diff_r`, `diff_b` 的計算與中值過濾。
  - 修改 `color_corrected` 參考修復後的 R 與 B。
  - 同步更新檔案頂端的 YAML Index 行號標記。
- **`task.md` / `handover.md`** 
  - 更新當下執行任務為 Phase 5.2 色差過濾。

## 4. 驗證方式
1. 執行 CMake 編譯與單元測試。
2. 確認無編譯錯誤並確保 `demosaic_test` 或整體處理時間無大幅增加（計算仍須具備一定的即時性）。
3. 觀察截圖結果或比對 PSNR，確認新增濾波器沒有造成嚴重效能瓶頸或異常黑白點。
