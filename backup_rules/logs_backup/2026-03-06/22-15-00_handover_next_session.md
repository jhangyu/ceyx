---
date: 2026-03-06T22:15:00
type: other
related_task: "Phase 6.4: 效能瓶頸分析與 Zero-copy 機制"
status: in_progress
---

# Phase 6.4 完整交接文件 (Next Session Handover)

> **用途**：上下文移交給下一個 Session 時使用。包含所有必要的背景、斷點位置、以及所有相關 Markdown 文件的索引。

---

## 🔴 快速定位：斷點在哪裡？

**任務**：Phase 6.4 — 效能瓶頸分析 + Zero-copy 機制  
**狀態**：計畫已完成，**尚未開始修改程式碼**  
**下一步行動**：開始 Step 6.4.1，修改 `HalidePipeline.cpp` 的 `process()` 函數，加入詳細分段計時 log。

**需要修改的第一個檔案**：
```
/Users/jhangyu/Documents/flutter_dng_decoder/dng_processor/native/src/HalidePipeline.cpp
```
目標位置：`HalidePipeline::process()` 函數（Line 992 附近）

---

## 📁 Markdown 文件索引

### 核心文件 (必讀)
| 文件 | 路徑 | 用途 |
|------|------|------|
| **rule.md** | `rule.md` | SOP 規範，每次開新 session 必讀 |
| **task.md** | `task.md` | 任務清單，頂部 ACTIVE 指針 |
| **handover.md** | `handover.md` | 當前技術決策與狀態 |
| **plan.md** | `plan.md` | 大階段里程碑 |

### Phase 6.4 相關文件
| 文件 | 路徑 | 用途 |
|------|------|------|
| **Phase 6.4 Summary** | `docs/logs/2026-03-06/22-12-00_summary.md` | 現狀總結、瓶頸分析 |
| **Phase 6.4 Plan** | `docs/logs/2026-03-06/22-13-00_implementation_plan.md` | 詳細實作計畫 ⭐ |
| **Phase 6.4 Execution** | `docs/logs/2026-03-06/22-14-00_task_execution.md` | 執行日誌（需持續更新） |
| **本交接文件** | `docs/logs/2026-03-06/22-15-00_handover_next_session.md` | 本文件 |

### Phase 6.3 完成文件 (參考)
| 文件 | 路徑 | 內容摘要 |
|------|------|---------|
| **Phase 6.3 Summary** | `docs/logs/2026-03-06/21-58-00_summary.md` | GPU 加速現狀分析 |
| **Phase 6.3 Plan** | `docs/logs/2026-03-06/22-15-00_implementation_plan.md` | GPU 實作計畫（已完成） |
| **Phase 6.3 Execution** | `docs/logs/2026-03-06/22-20-00_task_execution.md` | 測試結果：28/28 PASS |
| **Phase 6.3 Walkthrough** | `docs/logs/2026-03-06/22-20-00_walkthrough.md` | GPU 完成報告 ✅ |

### Phase 6.2 完成文件 (回溯參考)
| 文件 | 路徑 | 備注 |
|------|------|------|
| **Phase 6.2 Walkthrough** | `docs/logs/2026-03-06/22-05-00_walkthrough.md` | Halide 後處理融合完成 |
| **Phase 6.2 Execution** | `docs/logs/2026-03-06/21-55-00_task_execution.md` | 後處理遷移執行記錄 |

---

## 💡 關鍵技術現狀

### 效能數據 (Phase 6.3 之後)
```
2nd-call realize: 1071 ms
  估計分佈：
    - Param/Buffer 準備：~50 ms
    - AHD demosaic (CPU compute_root)：~800-900 ms  ⚠️ 主要瓶頸
    - Post-process GPU tile：~100-150 ms
    - copy_to_host：~20 ms
```

### GPU 排程現狀 (HalidePipeline.cpp build() 約第 843-980 行)
```
GPU (Metal) 路徑:
  ✅  exposed             → gpu_tile(16,16)
  ✅  lr_applied          → compute_root + gpu_tile(16,16)
  ✅  tc_applied          → compute_root + gpu_tile(16,16)
  ✅  lt_applied          → compute_root + gpu_tile(16,16)
  ✅  hsm_applied         → compute_root + gpu_tile(16,16)
  ✅  exp_gain_applied    → compute_root + gpu_tile(16,16)
  ✅  color_corrected     → compute_root + gpu_tile(16,16)

  ⚠️  linearised          → compute_root (CPU)
  ⚠️  g_h, g_v, r_h...   → compute_root (CPU) — stencil 依賴
  ⚠️  homo_h/v, sum_homo → compute_root (CPU) — stencil 依賴
  ⚠️  demosaic, refined_r → compute_root (CPU)
  ⚠️  diff_r, diff_b     → compute_root (CPU)
```

