#!/usr/bin/env python3
"""Assert that a built artifact's export table matches the Dart-derived
export manifest, for a single platform leg (WI-13, S-G2/S-G4).

Spec: Halcyon/docs/logs/2026-09-12/platform-parity-plan.md, WI-13 step 13.3.

Replaces the per-platform hand-kept allowlist loops in the four workflow
files with one manifest-driven check. The caller is responsible for
capturing the dump to a FILE first (never piping a dumper straight into this
script, and never piping it into `grep` either): `dumpbin -exports`/`nm`/
`llvm-nm` dying of SIGPIPE under `set -euo pipefail` on a SUCCESSFUL match is
the exact bug this convention avoids elsewhere in this repo (see the
pipefail comments in windows_build.yml / linux_build.yml / macos_build.yml /
android_build.yml).

Usage:
    assert_exports.py --manifest native/deps/export_manifest.toml \
        --platform macos --dump dylib_exports.txt

Output: one `SYMBOL <name> -> PRESENT|MISSING` line per applicable symbol,
one `GROUP <id> expected_on=<list> -> PASS|ABSENT|PARTIAL` line per guarded
cluster, then `EXPORTS_RESULT=PASS|FAIL|UNVERIFIED` and
`EXPORTS_CHECKED=<n>`. An empty or unparseable dump is UNVERIFIED with a
non-zero exit -- a dump this parser does not understand must never read as
"no missing symbols".
"""
import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gen_export_manifest as gem  # noqa: E402

KNOWN_PLATFORMS = ["macos", "linux", "windows", "android"]

# Mach-O `nm -gU` / ELF `nm -D` / `llvm-nm` (any platform) all share this
# line shape: <hex address> <type char> <name>. Mach-O names carry a leading
# "_" that is stripped by the caller via NORMALIZED names (see _normalize).
_NM_STYLE_RE = re.compile(r"^[0-9A-Fa-f]+\s+\S\s+(?P<name>\S+)\s*$", re.MULTILINE)

# `dumpbin -exports`: "  <ordinal>  <hint>  <RVA>  <name>[ = <alias>]".
# Anchored on the three leading numeric/hex columns so the header
# ("ordinal hint RVA name") and summary footer never match.
_PE_DUMPBIN_RE = re.compile(
    r"^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(?P<name>\S+)", re.MULTILINE
)


def _normalize(name):
    """Mach-O's C-symbol mangling prefixes every exported name with "_".
    No ceyx symbol name starts with "_", so stripping exactly one leading
    underscore is safe and removes the need for a mac-specific code path."""
    return name[1:] if name.startswith("_") else name


def parse_dump(text, platform):
    """Returns a normalized set of exported names, or None if the dump is
    empty/unparseable (the UNVERIFIED case)."""
    if not text.strip():
        return None

    names = set()
    if platform == "windows":
        names = {m.group("name") for m in _PE_DUMPBIN_RE.finditer(text)}
        if not names:
            # dumpbin unavailable -> the workflow's llvm-nm fallback, same
            # line shape as the POSIX nm-style dumps.
            names = {m.group("name") for m in _NM_STYLE_RE.finditer(text)}
    else:
        names = {m.group("name") for m in _NM_STYLE_RE.finditer(text)}

    if not names:
        return None
    return {_normalize(n) for n in names}


def _applicable(rec, platform):
    platforms = rec["platforms"]
    if platforms == "all":
        return True
    return platform in platforms


def run(manifest_path, platform, dump_text):
    symbols = gem.load_manifest(manifest_path)
    groups = gem.load_groups(manifest_path)

    present = parse_dump(dump_text, platform)
    if present is None:
        print(
            "::error::export dump is empty or unparseable; export presence is "
            "UNVERIFIED for this leg.",
            file=sys.stderr,
        )
        print("EXPORTS_RESULT=UNVERIFIED")
        print("EXPORTS_CHECKED=0")
        return 1

    ok = True
    checked = 0
    by_group = {}

    for name in sorted(symbols):
        rec = symbols[name]
        if not _applicable(rec, platform):
            continue
        checked += 1
        is_present = name in present
        print(f"SYMBOL {name} -> {'PRESENT' if is_present else 'MISSING'}")
        group = rec["group"]
        if group:
            by_group.setdefault(group, []).append(is_present)
        elif not is_present:
            ok = False

    for group_id in sorted(by_group):
        statuses = by_group[group_id]
        expected_on = groups.get(group_id, KNOWN_PLATFORMS)
        expects_this_leg = platform in expected_on
        if all(statuses):
            verdict = "PASS"
        elif not any(statuses):
            verdict = "ABSENT"
            if expects_this_leg:
                ok = False
        else:
            verdict = "PARTIAL"
            ok = False  # a guarded cluster must be all-or-nothing; never silent
        print(
            f"GROUP {group_id} expected_on={sorted(expected_on)} -> {verdict}"
        )

    print(f"EXPORTS_RESULT={'PASS' if ok else 'FAIL'}")
    print(f"EXPORTS_CHECKED={checked}")
    return 0 if ok else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--platform", required=True, choices=KNOWN_PLATFORMS)
    parser.add_argument("--dump", required=True, help="path to a captured export dump FILE")
    args = parser.parse_args(argv)

    dump_path = pathlib.Path(args.dump)
    if not dump_path.exists():
        print(f"::error::dump file {dump_path} does not exist.", file=sys.stderr)
        print("EXPORTS_RESULT=UNVERIFIED")
        print("EXPORTS_CHECKED=0")
        return 1

    dump_text = dump_path.read_text(encoding="utf-8", errors="replace")
    return run(pathlib.Path(args.manifest), args.platform, dump_text)


if __name__ == "__main__":
    sys.exit(main())
