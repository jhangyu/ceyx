#!/usr/bin/env python3
"""Drift gate: measured MIN_RUNTIME_<platform> == declared value (WI-14 S-F3).

Spec: Halcyon/docs/logs/2026-09-12/platform-parity-plan.md, WI-14 step 14.4.

The pin lives in the Halcyon repo, which ceyx's own CI does not check out, so
ceyx compares against its OWN copy of the expected floor
(native/deps/min_runtime_expected.toml, WI-14 step 14.4) instead. A floor
change without updating that declaration fails HERE, inside ceyx CI; a floor
change without a pin update fails at Halcyon's refresh boundary (14.3) --
neither check alone satisfies S-F3.

Usage:
    assert_min_runtime_matches_declared.py --emitted min_runtime.txt \
        --declared native/deps/min_runtime_expected.toml --platform windows

Reads the MIN_RUNTIME_<platform>=<value> line from --emitted (the file
native/scripts/read_min_runtime.py's --out writes; scanned line-by-line since
Windows also emits a PROVENANCE= line and macOS an indented breakdown),
compares it against
[<platform>].value in --declared. Prints MIN_RUNTIME_DRIFT_RESULT=PASS|FAIL
naming both values on mismatch, exits non-zero on FAIL or on any parse error
(never silently pass on an unreadable file).
"""
import argparse
import re
import sys
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover
    import tomli as tomllib  # type: ignore

EMITTED_RE = re.compile(r"^MIN_RUNTIME_(\w+)=(.+)$")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emitted", required=True,
                     help="the min_runtime.txt written by read_min_runtime.py's --out")
    ap.add_argument("--declared", required=True,
                     help="path to native/deps/min_runtime_expected.toml")
    ap.add_argument("--platform", required=True)
    args = ap.parse_args()

    emitted_path = Path(args.emitted)
    if not emitted_path.is_file():
        print("MIN_RUNTIME_DRIFT_RESULT=FAIL", file=sys.stderr)
        print(f"error: emitted file not found: {emitted_path}", file=sys.stderr)
        return 1
    measured = None
    for line in emitted_path.read_text().splitlines():
        m = EMITTED_RE.match(line)
        if m and m.group(1) == args.platform:
            measured = m.group(2)
            break
    if measured is None:
        print("MIN_RUNTIME_DRIFT_RESULT=FAIL", file=sys.stderr)
        print(f"error: no MIN_RUNTIME_{args.platform}= line found in {emitted_path}",
              file=sys.stderr)
        return 1

    declared_path = Path(args.declared)
    with declared_path.open("rb") as fh:
        declared_data = tomllib.load(fh)
    table = declared_data.get(args.platform)
    if table is None or "value" not in table:
        print("MIN_RUNTIME_DRIFT_RESULT=FAIL", file=sys.stderr)
        print(f"error: {declared_path} has no [{args.platform}].value entry", file=sys.stderr)
        return 1
    declared = table["value"]

    if measured != declared:
        print("MIN_RUNTIME_DRIFT_RESULT=FAIL")
        print(f"error: measured MIN_RUNTIME_{args.platform}={measured} != "
              f"declared {declared_path}::[{args.platform}].value={declared}", file=sys.stderr)
        return 1

    print(f"MIN_RUNTIME_DRIFT_RESULT=PASS (measured={measured}, declared={declared})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
