---
date: 2026-03-05T21:08:00
type: implementation_plan
related_task: "Setup Local Git Repository"
status: in_progress
---

## 目標
1. 在專案根目錄 (`/Users/jhangyu/Documents/flutter_dng_decoder`) 初始化一個 Git 伺服器，以便於專案版本控制與管理。
2. 將業界標準的版本控制與 CI/CD `Conventional Commits` 規範納入 `rule.md` 的開發 SOP 之中。

## Proposed Changes

1. **更新 `rule.md` (已完成)**:
   - 在標準開發流程加入「第 6 步：[版本控制] Git 提交與分支管理 (Git & CI/CD Workflow)」。
   - 定義了基於 Conventional Commits 的提交前綴 (feat, fix, docs, refactor 等)。
   - 訂立分支管理策略 (feature/*, bugfix/*, main 持續交付)。

2. **建立根目錄的 `.gitignore` 檔案**:
   我們將在專案根目錄建立一個 `.gitignore`，用來排除不應該進入版本控制的檔案，包含：
   - 系統檔案：`.DS_Store`
   - 巨大相片測試檔案：`sample.arw`, `sample.dng`, 以及其他 `*.dng`, `*.arw` 等大型媒體
   - 錯誤日誌與日誌檔案：`*.log` (包含 `hs_err_pid*.log`)
   - 虛擬環境：`.venv/`
   ```gitignore
   # OS
   .DS_Store

   # Logs
   *.log

   # Python / Env
   .venv/
   __pycache__/

   # Large media testing files
   *.dng
   *.arw
   ```
   *(註：`dng_processor` 目錄內已有 Flutter 預設的 `.gitignore`，這部分與根目錄互不衝突，會自動過濾編譯產物。)*

3. **初始化 Git 儲存庫 (`git init`)**:
   執行 `git init` 開啟版本控制。

4. **加入專案檔案至暫存區 (`git add .`)**:
   加入所有除了忽略清單以外的文件 (包含過去所有的 docs/logs 歷史記錄)。

5. **建立第一次提交 (`git commit`)**:
   使用 Conventional Commits 規範建立專案起點：
   `git commit -m "chore: initial project commit with DNG CPU decoding pipeline and Halide integration"`

## Verification Plan
1. 執行 `git status` 確認沒有預期外的大型檔案 (如 .dng) 或 build 資料夾被追蹤。
2. 執行 `git log` 確認 Conventional Commits 格式的 "chore" 第一次提交已生成。

---
*此文件於 2026-03-05T21:11:00 撰寫，在使用者看過並確認執行之前，不會更動任何專案指令。*
