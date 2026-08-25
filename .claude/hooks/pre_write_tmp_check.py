#!/usr/bin/env python3
"""禁止寫入 /tmp/ 或操作 /tmp/ — 所有臨時操作必須在專案內進行"""
import os
from pathlib import Path
import sys
import json

REPO_ROOT = Path(os.environ.get("CLAUDE_PROJECT_DIR", Path(__file__).resolve().parents[2])).resolve()


def _in_repo() -> bool:
    try:
        return Path.cwd().resolve().is_relative_to(REPO_ROOT)
    except AttributeError:
        cwd = str(Path.cwd().resolve())
        root = str(REPO_ROOT)
        return cwd == root or cwd.startswith(root + "/")


try:
    msg = json.loads(sys.stdin.read())
    tool = msg.get('tool') or {}
    tool_name = msg.get('tool_name') or tool.get('name', '')

    if not _in_repo():
        sys.exit(0)

    # Write 工具檢查
    if tool_name == 'Write':
        tool_input = msg.get('tool_input') or tool.get('input') or {}
        file_path = tool_input.get('file_path', '')
        if file_path.startswith('/tmp/'):
            print('❌ 禁止寫入 /tmp/ —— 請使用 native/scripts/tmp/ 或其他專案內目錄', file=sys.stderr)
            sys.exit(1)

    # Bash 工具檢查
    if tool_name == 'Bash':
        tool_input = msg.get('tool_input') or tool.get('input') or {}
        cmd = tool_input.get('command', '')
        import re

        # 檢查重定向到 /tmp (可以有多個空格, 無空格等變體)
        if re.search(r'(?:>|>>|\|)\s*/tmp(?:/|$|\s)', cmd):
            print('❌ 禁止重定向到 /tmp/ —— 請使用 native/scripts/tmp/ 或其他專案內目錄', file=sys.stderr)
            sys.exit(1)

        # 檢查工具寫入 /tmp (tee, cp, mv, install 等)
        if re.search(r'\b(tee|cp|mv|install|cat|dd)\b.*\s/tmp(/|$|\s)', cmd):
            print('❌ 禁止寫入 /tmp/ —— 請使用 native/scripts/tmp/ 或其他專案內目錄', file=sys.stderr)
            sys.exit(1)

        # 檢查 cd /tmp (開頭或分隔符後)
        if re.search(r'(?:^|;|&&|\|\|)\s*cd\s+/tmp(?:/|$|\s)', cmd):
            print('❌ 禁止操作 /tmp/ —— 請在專案內進行', file=sys.stderr)
            sys.exit(1)

        # 檢查環境變數設為 /tmp
        if re.search(r'[A-Z_]\w*=/tmp(/|$)', cmd):
            print('❌ 禁止將環境變數設為 /tmp/ —— 請在專案內進行', file=sys.stderr)
            sys.exit(1)

    sys.exit(0)

except Exception as e:
    print(f'Hook 執行失敗: {e}', file=sys.stderr)
    sys.exit(1)
