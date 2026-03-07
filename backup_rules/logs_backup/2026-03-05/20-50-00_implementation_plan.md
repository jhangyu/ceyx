---
date: 2026-03-05T20:50:00
type: implementation_plan
related_task: "Phase 5.3 Halide 色彩管線增強 — ProfileToneCurve 修正"
status: in_progress
---

## 目標
修正 `HalidePipeline.cpp` 中兩個致命 Bug，使 ToneCurve 能正確編譯並以正確順序套用。

---

## 背景
上一次對話中已完成：
- 513-entry ACR3 LUT (`kAcrDefaultLUT[513]`) 嵌入
- `evalAcrLUT()` 線性插值函式
- `applyToneCurveBuffer()` 雙路徑邏輯（profile curve vs ACR LUT）

但兩個關鍵 Bug 導致編譯失敗及 PSNR 驟降：

### Bug 1: 變數名稱不一致（編譯錯誤）
Lines 621-626 引用了已移除的 `kAcrDefaultCurve` / `kAcrDefaultCurveCount`。
但 `applyToneCurveBuffer()` 已內建 ACR LUT fallback — 當 `pts == nullptr && count == 0` 時自動使用 `evalAcrLUT()`。

### Bug 2: 套用順序錯誤
當前：HueSatMap → **ToneCurve** → LookTable
DNG SDK 正確順序：HueSatMap → LookTable → **ToneCurve**

---

## Proposed Changes

### [MODIFY] [HalidePipeline.cpp](file:///Users/jhangyu/Documents/flutter_dng_decoder/dng_processor/native/src/HalidePipeline.cpp)

#### 修改 1: 刪除 Step 5d 區塊（lines 616-631）
將目前位於 HueSatMap 與 LookTable 之間的 ToneCurve 程式區塊整段刪除。

#### 修改 2: 在 LookTable 之後（line 640 後）插入新的 ToneCurve 區塊
```cpp
// 5d. ProfileToneCurve channel-wise mapping (Phase 5.3)
// DNG SDK order: HueSatMap → LookTable → ToneCurve
{
    const double *tcPts = (metadata.toneCurveCount > 0)
                              ? metadata.toneCurvePoints : nullptr;
    const int tcCount = (metadata.toneCurveCount > 0)
                            ? (int)metadata.toneCurveCount : 0;
    std::cerr << "[CPU] Applying ToneCurve: "
              << ((metadata.toneCurveCount > 0) ? "Profile" : "ACR default 513-LUT")
              << "\n";
    applyToneCurveBuffer(rgbFlat.data(), width, height, tcPts, tcCount);
}
```

#### 修改 3: 更新管線階段註解（lines 7-19）
更新頂部的 pipeline stages 註解，反映正確的順序。

---

## Verification Plan

### 自動測試
```bash
cd /Users/jhangyu/Documents/flutter_dng_decoder/dng_processor/native/build
make -j4 test_dng_decoder 2>&1 | tail -30
./test_dng_decoder /Users/jhangyu/Documents/flutter_dng_decoder/sample.dng 2>&1 | grep -E "PASS|FAIL|PSNR|Tone|tone"
```

### 驗收條件
1. 編譯成功（無 `kAcrDefaultCurve` 未宣告錯誤）
2. 所有既有測試 PASS（無 regression）
3. PSNR ≥ 15 dB（Test 6.5 通過）
4. debug 輸出確認 `[CPU] Applying ToneCurve: ACR default 513-LUT`

---
*此文件於 2026-03-05T20:50:00 撰寫，在任何程式碼修改之前完成。*
