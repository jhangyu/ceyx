#!/usr/bin/env python3
"""PreToolUse Bash 白名單檢查 (per CLAUDE.md「Subagent / Bash 執行規範」).

禁止：
- inline interpreter (`python3 -c`, `bash -c`, `node -e` 等)
- heredoc (`<<EOF`, `<<-'EOF'` 等)
- env 變數前綴 (`FOO=1 ./bin` —— 必須改寫到 script 內或 wrapper)
- shell 控制流 (`for / while / until / if / case`)

違規 → exit 2，blocking 並把 stderr 訊息回傳 Claude，要求改寫成
`dng_processor/native/scripts/tmp/<name>.{py,sh}` 後再執行。
"""
from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(os.environ.get("CLAUDE_PROJECT_DIR", Path(__file__).resolve().parents[2])).resolve()


def _in_repo() -> bool:
    try:
        return Path.cwd().resolve().is_relative_to(REPO_ROOT)
    except AttributeError:
        cwd = str(Path.cwd().resolve())
        root = str(REPO_ROOT)
        return cwd == root or cwd.startswith(root + "/")


_INLINE_PATTERNS = [
    (r"\bpython3?\s+-c\b", "python3 -c"),
    (r"\bbash\s+-c\b", "bash -c"),
    (r"\bsh\s+-c\b", "sh -c"),
    (r"\bzsh\s+-c\b", "zsh -c"),
    (r"\bnode\s+-e\b", "node -e"),
    (r"\bruby\s+-e\b", "ruby -e"),
    (r"\bperl\s+-e\b", "perl -e"),
]
_HEREDOC = re.compile(r"<<-?\s*['\"]?[A-Za-z_]\w*['\"]?")
# 需要匹配 pipe 和換行，使用 DOTALL 或直接在模式中加入 \n 和 |
_CONTROL_KW = re.compile(r"(?:^|;|&&|\|\||[\n|])\s*(for|while|until|if|case)\s", re.DOTALL)
_ENV_PREFIX = re.compile(r"^\s*(?:[A-Z_][A-Z0-9_]*=\S+\s+){1,}\S")


def _check(cmd: str) -> list[str]:
    issues: list[str] = []
    for pat, name in _INLINE_PATTERNS:
        if re.search(pat, cmd):
            issues.append(f"inline interpreter `{name}` (CLAUDE.md §1)")
            break
    if _HEREDOC.search(cmd):
        issues.append("heredoc (`<<EOF` 等) —— 一次性腳本必須落檔 (CLAUDE.md §1/§3)")
    m = _CONTROL_KW.search(cmd)
    if m:
        issues.append(f"shell 控制流 `{m.group(1)}` —— 多步驟邏輯寫進 wrapper script (CLAUDE.md §3)")
    if _ENV_PREFIX.match(cmd):
        prefix = cmd.split(None, 1)[0]
        issues.append(f"env 前綴 `{prefix}` —— 改用 `os.environ.setdefault()` 或 wrapper (CLAUDE.md §2)")
    return issues


def main() -> int:
    try:
        msg = json.loads(sys.stdin.read())
    except Exception:
        return 0
    if msg.get("tool_name") != "Bash":
        if msg.get("tool", {}).get("name") != "Bash":
            return 0
    if not _in_repo():
        return 0
    tool_input = msg.get("tool_input") or msg.get("tool", {}).get("input") or msg.get("input") or {}
    cmd = tool_input.get("command", "")
    if not cmd:
        return 0

    issues = _check(cmd)
    if not issues:
        print(json.dumps({
            "hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "allow",
                "permissionDecisionReason": "Command passed whitelist safety check"
            }
        }))
        return 0

    print("❌ Bash 命令違反 CLAUDE.md「Subagent / Bash 執行規範」白名單寫法：", file=sys.stderr)
    for item in issues:
        print(f"  • {item}", file=sys.stderr)
    print("", file=sys.stderr)
    print("修正方向：", file=sys.stderr)
    print("  1. 寫成 wrapper script 落檔到 `dng_processor/native/scripts/tmp/<name>.{py,sh}`", file=sys.stderr)
    print("  2. 在 script 內處理 env / 迴圈 / heredoc / 多步驟邏輯", file=sys.stderr)
    print("  3. 再用既有白名單入口執行 wrapper（python3 scripts/tmp/... 或 bash scripts/tmp/...）", file=sys.stderr)
    print("", file=sys.stderr)
    print(f"原命令: {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
