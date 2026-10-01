#!/usr/bin/env python3
"""Every CMake test executable must be listed in native/tests/gates.py with how
it is run (runner / ci-build / manual), so an orphan is mechanically visible.

Exit: 0 = manifest equals the executable set; 1 = mismatch (each printed).
Parses tests.cmake as text: `add_executable(<name>` lines, plus
`foreach(<var> a b c)` bodies that call `add_executable(${<var>}`.
"""
import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gates  # noqa: E402

_ADD_EXECUTABLE = re.compile(r"^\s*add_executable\(\s*([A-Za-z0-9_]+|\$\{\w+\})", re.MULTILINE)
_FOREACH = re.compile(r"^\s*foreach\(\s*(\w+)\s+([^)]*)\)(.*?)^\s*endforeach\(\)",
                      re.MULTILINE | re.DOTALL)
_KINDS = ("runner:", "ci-build:", "manual:")


def executables(cmake_text: str) -> set[str]:
    names = {name for name in _ADD_EXECUTABLE.findall(cmake_text) if not name.startswith("${")}
    for variable, items, body in _FOREACH.findall(cmake_text):
        if re.search(r"add_executable\(\s*\$\{" + variable + r"\}", body):
            names.update(items.split())
    return names


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake-file", default=str(HERE.parent / "cmake" / "tests.cmake"))
    args = parser.parse_args()
    found = executables(Path(args.cmake_file).read_text(encoding="utf-8"))
    problems = []
    for name in sorted(found - gates.GATES.keys()):
        problems.append((name, "not in gates.py"))
    for name in sorted(gates.GATES.keys() - found):
        problems.append((name, "in gates.py but not an executable in tests.cmake"))
    for name, kind in sorted(gates.GATES.items()):
        if not kind.startswith(_KINDS) or kind.split(":", 1)[1].strip() == "":
            problems.append((name, f"bad kind {kind!r}"))
    for name, why in problems:
        print(f"[TestManifest] {name} -> FAIL ({why})")
    executed = len(found)
    print(f"[TestManifest SUMMARY] executed={executed} skipped=0 failed={len(problems)}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
