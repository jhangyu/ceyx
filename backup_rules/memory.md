# 專案記憶檔 (Memory / State Tracking)
> 最後更新: 2026-03-05T20:00:00+08:00

## 目前階段
**Phase 6.3: Halide Metal GPU 加速** — ✅ 已完成 (PSNR=19.53dB, 1071ms)
**Phase 6.4: Zero-copy 機制與效能瓶頸** — 🔴 進行中

## 重要決策與技術優化
- **Phase 6.1-6.2**: 將 HSM、LT、TC、LR、Gamma 後處理作業全面重構移入 Halide 運算圖，達成運算融合，CPU 耗時從 3800ms 降至 1500ms。
- **Phase 6.3**: 引入 Metal GPU 加速。將沒有 Stencil 依賴之後處理階段改用 GPU Tile 排程 (`gpu_tile(16,16)`)。AHD 去馬賽克由於鄰域計算複雜度，目前保持 CPU `compute_root()`。總時間壓至 **1071 ms**。

## 關鍵數據 (sample.dng — 16-bit Bayer)
| 項目 | 值 |
|------|-----|
| 尺寸 | 6048 × 4024 |
| 壓縮 | Lossless JPEG (tag 7) |
| PSNR 比較 (vs SDK reference) | 19.53 dB |
| PSNR 比較 (AHD 真實度) | 35.57 dB |
| 無優化 (Phase 3) | ~13,500 ms |
| C++ 多線程 (Phase 5) | ~3,800 ms |
| Halide CPU 融合 (Phase 6.2)| ~1,500 ms |
| Halide Metal GPU (Phase 6.3)| **1,071 ms** |

## XMP Key Parameters 與 DCP LUT
（同先前的狀態，包括 RNI Films Profile 與 Saturation = 1.35 增強）。`CameraToPCS` 矩陣負責白平衡應用，切勿二次套用 `AsShotNeutral`。

## 開發地雷與注意事項 (Gotchas)
1. **IDE Lint 誤報**: `clangd` 可能會顯示找不到 `HalidePipeline.h`，這是 IDE 尚未設定 CMake include variables 導致的，實務上以 `cmake` 編譯為準。
2. **GPU AHD Tile Boundary 偽影**: 對具備 Stencil（鄰域存取）的 Func 加上 `gpu_tile` 時，如果不妥善處理 `BoundaryConditions::repeat_edge` 及擴充 Halo 區域，會在 Tile 交界處產生色彩斷層條紋。
3. **compile_jit 重複編譯**: 建立 Halide Graph 時 JIT 編譯非常耗時，必須利用 `compiled = true` 狀態鎖來保護 `compile_jit` 確保僅執行一次。
4. **NativeFinalizer 記憶體生命週期**: 從 C++ 透過 Dart FFI 將指標轉成 Uint8List 並套用 NativeFinalizer 實作 Zero-copy 時，**必須**確保 Flutter Widget 按預期正確 Hold 住 Token，否則 GC 可能會在圖片渲染前提前釋放記憶體。
5. **雙重 PSNR 標準混淆**: 19.53dB 是「對比 DNG SDK 純正參考圖」的演算法差異值，35.57dB 則是早期純粹計算「AHD 對比理想訊號」的誤差。這兩者標準不同不可混用。

## 專案結構與文件體系
- `dng_processor/native/src/DngDecoder.cpp`: 讀取 DNG 與取出 metadata
- `dng_processor/native/src/HalidePipeline.cpp`: JIT-cached Halide pipe (處理 AHD, HSM, LT, LR...)
- `dng_processor/lib/src/dng_decoder_service.dart`: 對接 FFI 與未來的 Zero-copy 記憶體實踐。

- `memory.md` — 本檔案，專案狀態、關鍵數據與地雷坑
- `rule.md` — 開發標準作業程序 (SOP) 與行為準則
- `task.md` — 開發任務列表 (含當前進度)
- `handover.md` — 短期交接與上下文
- `file_index.md` — 檔案對照目錄

## Phase 6.4 待辦目標
1. **分段計時**: 於 HalidePipeline `process()` 埋設 T0-T4 效能檢測，定位 1071ms 中的最大熱點。
2. **Zero-copy**: 實裝 Dart `NativeFinalizer`。
