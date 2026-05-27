#!/usr/bin/env python3

import sys
import json
import subprocess
import re

try:
    data = json.load(sys.stdin)
except json.JSONDecodeError:
    sys.exit(0)

command = data.get("tool_input", {}).get("command", "")

BUILD_PATTERNS = [
    r"cmake\s+--build",
    r"build_native_watchdog\.py",
]
SKIP_PATTERNS = [
    r"run_decode_matrix\.py",
    r"compare_psnr\.py",
    r"test_decode.*baseline",
]

if not any(re.search(p, command) for p in BUILD_PATTERNS):
    sys.exit(0)
if any(re.search(p, command) for p in SKIP_PATTERNS):
    sys.exit(0)

print("\n[post-hook] Build 完成，執行 PSNR / 效能回退確認...")

try:
    result = subprocess.run(
        ["python3", "dng_processor/native/tests/run_decode_matrix.py"],
        capture_output=True,
        text=True,
        timeout=300,
    )
    output = (result.stdout + result.stderr).strip()
    print(output[-3000:] if len(output) > 3000 else output)

    if result.returncode != 0:
        print("\n[post-hook] ⚠️  回退測試執行失敗，請手動確認")
    else:
        print("\n[post-hook] ✅ 回退確認完成，請核對 PSNR 與 timing 數值")
except subprocess.TimeoutExpired:
    print("[post-hook] ⚠️  run_decode_matrix.py timeout (300s)")
except Exception as e:
    print(f"[post-hook] ⚠️  錯誤: {e}")

sys.exit(0)
