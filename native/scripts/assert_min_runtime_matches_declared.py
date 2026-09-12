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


class DeclarationError(Exception):
    """The declaration file cannot be resolved to exactly one floor."""


def resolve_declared(declared_data, platform, arch=None):
    """The declared floor for ``platform`` (and ``arch`` where the platform
    declares per-arch subtables). Single source of this resolution rule --
    publish_release.py imports it rather than reimplementing it, so the two
    consumers cannot drift apart.

    Two accepted shapes, deliberately MUTUALLY EXCLUSIVE:
      [macos]        value = "15.0"    -- arch-independent floor
      [macos.arm64]  value = "15.0"    -- arch-dependent floor
      [macos.x86_64] value = "14.0"

    Never falls back. A platform that declares per-arch tables and is queried
    without an arch RAISES, because guessing here is exactly how the x86_64
    leg silently compared itself against the arm64 floor (CI 34697591379).
    """
    table = declared_data.get(platform)
    if table is None:
        raise DeclarationError(f"no [{platform}] entry")
    arch_tables = {k: v for k, v in table.items() if isinstance(v, dict)}
    has_scalar = "value" in table

    if arch_tables and has_scalar:
        raise DeclarationError(
            f"[{platform}] declares BOTH a scalar value and per-arch subtables "
            f"({', '.join(sorted(arch_tables))}) -- ambiguous; a consumer that "
            f"omits the arch would silently read the wrong floor. Declare one "
            f"form or the other."
        )
    if arch_tables:
        if arch is None:
            raise DeclarationError(
                f"[{platform}] is declared PER-ARCH ({', '.join(sorted(arch_tables))}) "
                f"but no arch was supplied -- pass --arch."
            )
        sub = arch_tables.get(arch)
        if sub is None:
            raise DeclarationError(
                f"[{platform}] has no [{platform}.{arch}] entry (declared arches: "
                f"{', '.join(sorted(arch_tables))}) -- add a measured entry rather "
                f"than falling back to another arch's floor."
            )
        if "value" not in sub:
            raise DeclarationError(f"[{platform}.{arch}] has no 'value' key")
        return sub["value"]

    if not has_scalar:
        raise DeclarationError(f"[{platform}] has no 'value' key and no per-arch subtables")
    return table["value"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emitted", required=True,
                     help="the min_runtime.txt written by read_min_runtime.py's --out")
    ap.add_argument("--declared", required=True,
                     help="path to native/deps/min_runtime_expected.toml")
    ap.add_argument("--platform", required=True)
    ap.add_argument("--arch", default=None,
                     help="canonical arch key (arm64 / x86_64, per "
                          "native/deps/arch_map.toml). REQUIRED for a platform "
                          "whose declaration is per-arch, e.g. macos.")
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
    try:
        declared = resolve_declared(declared_data, args.platform, args.arch)
    except DeclarationError as exc:
        print("MIN_RUNTIME_DRIFT_RESULT=FAIL", file=sys.stderr)
        print(f"error: {declared_path}: {exc}", file=sys.stderr)
        return 1

    # Name the key that was ACTUALLY read, not the key the caller's flags
    # suggest: passing --arch to an arch-independent platform must not make
    # the log claim a [platform.arch] table that does not exist.
    _table = declared_data[args.platform]
    _is_per_arch = any(isinstance(v, dict) for v in _table.values())
    key = f"[{args.platform}.{args.arch}]" if _is_per_arch else f"[{args.platform}]"
    if measured != declared:
        print("MIN_RUNTIME_DRIFT_RESULT=FAIL")
        print(f"error: measured MIN_RUNTIME_{args.platform}={measured} != "
              f"declared {declared_path}::{key}.value={declared}", file=sys.stderr)
        return 1

    print(f"MIN_RUNTIME_DRIFT_RESULT=PASS (measured={measured}, "
          f"declared={declared} from {key})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
