---
date: 2026-03-06T19:06:19
type: summary
related_task: "Chore: 建立檔案開頭 YAML-like 索引註解"
status: in_progress
---

## 現狀確認 (現狀總結)

1. **使用者需求**: 為了避免未來 AI 讀取檔案時產生 Context Loss，希望能建立一個規範：在長度較大（或具備多個模組）的程式碼檔案開頭，加入 YAML 格式的註解。此註解用以描述該檔案的用途、核心模組與對應行號 (或關鍵字搜尋點)，讓 AI 能在前 10 行快速掌握整體架構。
2. **目前 codebase 狀態**:
   - `DngDecoder.cpp` (約 362 行)
   - `HalidePipeline.cpp` (預期為目前最複雜的管線邏輯)
   - `main.dart` (約 256 行)
   - `dng_decoder_service.dart` (約 137 行)
   - 這些大型檔案目前開頭只有 `#include` 或 `import`，沒有整體的模組索引。
3. **下一步**: 撰寫 `implementation_plan.md` 來定義這個 YAML 註解的標準規範（同時也會提案加進 `rule.md`），並列出預計要修改的檔案列表與實作步驟。
