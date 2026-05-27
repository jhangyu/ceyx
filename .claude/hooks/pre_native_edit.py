#!/usr/bin/env python3

import sys
import json
import subprocess
import re

# Read JSON from stdin
try:
    data = json.load(sys.stdin)
except json.JSONDecodeError:
    sys.exit(0)

tool_name = data.get("tool_name", "")
file_path = data.get("tool_input", {}).get("file_path", "")

# Check if this is a native C++ file edit
if tool_name in ("Edit", "Write"):
    native_pattern = r"dng_processor/native/.*\.(cpp|h|c|mm)$"
    if re.search(native_pattern, file_path):
        try:
            result = subprocess.run(
                [
                    "python3",
                    "dng_processor/native/scripts/build_native_watchdog.py",
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
