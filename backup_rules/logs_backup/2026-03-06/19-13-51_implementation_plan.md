---
date: 2026-03-06T19:13:51
type: implementation_plan
related_task: "Phase 5.3 剩餘：Contrast2012 + Saturation/Vibrance 精進"
status: in_progress
---

## 目標

實作 Phase 5.3 剩餘兩項：
1. **Contrast2012 控制**：套用 Lightroom `crs:Contrast2012` 對比度調整。
2. **Saturation/Vibrance 精進**：改良現有 `crs:Saturation` 和 `crs:Vibrance` 邏輯，使其更接近 Lightroom 行為。

---

## 修改方案

### [MODIFY] HalidePipeline.cpp

**位置：** `HalidePipeline.cpp` 第 661–720 行（Phase 5.1 LR params block）

#### 問題 1：Contrast2012 未套用
`lrParams.contrast2012` 已解析但完全未使用。

**Lightroom Contrast2012 數學模型：**
- Lightroom 的 Contrast 以 `0.5` 為軸心（在 tone-curve 空間），拉伸或壓縮像素值。
- 我們已通過 ToneCurve 完成了基礎影調，Contrast 應在 ToneCurve 後、Exposure 補償前套用到每個 RGB 通道值。
- 線性近似模型（足夠準確）：
  ```
  contrast_factor = 1.0 + contrast2012 / 100.0
  v_out = clamp(0.5 + (v_in - 0.5) * contrast_factor, 0, 1)
  ```
- 套用順序：ToneCurve 後 → Exposure2012 前（或同步套用更精確）。
  
  實際上我們會在 LR params 處理迴圈中，在 Exposure2012 後立即套用 Contrast，對每個 RGB 通道值操作（非 HSV 空間）。

#### 問題 2：Saturation 精進
當前公式：`s * (1 + lrSat)` 

問題：對高飽和像素（s 接近 1）仍會線性縮放，可能造成 clamp 失真。

**改良公式：**
使用更接近 Lightroom 的非線性 Saturation 模型：
```
// Lightroom S 調整：弱飽和 → 更大影響，高飽和 → 較小影響
if (lrSat > 0) {
  s = clamp(s + lrSat * s * (1.0f - s), 0, 1);  // 正值：S型增強
} else {
  s = clamp(s * (1.0f + lrSat), 0, 1);           // 負值：線性減弱
}
```

#### 問題 3：Vibrance 精進
當前公式：`boost = lrVib * (1 - s)`（一次方衰減）

**改良：使用二次方衰減**:
```
// boost 隨已有飽和度二次衰減 → 低飽和像素獲得更大提升
float boost = lrVib * (1.0f - s * s);  // 二次方比一次方更平滑
s = clamp(s + boost, 0, 1);
```

#### 套用順序（重要）
在 LR params 處理迴圈中，調整為：
1. Convert RGB → HSV
2. Apply Exposure2012 (on V)
3. **(NEW)** Apply Contrast2012 to each RGB channel SEPARATELY  
   → 注意：Contrast 在 linear RGB space 操作，非 HSV
4. Apply Saturation (refined on S)
5. Apply Vibrance (refined on S)
6. Convert HSV → RGB

由於 Contrast 操作 RGB 而非 HSV，需要在 HSV-to-RGB 轉換後再對 RGB 套用 Contrast，然後重新回 HSV 繼續：

```
// 最終流程：
// 1. rgb2hsv → apply Exposure → hsv2rgb
// 2. apply Contrast on RGB
// 3. rgb2hsv → apply Sat/Vib → hsv2rgb
```

但為避免兩次 HSV 轉換的開銷及精度損失，採用以下統一流程：
```
// 在一次 pixel loop 中：
// a. rgb2hsv
// b. Apply Exposure on V
// c. hsv2rgb (轉回 RGB)
// d. Apply Contrast on RGB (0.5為軸心)
// e. rgb2hsv (重新轉) 
// f. Apply Sat/Vib on S
// g. hsv2rgb (最終輸出)
```

**優化：** 若 contrast 接近 0（|contrast2012| < 0.5），跳過 contrast+extra-hsv 轉換，維持原有效能。

#### YAML 索引更新
修改完成後，需更新 `HalidePipeline.cpp` 開頭的 YAML 索引中 Phase 5c 的描述。

---

## 驗證計畫

### 自動化測試

**測試二進制已存在於：**
```
/Users/jhangyu/Documents/flutter_dng_decoder/dng_processor/native/build/
```

**步驟 1：重新編譯**
```bash
cd /Users/jhangyu/Documents/flutter_dng_decoder/dng_processor/native/build
cmake --build . --target test_dng_decoder 2>&1 | tail -20
```

**步驟 2：尋找測試 DNG 檔案**
```bash
ls /Users/jhangyu/Documents/flutter_dng_decoder/dng_processor/native/tests/
```

**步驟 3：執行測試**
```bash
cd /Users/jhangyu/Documents/flutter_dng_decoder/dng_processor/native/build
./test_dng_decoder <path_to_test_dng>
```

**預期結果：**
- 所有 PASS/FAIL 中，Test 6.5 PSNR 應 ≥ 19.0 dB（不應回退超過 0.5 dB）
- 若 Contrast2012 = 0（DNG 預設），PSNR 應基本不變
- 若 Contrast 有非零值，可能有 ±1 dB 的 PSNR 變化（即使 Contrast 改善視覺效果，PSNR 對比的是 DNG SDK 預設輸出）

### 回退驗證
- Contrast2012 = 0 且 Saturation = 0 且 Vibrance = 0 時，行為應與修改前 **完全相同**
- 確認程式碼加入 `hasContrast` 判斷，當 contrast2012 ≈ 0 時跳過處理
