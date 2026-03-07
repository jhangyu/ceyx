---
date: 2026-03-06T19:06:19
type: implementation_plan
related_task: "Chore: 建立檔案開頭 YAML-like 索引註解"
status: in_progress
---

## 目標與背景
隨著程式碼不斷增加，大型檔案容易讓 AI 遇到上下文限制（Context Limit）問題。目標是定義一個能在大型程式碼檔案開頭使用的 `YAML-like Index`，讓 AI 在閱讀前 10~20 行時，就能精準掌握這份檔案包含哪些模組區塊以及對應的行號。

## 設計架構 (YAML Header Format)

計畫在每一個長度超過 100 行或邏輯較複雜的檔案最前方（在 import / include 之後，或是檔案的最開頭），加入以下格式的註解：

**C++ 範例 (/** ... **/)**:
```cpp
/*
---
file_summary: "Halide 影像處理管線，包含去馬賽克、色彩矩陣與曝光補償"
modules:
  - name: "Helper Functions"
    description: "RGB與HSV互轉邏輯"
    lines: "23-89"
  - name: "applyHueSatMap"
    description: "CPU端三線性插值實作"
    lines: "91-173"
  - name: "HalidePipeline::process"
    description: "Halide 主要管線入口"
    lines: "383-733"
---
*/
```

**Dart 範例 (/* ... */)**:
```dart
/*
---
file_summary: "管理 DNG 解析生命週期與 FFI 綁定狀態"
modules:
  - name: "DngImage"
    description: "Dart 端影像資料容器"
    lines: "8-34"
  - name: "DngDecoderService"
    description: "負責呼叫 FFI 介面進行解碼"
    lines: "47-137"
---
*/
```

## 預計修改計畫

### 1. 修改 `rule.md`
在 `rule.md` 中新增「大型檔案索引 (File Index) 規範」條款。要求 AI 在更新或建立超過百行的檔案時，必須同時維護這個區塊。

### 2. 修改現有大型 codebase 檔案
依序為以下檔案加入這個索引（行號以當前版本為準）：
- **Native (C++)**:
  - `dng_processor/native/src/DngDecoder.cpp`
  - `dng_processor/native/src/HalidePipeline.cpp`
- **Flutter (Dart)**:
  - `dng_processor/lib/main.dart`
  - `dng_processor/lib/src/dng_decoder_service.dart`
  - `dng_processor/lib/src/dng_bindings.dart`

### 3. 更新 `task.md`
加入本次任務作為一項獨立的 Chore。

## 驗證方式
- 修改完成後，檢查是否引發編譯錯誤。
- 肉眼檢查標示的行號是否正確對應到實體功能的開頭。
