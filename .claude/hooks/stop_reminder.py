#!/usr/bin/env python3

import os
from pathlib import Path

REPO_ROOT = Path(os.environ.get("CLAUDE_PROJECT_DIR", Path(__file__).resolve().parents[2])).resolve()


def _in_repo() -> bool:
    try:
        return Path.cwd().resolve().is_relative_to(REPO_ROOT)
    except AttributeError:
        cwd = str(Path.cwd().resolve())
        root = str(REPO_ROOT)
        return cwd == root or cwd.startswith(root + "/")


if _in_repo():
    print("""
╔══════════════════════════════════════════════════════════════╗
║                📋 對話結束前使用sonnet agent更新                ║
╠══════════════════════════════════════════════════════════════╣
║  必更新：                                                     ║
║    • memory.md   ── 新發現的架構決策或避坑記錄                   ║
║    • task.md     ── 完成狀態、下一步、中斷點快照                  ║
║    • handover.md ── 中斷點摘要與下一步建議                       ║
║    • plan.md     ── Phase 完成狀態（若有里程碑推進）             ║
║                                                              ║
║  視情況更新：                                                 ║
║    • file_index.md ── 若新增 / 移除 / 重命名任何檔案            ║
║    • unit_test.md  ── 若 PSNR 門檻或測試策略有變                ║
╚══════════════════════════════════════════════════════════════╝
""")