### PSNR 歷史軌跡
```
Phase 3 (Bilinear):      18.43 dB
Phase 5.1 (Exposure):    19.34 dB
Phase 5.3 (ToneCurve):  19.57 dB
Phase 5.2 (AHD):         35.57 dB ← 大幅提升
Phase 6.3 (GPU):         19.53 dB ← 注意：與 DNG SDK ref 比較，非 AHD 後結果
```
> **注意**：PSNR 計算比較的是 Halide 輸出與 DNG SDK reference render 的差異。

---

## 🛠️ Phase 6.4 實作計畫摘要

### Step 6.4.1 (優先) — 分段計時分析
**地點**：`HalidePipeline.cpp` → `HalidePipeline::process()` (Line 992)

在 `process()` 內的以下位置各插入計時：
```
T0: 函數入口
T1: 所有 pipe.xxx.set() 完成後
T2: hsmBuf/ltBuf/tcBuf 填充完成後
T3: pipe.exposed.realize(halide_out) 完成後
T4: halide_out.copy_to_host() 完成後
```

### Step 6.4.2 — Flutter NativeFinalizer 零拷貝
**地點 1**：`dng_processor/native/src/DngDecoder.cpp` 或 C-API wrapper  
→ 確認 / 新增 `extern "C" void halide_free_buffer(uint8_t*)` 函數

**地點 2**：`lib/ffi/dng_bindings.dart`  
→ 新增 `freeBuffer` Dart FFI 綁定

**地點 3**：`lib/services/dng_decoder_service.dart`  
→ 改用 `Pointer<Uint8>.asTypedList()` 替代 `Uint8List.fromList()`
→ 綁定 `NativeFinalizer` 到 buffer

### Step 6.4.3 (視情況) — AHD Demosaic GPU 化
若 6.4.1 顯示 AHD > 500ms，則考慮：
- 對 `g_h`,`g_v` 等加 `gpu_tile` + BoundaryConditions halo
- 分階段驗證 (先 g_h/g_v，再 homo，再 sum_homo)

---

## 📂 需要讀取的原始碼檔案

| 檔案 | 行數 | 本 Phase 關注點 |
|------|------|----------------|
| `native/src/HalidePipeline.cpp` | 1182 行 | **YAML index 見文件開頭**；process() 在 L992-L1182 |
| `native/include/HalidePipeline.h` | ~30 行 | API 介面確認 |
| `lib/ffi/dng_bindings.dart` | 中等 | 新增 freeBuffer FFI |
| `lib/services/dng_decoder_service.dart` | 中等 | 改 NativeFinalizer |

---

## ⚡ 新 Session 啟動指令

下一個 Session 開始時，建議先閱讀以下三個文件後再繼續：
```
1. rule.md
2. task.md (特別看 ACTIVE 區塊)
3. docs/logs/2026-03-06/22-15-00_handover_next_session.md (本文件)
4. docs/logs/2026-03-06/22-13-00_implementation_plan.md (Phase 6.4 實作計畫)
```

確認斷點後，直接開始 Step 6.4.1 修改 `process()` 分段計時。

---

## 🚨 已知地雷 (Gotchas)

1. **IDE lint 誤報**：clangd 無法找到 `HalidePipeline.h`，這是 IDE 未配置 cmake include paths 的問題，**不影響實際編譯**。cmake 可正常 build。
2. **GPU AHD tile boundary**：若嘗試對 demosaic stencil Funcs 加 gpu_tile，必須確保 `BoundaryConditions::repeat_edge` 覆蓋已包含 tile halo，否則 tile boundary 會出現色彩條紋。
3. **NativeFinalizer 生命週期**：必須確保 Flutter Widget 持有 token，防止 GC 在 image 顯示前提前回收 buffer。
4. **compile_jit 只執行一次**：修改 `build()` 時注意 `compiled = true` 保護，不可讓 pipeline 被重建。
5. **PSNR 19.53 dB vs 35.57 dB**：前者是與 DNG SDK reference 的差異（色彩演算法不同），後者是 AHD 後的精度。不要搞混。
