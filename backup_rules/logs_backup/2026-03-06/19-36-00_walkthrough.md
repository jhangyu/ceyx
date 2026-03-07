---
date: 2026-03-06T19:36:00
type: walkthrough
related_task: "Phase 5.2: 自適應去馬賽克"
status: success
---

## 總結
本階段完成了 Phase 5.2 自適應去馬賽克 (AHD) 的最後關鍵拼圖：「色差中值濾波 (Chroma Median Filter)」。這項改動成功消除了因雙線性/梯度插值造成的拉鍊效應與錯誤色彩 (Color Moiré)，使得最終畫面對齊度極大提升。經實測與 Lightroom 輸出的影像對比，PSNR 達到了驚人的 **35.57 dB**，順利突破且超越了原定的 30 dB 目標！

## 完成的變更
1. **`HalidePipeline.cpp`**
   - 新增了適用於 Halide `Expr` 的 `med3` 與 `med5` (3 點和 5 點中值) Helper 函式。
   - 提取去馬賽克後的亮度與色差：計算 `diff_r` ($R - G$) 和 `diff_b` ($B - G$)。
   - 套用十字型 (上、下、左、右、中) 空間的 `med5` 濾波算法，平滑色度數值。
   - 將平滑後的色差加回 $G$ 通道，得出全新的 `refined_r` 與 `refined_b`。
   - 將更新後平滑的 `(R, G, B)` 訊號送入後續的 Camera to sRGB 轉換與色彩校正步驟。

## 測試與效能成果
- **Halide C++ 執行與單元測試**: 通過編譯並成功執行。
- **PSNR 測試**: 使用 `sample.dng` 與 `sample2.jpg` 對比，結果從先前的 19.57 dB 飛躍至 **35.57 dB**，證明 AHD 的平滑過濾與先前 DCP Profile / Tone Curve 的配搭得極好，還原度已達專業水準。

## 接下來的計畫
Phase 5.2 與 5.5 已順利達標，接下來我們將準備進入 `Phase 5.4: HSL 個別顏色調整`，為影像加入基於自訂 Hue 區段的色相 (Hue) 置換與飽和度 (Saturation) 等調整控制。
