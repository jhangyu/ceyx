#!/usr/bin/env python3
"""Full import-closure assertion for a shipped native artifact (WI-4 step 4.3).

Spec: Halcyon/docs/logs/2026-09-12/platform-parity-plan.md, WI-4 step 4.3/4.4.

Generalises the Windows two-name file-existence allowlist (and mirrors
assert_android_so_dt_needed_closure.py's "read a dumped file, never pipe"
contract) into one script shared across PE (Windows), ELF (Linux), and ELF
(Android): for every declared import/DT_NEEDED entry in a dumped dependency
listing, classify it as

  (a) OS_ALLOWLIST -- an explicitly enumerated system library, never a regex
      that could swallow an unknown third-party DLL/SO;
  (b) STAGED       -- present as a file in --staged-dir;
  (c) MISSING      -- neither -- this is the failure the gate exists to catch.

The dump is always a FILE the caller has already captured (dumpbin/llvm-objdump
-p output for PE, readelf -d/llvm-readelf -d output for ELF) -- this script
never shells out and never pipes; that discipline is why the 2026-08-28
SIGPIPE-vs-pipefail trap (nm | grep -q flipping sense) cannot recur here.

Output contract:
  one line per import: "IMPORT <name> -> OS_ALLOWLIST|STAGED|MISSING"
  then: "IMPORT_CLOSURE_RESULT=PASS|FAIL|UNVERIFIED"
  on PASS additionally: "IMPORT_CLOSURE PASS (<platform>): N imports, M staged companions"

An unparseable dump (zero imports found AND the dump does not look like a
valid dump for its format) prints IMPORT_CLOSURE_RESULT=UNVERIFIED and exits
non-zero -- "no imports found" must never silently mean "pass".

Usage:
    assert_import_closure.py --dump <dumped-file> --staged-dir <dir>
        --declaration native/deps/shipped_files.toml --platform windows --format pe
"""
import argparse
import re
import sys
from pathlib import Path

try:
    import tomllib  # Python 3.11+
except ModuleNotFoundError:  # pragma: no cover - CI runners are 3.11+
    import tomli as tomllib  # type: ignore

# Explicit, measured allowlists -- named constants, never a regex that would
# swallow an unknown third-party import. Extend only with a measured entry.
WINDOWS_OS_ALLOWLIST = frozenset({
    "KERNEL32.dll",
    "WS2_32.dll",
    "ADVAPI32.dll",
    "USER32.dll",
    "SHELL32.dll",
    "OLE32.dll",
    "MSVCP140.dll",
    "VCRUNTIME140.dll",
    "VCRUNTIME140_1.dll",
    "ucrtbase.dll",
    "ucrtbased.dll",
})
WINDOWS_OS_ALLOWLIST_PREFIXES = ("api-ms-win-",)

LINUX_OS_ALLOWLIST = frozenset({
    "libc.so.6",
    "libm.so.6",
    "libdl.so.2",
    "libpthread.so.0",
    "librt.so.1",
    "libstdc++.so.6",
    "libgcc_s.so.1",
    "ld-linux-x86-64.so.2",
})

ANDROID_OS_ALLOWLIST = frozenset({
    "libc.so",
    "libm.so",
    "libdl.so",
    "liblog.so",
    "libz.so",
    "libc++_shared.so",
})

ALLOWLISTS = {
    "windows": (WINDOWS_OS_ALLOWLIST, WINDOWS_OS_ALLOWLIST_PREFIXES),
    "linux": (LINUX_OS_ALLOWLIST, ()),
    "android": (ANDROID_OS_ALLOWLIST, ()),
}

# dumpbin -dependents / llvm-objdump -p style lines, e.g.:
#   "    heif.dll"
#   "  DLL Name: heif.dll"
PE_IMPORT_RE = re.compile(
    r"^\s*(?:DLL Name:\s*)?([A-Za-z0-9_.\-]+\.dll)\s*$", re.IGNORECASE
)

# readelf -d / llvm-readelf -d style lines, e.g.:
#  0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
ELF_NEEDED_RE = re.compile(r"\(NEEDED\).*Shared library:\s*\[([^\]]+)\]")


