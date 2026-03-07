---
date: 2026-03-06T22:12:00
type: summary
related_task: "Phase 6.4: 效能瓶頸分析與 Zero-copy 機制"
status: in_progress
---

## 任務目標
Phase 6.4 的核心目標是通過以下兩個方向繼續壓縮處理時間，向 < 0.5s 目標推進：
1. **記憶體零拷貝 (Zero-copy)**：消除 FFI 層的記憶體拷貝，改用 `NativeFinalizer` 讓 Flutter 直接使用 C++ 分配的 Buffer。
2. **效能瓶頸分析**：定量分析 Phase 6.3 結果，確定 AHD demosaic 是否仍是主要瓶頸，並研究後續優化方向。

## 現狀總結 (Phase 6.3 完成狀況)

### 效能數據
| 階段 | 2nd-call 時間 |
|------|--------------|
| 無優化 (Phase 3) | ~13,500 ms |
| C++ 多線程 | ~3,800 ms |
| Halide CPU 融合 (Phase 6.2) | ~1,500 ms |
| Halide Metal GPU (Phase 6.3) | **~1,071 ms** |

### 瓶頸分析
- Post-process Funcs (HSM/LT/TC/LR/Gamma) 已 GPU-tile 化。
- AHD demosaic 的 CPU `compute_root()` stencil 計算仍是主要耗時（估計 800-900ms）。
- Host→Device 和 Device→Host 傳輸時間尚未測量。

## 待解決挑戰
1. **Flutter FFI 記憶體拷貝**：`process()` 目前回傳 `new uint8_t[]`，Flutter 端需要拷貝一次。`NativeFinalizer` 可消除此次拷貝。
2. **AHD Demosaic GPU 化**：stencil 鄰域計算需要 tile boundary halo 處理，技術複雜度較高。
3. **傳輸瓶頸測量**：需量化 Host↔Device 傳輸時間，確認是否是瓶頸。

## Phase 6.4 範疇確定
- **6.4.1**: 在 `process()` 加入分段計時 log，定量分析各子步驟耗時。
- **6.4.2**: 實作 Flutter 端 `NativeFinalizer` 零拷貝機制。
- **6.4.3**: 評估 AHD demosaic GPU 化的可行性（如 gpu_tile + 邊界擴充）。
