# 開發標準作業程序 (Development S.O.P.)

為了避免專案上下文長度過長導致 AI 遺失記憶 (Context Loss)，並建立完整可追溯的開發歷史，此專案嚴格遵守以下由時間戳記驅動的文件化開發流程。

---

## 🚀 新對話啟動指令 (Startup Protocol)

> **每次開啟新對話，使用者請直接複製以下文字貼到最開頭，強制 AI 執行環境掃描：**

```
請先依序閱讀以下文件並進行環境掃描，再開始任何工作：
1. rule.md — 開發標準作業程序
2. task.md — 任務清單，**特別注意頂部「🔴 現在進行中」區塊**
3. handover.md — 當前狀態與技術決策
4. task.md 中「現在進行中」連結的最新 Execution Log

完成掃描後，先輸出一份簡短的「現狀確認」，說明你對當前斷點的理解，再等待我的指示。
```

---

## 目錄結構與檔案索引約定
為了避免模型在尋找測試或原始程式碼時浪費 Token，請查閱 [**file_index.md**](file_index.md)，內含全專案各資料夾與重要檔案的定義。

所有開發過程產生的記錄檔均需放置於專屬的 `docs/logs/` 目錄內，並按照日期進行分類，以保持專案根目錄的整潔。

```text
docs/
└── logs/
    └── YYYY-MM-DD/
        ├── HH-MM-SS_summary.md
        ├── HH-MM-SS_implementation_plan.md
        ├── HH-MM-SS_task_execution.md
        ├── HH-MM-SS_walkthrough.md
        └── HH-MM-SS_memory.md
```

---

## 📝 特殊規則：Debug 與推導過程記錄 (`..._memory.md`)

- **時機**：任何 debug 執行過程中產生新發現、unit test 測試結果反思、或是提出新的 debug 思路與推導過程時。
- **動作**：
  1. **修改 Code 前記錄**：詳細記錄當前遇到的問題、推導過程、以及預計的解決思路。
  2. **修改 Code 並測試後記錄**：記錄測試結果，驗證該思路是否正確，總結結果與新發現。
- **存檔**：寫入 `docs/logs/YYYY-MM-DD/HH-MM-SS_memory.md`。
- **嚴格禁令**：**嚴厲禁止**一路不中斷地連續執行（一口氣修改大量程式碼並連續測試）。為了避免遇到上下文超限（Context Limit）導致模型對話中斷，使修改到一半的程式碼狀態遺失並造成後續 debug 困難，**必須**在修改程式碼前以及測試完成後，分別進行一次記憶記錄並等待確認。

---

## 📝 特殊規則：大型程式碼檔案的 YAML 索引 (Long File Index)
- **問題背景**：當單一程式碼檔案超過約 100 行，或包含多個邏輯模組時，AI 在掃描整份文件時極易遺失注意力或花費過多 Context 額度。
- **動作**：**必須**在該檔案最開頭（通常在 `import` 或 `#include` 區塊附近），建立一份 YAML-like 的索引，詳細記錄每個 function 的功能和所在行數。
- **讀取限制 (針對模型)**：模型在需要了解或修改該檔案前，**強制規定只能先藉由 view_file (StartLine=1, EndLine=100) 讀取檔案的最前 100 行**來獲取開頭的 Index 資訊。獲取 Index 後，必須透過指派特定的 StartLine 與 EndLine 參數去讀取特定的 function 或區塊，**絕對禁止在未查看 Index 的情況下，自行將整個大型檔案完整載入**。
- **索引規範與範例**：關於應如何在程式碼檔案最上方註記索引內容，以及更新時機與格式細節，請參照獨立規範檔案：[update_code_index_rule.md](update_code_index_rule.md)。

---

## 標準開發流程 (6 步驟)

