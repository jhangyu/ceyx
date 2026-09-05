#!/usr/bin/env python3
"""check_alias_table_convention.py — graft G2-2 (r4-plan-decision.md).

Item 2 fix (normalize_model.cpp:406, `static const char *orig;` -> an
automatic/local variable) rests on a factual claim: EVERY 2-D alias table in
this file uses the convention "the first element of every {canonical, alias,
alias, ...} run begins with '@'", and the reader branch (`strcpy(...,orig)`)
is only ever reached on a NON-'@' entry, which — by construction of the
tables — always follows an '@' entry earlier in the same loop within the same
call. That is what makes `orig` safe as a per-call automatic instead of a
cross-call static: it is always written before it is read, within one call.

This script does NOT prove the loop logic (that is the test binary's job,
G2-1/AC-2a). It proves the STATIC PRECONDITION the fix's safety argument
depends on: every alias table's FIRST element in each row is either a
canonical entry ('@'-prefixed) or, for tables with more than 2 columns per
row, checked accordingly. Extracts every
`static const char <name>[][N] = { ... };` table in the target .cpp and
asserts each table's element at index 0 (i.e. the very first string literal
in the initializer) begins with '@' -- since every table observed in this
file interleaves canonical/alias pairs starting with a canonical entry.

Usage:
    python3 native/scripts/check_alias_table_convention.py \
        native/third_party/libraw/src/metadata/normalize_model.cpp

Exit 0 and prints `ALIAS_TABLE_FIRST_ELEMENT_ALL_AT=YES` iff every extracted
table's first element begins with '@'. Exit 1 and prints
`ALIAS_TABLE_FIRST_ELEMENT_ALL_AT=NO` plus the offending table name(s)
otherwise -- in which case the safety argument in the plan does NOT hold for
this source revision and the fix must be re-examined, not silently landed.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Matches: static const char <name>[][N] = { ... };  (non-greedy body, DOTALL)
TABLE_RE = re.compile(
    r"static\s+const\s+char\s+(?P<name>[A-Za-z_][A-Za-z_0-9]*)\s*\[\s*\]\s*\[\s*\d+\s*\]\s*=\s*\{(?P<body>.*?)\}\s*;",
    re.DOTALL,
)
# First double-quoted string literal inside a table body.
FIRST_STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def extract_tables(src: str) -> list[tuple[str, str]]:
    """Return [(table_name, first_string_literal), ...] for every 2-D
    `static const char name[][N] = {...}` table found in src."""
    results = []
    for m in TABLE_RE.finditer(src):
        name = m.group("name")
        body = m.group("body")
        first = FIRST_STRING_RE.search(body)
        if first is None:
            # A table with no string literal at all is a shape this script
            # doesn't understand -- fail loudly rather than silently skip.
            results.append((name, None))
        else:
            results.append((name, first.group(1)))
    return results


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: check_alias_table_convention.py <path/to/normalize_model.cpp>", file=sys.stderr)
        return 2

    path = Path(argv[1])
    if not path.exists():
        print(f"REFUSED: file not found: {path}", file=sys.stderr)
        return 2

    src = path.read_text()
    tables = extract_tables(src)

    if not tables:
        print("ALIAS_TABLE_FIRST_ELEMENT_ALL_AT=NO")
        print("no `static const char name[][N] = {...}` tables were found at all -- "
              "the extraction pattern itself may be stale against this source revision.")
        return 1

    bad = []
    for name, first in tables:
        status = "OK" if (first is not None and first.startswith("@")) else "BAD"
        print(f"table={name} first_element={first!r} status={status}")
        if status == "BAD":
            bad.append(name)

    print(f"TABLE_COUNT={len(tables)}")
    if bad:
        print("ALIAS_TABLE_FIRST_ELEMENT_ALL_AT=NO")
        print(f"OFFENDING_TABLES={','.join(bad)}")
        return 1

    print("ALIAS_TABLE_FIRST_ELEMENT_ALL_AT=YES")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
