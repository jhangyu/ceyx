#!/usr/bin/env python3

import os
import sys
import json
import subprocess
import re
from pathlib import Path

REPO_ROOT = Path(os.environ.get("CLAUDE_PROJECT_DIR", Path(__file__).resolve().parents[2])).resolve()


def _in_repo() -> bool:
    try:
        return Path.cwd().resolve().is_relative_to(REPO_ROOT)
    except AttributeError:
        cwd = str(Path.cwd().resolve())
        root = str(REPO_ROOT)
        return cwd == root or cwd.startswith(root + "/")

# Read JSON from stdin
try:
    data = json.load(sys.stdin)
except json.JSONDecodeError:
    sys.exit(0)

if not _in_repo():
    sys.exit(0)

tool_name = data.get("tool_name") or data.get("tool", {}).get("name", "")
tool_input = data.get("tool_input") or data.get("tool", {}).get("input") or data.get("input") or {}
file_path = tool_input.get("file_path", "")

# Check if this is a native C++ file edit
if tool_name in ("Edit", "Write"):
    native_pattern = r"(?:^|/)native/(src|include|tests)/.*\.(cpp|h|c|mm)$"
    if re.search(native_pattern, file_path):
        try:
            result = subprocess.run(
                [
                    "python3",
                    "native/scripts/build_native_watchdog.py",
                    "--skip-configure",
                    "--target",
                    "test_decode",
                ],
                capture_output=True,
                text=True,
                timeout=120,
            )

            if result.returncode == 0:
                print("✅ Build watchdog passed")
            else:
                print("⚠️ Build watchdog failed")
                stderr_tail = result.stderr[-1000:] if result.stderr else ""
                if stderr_tail:
                    print(stderr_tail)

        except subprocess.TimeoutExpired:
            print("⚠️ Build watchdog timeout (120s)")
        except Exception as e:
            print(f"⚠️ Build watchdog error: {e}")

sys.exit(0)
