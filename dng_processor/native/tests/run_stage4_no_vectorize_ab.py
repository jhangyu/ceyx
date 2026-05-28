#!/usr/bin/env python3
"""
Retired Stage4 no-vectorize A/B runner.

This script used to pass the historical CMake flag
`DNG_RENDER_STAGE4_NO_VECTORIZE` and parse `[RenderHalide]` /
`[RenderHalideDiff]` logs. The native emitters and the env-gated
Stage4-isolated measurement path were retired in commit 49d8111, so the old
runner could only produce a misleading all-N/A report.

Use `run_decode_matrix.py --repeat 3 --timing` for current Stage4 PSNR and
performance validation.
"""

import argparse
import sys


RETIRED_MESSAGE = """\
run_stage4_no_vectorize_ab.py is retired.

Reason:
  The Stage4-isolated `[RenderHalide]` / `[RenderHalideDiff]` measurement
  emitters were removed in commit 49d8111 with the retired env switches. The
  old script no longer had a valid parser target and produced all-N/A tables.

Current validation:
  python3 dng_processor/native/tests/run_decode_matrix.py --repeat 3 --timing
"""


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Retired Stage4 no-vectorize A/B runner."
    )
    parser.add_argument(
        "--allow-retired",
        action="store_true",
        help="Acknowledge retirement and print the replacement command.",
    )
    parser.parse_args()
    print(RETIRED_MESSAGE, file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