### 1. [環境掃描] 現狀總結 (`..._summary.md`)
- **時機**：每次啟動新對話或開始一個全新的任務階段時。
- **動作**：閱讀上下文、核心文件 (handover.md, plan.md, task.md) 與目標後，生成一份現狀總結。
- **存檔**：寫入 `docs/logs/YYYY-MM-DD/HH-MM-SS_summary.md`。
- **註記**：在 `task.md` 的對應任務後方標註相對連結：`[Summary](docs/logs/YYYY-MM-DD/HH-MM-SS_summary.md)`。

### 2. [計畫制定] 實作計畫 (`..._implementation_plan.md`)
- **時機**：確認目標後，在實際修改任何程式碼**之前**。
- **動作**：生成詳細的架構設計與程式碼修改計畫。
- **存檔**：寫入 `docs/logs/YYYY-MM-DD/HH-MM-SS_implementation_plan.md`。
- **註記**：在 `task.md` 標註相對連結：`[Plan](docs/logs/YYYY-MM-DD/HH-MM-SS_implementation_plan.md)`。

### 3. [開發執行與記錄] 執行日誌 (`..._task_execution.md`)
- **時機**：開始修改程式碼直到該子任務測試完成的整個循環。
- **動作**：此檔案為追加式紀錄，包含四個子段落：
  1. **Modify Plan (預計修改)**：列出本次要修改的檔案列表與邏輯。
  2. **⏹️ 中斷點快照 (Breakpoint Snapshot)**：**每完成一個子步驟後立即覆寫更新**，格式如下：
     ```
     - **已完成**: `具體完成的子步驟`
     - **下一步**: `接下來要做的子步驟`
     - **待確認**: `需要使用者決策的事項（若無則填「無」）`
     - **更新時間**: YYYY-MM-DDTHH:MM:SS
     ```
  3. **Test Result (測試結果)**：修改完成後執行測試，將日誌、截圖說明或報錯訊息追加寫入。
  4. **Modify Summary (修改總結)**：確定任務完成（或判斷需要放棄/重構）後，追加總結這回合的最終成果。
- **存檔**：寫入 `docs/logs/YYYY-MM-DD/HH-MM-SS_task_execution.md`。
- **註記**：在 `task.md` 標註相對連結：`[Execution](docs/logs/YYYY-MM-DD/HH-MM-SS_task_execution.md)`。

> **⚠️ Token 壓力警示規則**：當 AI 預估目前的對話內容已達到約 60-70% 的上下文容量（通常在執行複數個大型檔案修改後），**必須主動提示使用者**：
> 「⚠️ 上下文壓力警示：目前對話長度已較長，建議在完成本子任務後中斷對話，下次開啟新對話繼續，以避免上下文意外截斷。」
> 並在發出警示前，**強制更新中斷點快照與 `handover.md`**，確保狀態已持久化。

### 4. [成果驗證] 驗證報告 (`..._walkthrough.md`)
- **時機**：任務執行完成並通過基本測試後，進行全功能驗證或 E2E 驗證時。
- **動作**：總結本次變更的最終成果，並包含測試證明（如截圖、錄影連結）。
- **存檔**：寫入 `docs/logs/YYYY-MM-DD/HH-MM-SS_walkthrough.md`。
- **註記**：在 `task.md` 標註相對連結：`[Walkthrough](docs/logs/YYYY-MM-DD/HH-MM-SS_walkthrough.md)`。

### 5. [狀態同步] 更新核心文件
- **時機**：一個任務階段 (Task) 成功完成、大規劃目標達成、發現重大知識或是新增了重要的單元測試時。
- **動作**：將最新的全域狀態與決策，提綱挈領地更新回專案根目錄的核心文件（避免過於冗長），並不要刪除過去的內容：
  - **`handover.md`**: 短期狀態與技術決策移交。
  - **`task.md`**: 打勾完成項目，更新「🔴 現在進行中」指針。
  - **`plan.md`**: **大階段里程碑計畫**。當時序推進到一個新的大 Phase 或里程碑被整體改變/達成時，**必須**同步將目前的階段完成狀況打勾，供後續大方向參考。
  - **`memory.md`**: **全域知識庫與避坑指南**。當踩到開發地雷 (Gotchas)、發現特定架構瓶頸、解決重要 Bug 或完成一整個大 Phase 時，**必須**將該關鍵知識、注意事項及效能數據更新至 `memory.md` 中，供未來防呆與決策參考。
  - **`unit_test.md`**: **測試策略與索引**。當專案內新增了測試邏輯檔案或需要修正目前全域的 Watchdog 策略時，**必須**將新的測試案例及其路徑補到 `unit_test.md`。

