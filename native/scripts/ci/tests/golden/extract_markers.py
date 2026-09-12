#!/usr/bin/env python3
"""Golden-fixture extraction helper (WI-3).

Extracts the ordered, normalized marker lines for one CI leg out of a FULL
job log (per plan C-G17: full job logs only, never a `--log-failed` capture
or any other pre-filtered subset) and prints them to stdout, one per line.

Usage:
    python3 native/scripts/ci/tests/golden/extract_markers.py \\
        --log tmp/verify/pyci-AC2-BASELINE-r7-34707800234-d33cc607-ALLGREEN.log \\
        --leg linux \\
        > native/scripts/ci/tests/golden/baseline/r7-34707800234-d33cc607-linux.markers

This is a committed helper, not a throwaway snippet, so the handover's
regeneration procedure can be PROVED (a regenerated fixture piped through
`diff` against the committed one, exit 0) rather than merely asserted.

``--log`` may be either:
  * a combined multi-job log (`gh run view <run-id> --log`, no `--job`),
    which prefixes every line with a job-name column -- this script filters
    to the job matching ``--leg`` via ``LEG_TO_JOB_NAME``; or
  * a single-job log (`gh run view <run-id> --log --job <job-id>`), which
    has no job-name column -- every line is already in scope, so no
    filtering happens (detected by the first non-blank line's leading field
    not matching any known job name).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Maps this campaign's leg names to the exact GitHub Actions job display
# name they correspond to in a combined run log. Provenance:
# tmp/verify/pyci-AC2-BASELINE-r7-34707800234-d33cc607-ALLGREEN.log
# (`cut -f1 <log> | sort -u`).
LEG_TO_JOB_NAME = {
    "linux": "Linux / x86_64 Vulkan",
    "macos-arm64": "macOS / arm64",
    "macos-x86_64": "macOS / x86_64",
    "windows": "Windows / x86_64 Vulkan",
    "android": "Android / arm64-v8a Vulkan",
    "allplatformsgreen": "All platform legs succeeded",
    "publish": "Publish release assets",
    "nativetests": "Build native test targets",
    "dartanalyze": "dart analyze (compile-only)",
}

_KNOWN_JOB_NAMES = frozenset(LEG_TO_JOB_NAME.values())


def _select_leg_text(log_text: str, leg: str) -> str:
    job_name = LEG_TO_JOB_NAME[leg]
    lines = log_text.splitlines()
    is_combined = any(
        line.split("\t", 1)[0].lstrip("﻿") in _KNOWN_JOB_NAMES for line in lines
    )
    if not is_combined:
        # Already a single-job log (no job-name column) -- nothing to filter.
        return log_text
    selected = [line for line in lines if line.split("\t", 1)[0].lstrip("﻿") == job_name]
    return "\n".join(selected)


def main(argv=None) -> int:
    # Make `ci.markerdiff` importable regardless of cwd.
    # __file__ = <repo>/native/scripts/ci/tests/golden/extract_markers.py
    # parents[3] = <repo>/native/scripts
    scripts_dir = Path(__file__).resolve().parents[3]
    sys.path.insert(0, str(scripts_dir))
    from ci import markerdiff  # noqa: E402  (import after sys.path setup)

    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True)
    parser.add_argument("--leg", required=True, choices=sorted(LEG_TO_JOB_NAME))
    args = parser.parse_args(argv)

    log_text = Path(args.log).read_text(encoding="utf-8")
    leg_text = _select_leg_text(log_text, args.leg)
    for line in markerdiff.extract(leg_text):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
