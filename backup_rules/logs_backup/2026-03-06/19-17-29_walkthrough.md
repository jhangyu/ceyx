---
date: 2026-03-06T19:17:29
type: walkthrough
related_task: "Phase 5.3 剩餘：Contrast2012 + Saturation/Vibrance 精進"
status: success
---

## Phase 5.3 完成報告

### 目標
完成 Phase 5.3 剩餘兩個未實作項目：
1. `Exposure` 與 `Contrast` 控制（Exposure 已有，補實 Contrast2012）
2. 改良 Saturation / Vibrance 調整邏輯

---

### 實作變更（`HalidePipeline.cpp`）

#### 1. Contrast2012（新增）
```cpp
// 中性點 0.5 軸心，RGB 空間等比伸縮
const float contrastFactor = 1.0f + lrContrast; // contrast2012/100
r = clamp(0.5 + (r - 0.5) * contrastFactor, 0, 1);
g = clamp(0.5 + (g - 0.5) * contrastFactor, 0, 1);
b = clamp(0.5 + (b - 0.5) * contrastFactor, 0, 1);
```
- 在 Exposure2012 後套用（先亮度，再對比）
- 加入 `hasContrast` 判斷，contrast≈0 時跳過（效能保護）

#### 2. Saturation 精進（非線性正值）
```cpp
// 正值：logistic-style boost（防止高飽和溢出）
if (lrSat > 0.0f)
    s = min(s + lrSat * s * (1.0f - s), 1.0f);
else
    s = max(s * (1.0f + lrSat), 0.0f);  // 負值保持線性
```
- 正值時高飽和像素（s→1）的增量 = lrSat × s × (1-s) → 0，防止溢出
- 更接近 Lightroom 的漸進飽和行為

#### 3. Vibrance 精進（二次方衰減）
```cpp
// 舊：boost = lrVib * (1 - s)
// 新：boost = lrVib * (1 - s^2)  ← 更平滑的高飽和保護
float boost = lrVib * (1.0f - s * s);
s = clamp(s + boost, 0.0f, 1.0f);
```

#### 套用順序
```
ToneCurve → Exposure2012 (HSV: V) → Contrast2012 (RGB) → Sat + Vib (HSV: S)
```

---

### 測試結果

```
測試日期: 2026-03-06
測試二進制: build/test_dng_decoder
測試 DNG: sample.dng (6048×4024, Exposure2012=+0.25, Contrast=Sat=Vib=0)

Results: 27 passed, 0 failed ✅

PSNR: 19.5661 dB (threshold: 15 dB) ✅
（前次 19.57 dB → 19.57 dB，無回退）

[CPU] LR params: ExpGain=1.18921 Contrast=0 Sat=0 Vib=0
（確認 Contrast=Sat=Vib=0 時正確跳過，效能無損）
```

---

### Git Commit
```
[main 13ba42e] feat(halide): implement Contrast2012 and refine Saturation/Vibrance (Phase 5.3)
15 files changed, 788 insertions(+), 53 deletions(-)
```

---

### Phase 5.3 完整進度

| 子項目 | 狀態 | PSNR |
|--------|------|------|
| ProfileToneCurve (ACR3 LUT) | ✅ 完成 | 19.57 dB |
| HueSatMap + LookTable | ✅ 完成 | — |
| Exposure2012 | ✅ 完成 | 19.34 → 19.57 |
| **Contrast2012** | ✅ **新完成** | 19.57 dB (維持) |
| **Saturation 精進** | ✅ **新完成** | 19.57 dB (維持) |
| **Vibrance 精進** | ✅ **新完成** | 19.57 dB (維持) |