> **即時同步規則**：每當完成一個**影響架構的子決策**（如：選定某個 Library、更改介面設計、發現重要 Bug 並決定繞過方式），AI **必須立即**將其簡要寫入 `handover.md`，並將長遠影響的「教訓與地雷」整理歸納後寫入 `memory.md` 的「開發地雷與注意事項」區塊。

### 6. [版本控制] Git 提交與分支管理 (Git & CI/CD Workflow)
- **時機**：確認單一子任務或是 Bug 修復完成並通過基本測試後。
- **動作**：
  1. 確保所有的 Log 和文件 (`task.md`, `handover.md` 等) 更新已保存。
  2. 使用 `git status` 確認變更狀態。
  3. 使用 `git add <具體檔案>` 階段性加入變更。
  4. **嚴格遵守 Conventional Commits 規範**（以利 CI/CD 自動化釋出與 Changlog 產生）：
     - `feat:` 新增功能
     - `fix:` 修復 Bug
     - `docs:` 只有文件變更 (如更新 `rule.md`, `handover.md`)
     - `style:` 程式碼格式變更（不影響程式碼運作）
     - `refactor:` 重構程式碼（非新增功能或修補 Bug）
     - `perf:` 效能優化
     - `test:` 新增或修改測試
     - `chore:` 建置過程或輔助工具的變更
     - 例如：`git commit -m "fix(halide): correct ToneCurve application order based on DNG SDK"`
- **分支規範**：若實作重大 Feature，應要求使用者開立 `feature/xxx` 分支；修正問題應使用 `bugfix/xxx` 分支；確保 `main` 分支隨時處於可編譯、可自動化測試通過的穩定狀態。

### 7. [決策中繼點] 評估下一步
- **成功**：如果階段任務成功完成提交，且使用者無特別指示，AI 可自動著手開始下一項有依賴關係的 Task（重複進入步驟 1~4）。
- **失敗或卡關**：如果任務失敗或遇到無法輕易繞過的架構瓶頸，先在執行日誌 (`_task_execution.md`) 總結本次測試結果與失敗原因，接著向使用者回報並要求評估，詢問使用者希望如何開始下一階段的調整策略。

---

## `task.md` 頂部指針區塊格式 (必填)

> `task.md` **頂部必須維護以下「現在進行中」區塊**，讓新對話能一眼定位斷點：

```markdown
## 🔴 現在進行中 (ACTIVE)
- **Task**: 7. 優化與防呆 (UX Enhancements)
- **Step**: 3. [開發執行] — 正在實作「匯出/下載」功能
- **Execution Log**: [docs/logs/YYYY-MM-DD/HH-MM-SS_task_execution.md](...)
- **衡斷點**: 已完成 CSV 匯出邏輯，尚未實作 Excel 格式與 UI 觸發按鈕
```

---

## 紀錄檔格式 (YAML Frontmatter)
為了確保未來資料的可檢索性，所有存放於 `docs/logs/` 的 Markdown 檔案，開頭都**必須**包含 YAML Frontmatter 區塊：

```markdown
---
date: YYYY-MM-DDTHH:MM:SS
type: summary | implementation_plan | task_execution | walkthrough
related_task: "任務名稱 (對應 task.md)"
status: in_progress | success | failed
---

## 目標 / 內容 ...
```

