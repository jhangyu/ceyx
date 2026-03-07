---
date: 2026-03-06T22:13:00
type: implementation_plan
related_task: "Phase 6.4: 效能瓶頸分析與 Zero-copy 機制"
status: in_progress
---

# Phase 6.4 Implementation Plan: Zero-copy & Performance Analysis

## 目標
1. 實作詳細分段計時，定量識別瓶頸。
2. 實作 Flutter `NativeFinalizer` 零拷貝機制，消除 FFI Buffer 拷貝。
3. 評估 AHD demosaic GPU 化方案。

---

## Step 6.4.1: 分段計時分析 (效能剖析)

### 修改 `HalidePipeline.cpp` — process() 函數
在以下各時間點插入 `std::chrono::steady_clock::now()` 計時：
```
[T0] 函數入口
[T1] pipe.rawParam.set() 等所有 Param 綁定完成
[T2] Buffer 準備與填充 (hsmBuf, ltBuf, tcBuf) 完成
[T3] pipe.exposed.realize() 完成
[T4] halide_out.copy_to_host() 完成
[T5] 函數返回前
```

輸出格式示範：
```
[Halide Perf] Param bind:      5 ms
[Halide Perf] Buffer fill:    45 ms
[Halide Perf] realize:      980 ms
[Halide Perf] copy_to_host:  20 ms
[Halide Perf] Total:       1071 ms
```

---

## Step 6.4.2: Flutter 端 NativeFinalizer 零拷貝

### 目前問題
`HalidePipeline::process()` 回傳 `uint8_t *out = new uint8_t[...]`，Flutter 端需要：
1. 透過 Dart FFI 拿到指標。
2. `Uint8List.fromList()` 拷貝一次記憶體 → 浪費時間與記憶體。

### 解決方案
修改 Flutter 端 `DngDecoderService` 或 `DngBindings`，改用 C-API 返回指標後，
用 `Pointer<Uint8>` 直接包裝，並通過 `NativeFinalizer` 綁定 `free()` 回收：

```dart
// 範例 Dart 端 zero-copy flow
final ptr = dngBindings.processImage(...);          // C++ 返回 uint8_t*
final buffer = ptr.asTypedList(width * height * 4); // 零拷貝！不呼叫 fromList
// 建立 NativeFinalizer 在 GC 時呼叫 free(ptr)
final finalizer = NativeFinalizer(dngBindings.freeBuffer.cast());
finalizer.attach(buffer, ptr.cast(), detach: token);
```

#### 需修改的 Flutter 檔案
- `lib/services/dng_decoder_service.dart` — 改用 pointer 而非 List
- `lib/ffi/dng_bindings.dart` — 新增 `freeBuffer(Pointer<Uint8>)` FFI 綁定

#### 需修改的 C++ 頭檔
- `include/HalidePipeline.h` — 確認 `process()` 已回傳 `uint8_t*`（已是）
- 確認 C-API `extern "C"` 包裝層有 `free_buffer(uint8_t*)` 函數

---

## Step 6.4.3: AHD Demosaic GPU 化評估

AHD 中間 Funcs 目前的 `compute_root()` 消耗大量 CPU 時間（估計 800ms）。

### GPU 化挑戰
- `homo_h`/`homo_v` 需比較鄰域 8 方向亮度差，需要 ±1 像素的 stencil。
- `sum_homo_h`/`sum_homo_v` 需 3×3 窗格加總，stencil 半徑更大。
- `g_h`/`g_v` 依賴 ±2 的水平/垂直像素，需要 tile halo = 2。

### 可行方案 A：gpu_tile 加 Boundary Condition
```cpp
// 對 linearised 添加邊界條件並 GPU tile
g_h.compute_root().gpu_tile(x, y, xo, yo, xi, yi, 32, 4);
// 依賴 clamped BoundaryConditions::repeat_edge 自動處理 halo
```

### 可行方案 B：保留 stencil Funcs 在 CPU，只加速後半段
現狀（Phase 6.3）已是此方案，繼續評估是否值得投入 A 方案。

**建議**：先完成 6.4.1 計時分析，確認各子步驟比例後，再決定 6.4.3 的投入力度。

---

## 驗證計畫
1. **編譯驗證**：`cmake --build . --target test_dng_decoder`
2. **效能驗證**：對比計時 log，確認 Buffer fill 時間是否可優化。
3. **零拷貝驗證**：Flutter 端測試記憶體使用量，確認無拷貝發生。
4. **PSNR 迴歸**：PSNR 必須維持 > 19 dB。

## ⚠️ 風險
- `NativeFinalizer` 需確保 GC 未在 Flutter 使用 Buffer 期間提前回收。
- AHD GPU tile halo 若計算錯誤，會導致 tile boundary 出現色彩條紋假像。
