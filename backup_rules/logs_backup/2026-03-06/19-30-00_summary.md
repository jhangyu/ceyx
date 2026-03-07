---
date: 2026-03-06T19:30:00
type: summary
---

## 1. 專案與環境現狀總結
- 專案目前處於 `Phase 5.4 / 5.2` 交界點，根據任務指派，我們開始進行 `Phase 5.2 自適應去馬賽克` 開發。
- `Phase 5.3` (Exposure, Contrast, Saturation, Vibrance) 已全部完成並正確生效 (PSNR 維持在 19.57 dB，表現穩定)。
- 檢視 `HalidePipeline.cpp` 的 `demosaic` 階段：目前已經實作了**AHD（Adaptive Homogeneity-Directed）**的前半部，即水平與垂直方向的亮度雙邊梯度 (Homogeneity) 權重選擇。

## 2. 待解決的問題與目標 (Phase 5.2)
- 目前的 `demosaic` 在算出 R, G, B 後直接轉換色彩空間。
- **缺失的部分**：標準 AHD 演算法的關鍵防偽像 (Anti-aliasing) 步驟——**色差中值濾波 (Chroma Median Filtering)**。沒有中值濾波，在細節密集處容易出現拉鍊效應 (Zipper Artifacts) 與錯誤色彩 (False Colors / Color Moiré)。
- **目標**：在現有 `demosaic` 結果之上加入色差過濾管線。
  1. 計算色差值 `C_r = R - G` 和 `C_b = B - G`。
  2. 對色差套用一個十字型 `5-point Median Filter`，取局部中值平滑色度。
  3. 重建最終的亮度與色彩: `Final R = med_C_r + G`, `Final B = med_C_b + G`。
- 完成實作後將有效完善 Phase 5.2 所要求的 AHD 去馬賽克演算法，可望再度提升 PSNR 或消除邊緣雜色。