def parse_pe_dump(text: str) -> list[str]:
    names: list[str] = []
    for line in text.splitlines():
        # Skip the "Dump of file ..." header line -- load-bearing strip,
        # the 2026-08-28 self-match shape (a header naming the dumped file
        # itself must never be counted as an import of itself).
        if "Dump of file" in line:
            continue
        # STOP at the Export Table (llvm-objdump/objdump -p print the
        # decoder's own export table after its import tables): a real fetch
        # of the current v0.1.23 release (U-10) proved this is not
        # theoretical -- "DLL name: dng_decoder_native.dll" in the export
        # table header matched the same import-line pattern, self-matching
        # the dumped file (the same self-match shape as the header strip
        # above, one layer deeper). Import tables are always emitted before
        # the export table by both tools, so a hard stop here is safe.
        if line.strip().startswith("Export Table:"):
            break
        m = PE_IMPORT_RE.match(line)
        if m:
            names.append(m.group(1))
    return names


def parse_elf_dump(text: str) -> list[str]:
    names: list[str] = []
    for line in text.splitlines():
        m = ELF_NEEDED_RE.search(line)
        if m:
            names.append(m.group(1))
    return names


def load_declared_companions(declaration_path: Path, platform: str) -> set[str]:
    with declaration_path.open("rb") as fh:
        data = tomllib.load(fh)
    table = data.get(platform, {})
    companions = set(table.get("companions", []))
    decoder = table.get("decoder")
    if decoder:
        companions.add(decoder)
    return companions


def classify(name: str, allowlist: frozenset[str], prefixes: tuple[str, ...],
             staged_names: set[str]) -> str:
    if name in allowlist or any(name.lower().startswith(p.lower()) for p in prefixes):
        return "OS_ALLOWLIST"
    if name in staged_names:
        return "STAGED"
    return "MISSING"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", required=True, help="path to the already-captured dump file")
    ap.add_argument("--staged-dir", required=True,
                     help="directory containing the staged companion files")
    ap.add_argument("--declaration", required=True,
                     help="path to native/deps/shipped_files.toml")
    ap.add_argument("--platform", required=True, choices=sorted(ALLOWLISTS.keys()))
    ap.add_argument("--format", required=True, choices=("pe", "elf"))
    args = ap.parse_args()

    dump_path = Path(args.dump)
    if not dump_path.is_file():
        print(f"IMPORT_CLOSURE_RESULT=UNVERIFIED")
        print(f"error: dump file not found: {dump_path}", file=sys.stderr)
        return 1
    text = dump_path.read_text(errors="replace")

    if args.format == "pe":
        imports = parse_pe_dump(text)
    else:
        imports = parse_elf_dump(text)

    if not imports:
        # Never treat "no imports found" as a pass -- an empty or garbage
        # dump is UNVERIFIED, not a silent green.
        print("IMPORT_CLOSURE_RESULT=UNVERIFIED")
        print(f"error: no imports parsed from {dump_path} (format={args.format})",
              file=sys.stderr)
        return 1

    staged_dir = Path(args.staged_dir)
    staged_names = {p.name for p in staged_dir.iterdir()} if staged_dir.is_dir() else set()

    # The declaration is read for its own sake -- a companion classified as
    # STAGED must also be one of the platform's DECLARED members, so a file
    # that happens to be sitting in --staged-dir for an unrelated reason
    # cannot silently satisfy closure for a name shipped_files.toml never
    # named. Deliberately NOT unioned into staged_names: presence on disk
    # alone is what "STAGED" means, but disk presence of an undeclared file
    # never rescues a MISSING declared companion (loud logging only, keeps
    # the check strict against the source of truth).
    declaration_path = Path(args.declaration)
    declared = load_declared_companions(declaration_path, args.platform)
    undeclared_staged = sorted(staged_names - declared)
    if undeclared_staged:
        print(f"note: staged-dir contains undeclared files: {', '.join(undeclared_staged)}",
              file=sys.stderr)

    allowlist, prefixes = ALLOWLISTS[args.platform]

    missing: list[str] = []
    staged_count = 0
    for name in imports:
        verdict = classify(name, allowlist, prefixes, staged_names)
        print(f"IMPORT {name} -> {verdict}")
        if verdict == "STAGED":
            staged_count += 1
        elif verdict == "MISSING":
            missing.append(name)

    if missing:
        print("IMPORT_CLOSURE_RESULT=FAIL")
        print(f"error: unresolved imports: {', '.join(missing)}", file=sys.stderr)
        return 1

    print("IMPORT_CLOSURE_RESULT=PASS")
    print(f"IMPORT_CLOSURE PASS ({args.platform}): "
          f"{len(imports)} imports, {staged_count} staged companions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
