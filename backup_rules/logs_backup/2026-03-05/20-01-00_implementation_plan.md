---
date: 2026-03-05T20:01:00
type: implementation_plan
related_task: "Phase 5.3 Halide 色彩管線增強 — ProfileToneCurve 映射"
status: in_progress
---

## 目標
在 `HalidePipeline.cpp` 的 CPU 後處理區段，於 HueSatMap 之後、LookTable 之前，加入 **ProfileToneCurve** 的分段線性映射。

這是目前 PSNR 落差的最大貢獻源之一（預計 +3–6 dB）。

---

## 背景知識

### ProfileToneCurve 格式
- `DngMetadata.toneCurvePoints[256]`：交錯式 `(input, output)` 控制點，`[0, 1]` 範圍
- `DngMetadata.toneCurveCount`：控制點對數（0 代表無 Profile 曲線）
- 控制點 `(x, y)` 表示：像素線性亮度 `x` 映射到曲線輸出亮度 `y`

### 套用模式
1. **有 ProfileToneCurve**（`toneCurveCount > 0`）：分段線性插值套用
2. **無 ProfileToneCurve**（`toneCurveCount == 0`）：使用 Adobe ACR 預設曲線（Medium Contrast），控制點：
   ```
   (0.0, 0.0), (0.0, 0.0), (0.053, 0.0), (0.1, 0.034), (0.15, 0.076),
   (0.2, 0.131), (0.25, 0.194), (0.3, 0.256), (0.4, 0.375),
   (0.5, 0.489), (0.6, 0.607), (0.7, 0.720), (0.8, 0.840),
   (0.9, 0.940), (1.0, 1.0)
   ```

### 套用空間
DNG SDK 的 BaselineRGBTone 是在 **線性光 sRGB** 空間的 **亮度通道** 工作。實作上，將 sRGB 三個通道的 `max(R,G,B)` (value) 或各通道獨立套用曲線均可。

**選擇策略**：套用於每個通道獨立（channel-wise），與 DNG SDK `dng_render` 行為最接近，也最簡單。

---

## Proposed Changes

---

### C++ Native Layer

#### [MODIFY] [HalidePipeline.cpp](file:///Users/jhangyu/Documents/flutter_dng_decoder/dng_processor/native/src/HalidePipeline.cpp)

**新增 `applyToneCurve()` 靜態函式**（加入 applyHueSatMap 函式之後的 anonymous namespace）：

```cpp
// 分段線性插值 tone curve 映射，套用到單一 float 值 [0,1]
static float evalToneCurve(const double *pts, int count, float x) {
  // pts 為交錯 (input, output) 對，count 為對數
  if (count == 0) return x;  // identity fallback
  if (x <= (float)pts[0]) return (float)pts[1];
  if (x >= (float)pts[(count-1)*2]) return (float)pts[(count-1)*2+1];
  for (int i = 0; i < count-1; i++) {
    float x0 = (float)pts[i*2], y0 = (float)pts[i*2+1];
    float x1 = (float)pts[(i+1)*2], y1 = (float)pts[(i+1)*2+1];
    if (x <= x1) {
      float t = (x - x0) / (x1 - x0);
      return y0 + t * (y1 - y0);
    }
  }
  return (float)pts[(count-1)*2+1];  // clamp to last
}

// ACR 預設曲線控制點（Medium Contrast）
static const double kAcrDefaultCurve[] = {
  0.0,   0.0,
  0.053, 0.0,
  0.1,   0.034,
  0.15,  0.076,
  0.2,   0.131,
  0.25,  0.194,
  0.3,   0.256,
  0.4,   0.375,
  0.5,   0.489,
  0.6,   0.607,
  0.7,   0.720,
  0.8,   0.840,
  0.9,   0.940,
  1.0,   1.0
};
static const int kAcrDefaultCurveCount = 14;

// 套用 tone curve 到 rgb float buffer（channel-wise）
void applyToneCurveBuffer(float *rgb, int width, int height,
                          const double *pts, int count) {
  for (int i = 0; i < width * height * 3; i++) {
    rgb[i] = std::min(std::max(evalToneCurve(pts, count, rgb[i]), 0.0f), 1.0f);
  }
}
```

**修改 `HalidePipeline::process()`**，在 `hasHSM` / `hasLT` 處理後，插入 Step 5d：

```cpp
// 5d. ProfileToneCurve (Phase 5.3)
// 套用時機：HueSatMap 之後，LookTable 之前
const double *tcPts  = (metadata.toneCurveCount > 0)
    ? metadata.toneCurvePoints : kAcrDefaultCurve;
const int tcCount    = (metadata.toneCurveCount > 0)
    ? (int)metadata.toneCurveCount : kAcrDefaultCurveCount;
applyToneCurveBuffer(rgbFlat.data(), width, height, tcPts, tcCount);
```

**插入位置**：`applyHueSatMap(HSM)` 之後，但 `applyHueSatMap(LT)` 之前。

---

### Test Layer

#### [MODIFY] [test_main.cpp](file:///Users/jhangyu/Documents/flutter_dng_decoder/dng_processor/native/tests/test_main.cpp)

新增測試點（在 Test 6.0 區塊附近）：
- **Test 6.0b**: `toneCurveCount` 印出（確認提取到的控制點數）
- **Test 6.0c**: 若 `toneCurveCount == 0` 使用 ACR 預設曲線 — 輸出確認訊息
- PSNR 比對仍維持 >15 dB，期待提升

---

## Verification Plan

### 測試命令
```bash
cd /Users/jhangyu/Documents/flutter_dng_decoder/dng_processor/native/build
make -j4 test_dng_decoder 2>&1 | tail -30
./test_dng_decoder /Users/jhangyu/Documents/flutter_dng_decoder/sample.dng 2>&1 | grep -E "PASS|FAIL|PSNR|Tone|tone"
```

### 驗收條件
1. 所有 27 個既有測試仍為 PASS（無 regression）
2. PSNR 從 19.34 dB 提升（预計 +2~5 dB）
3. 無 out-of-bounds crash（toneCurveCount=0 時使用 ACR 預設曲線）
4. debug 輸出確認 `[CPU] Applying ToneCurve ...` 訊息

---

## 風險評估
- **低風險**：evalToneCurve 是純數學映射，不影響記憶體佈局
- **中風險**：ACR 預設曲線控制點的選取可能與 DNG SDK 實際曲線不完全一致，需靠 PSNR 驗證
- **套用順序**：按照 DNG SDK 參考流程（HueSatMap → ToneCurve → LookTable），正確

---
*此文件於 2026-03-05T20:01:00 撰寫，在任何程式碼修改之前完成，遵守 rule.md SOP 步驟 2。*
