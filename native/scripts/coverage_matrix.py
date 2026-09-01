#!/usr/bin/env python3
"""Render a human-readable codec coverage matrix from the expectation ledger.

Spec: docs/logs/2026-09-01/contract-ci-completion-round.md, CI-T9.

This is a thin reporting layer over render_expectations.load_ledger() -- it
does not parse the ledger itself, so it can never drift from the same
validation (bare-0 rejection, reason/owner discipline) that
render_expectations.py and ci_conventions_check.py's C8 already enforce.

Output is a Markdown table: rows are legs, columns are "<format>:<direction>"
pairs, cells are "1", "0 (<instrument>)" for an honest gap. Legs whose
`instrument` starts with "none" (currently only android-arm64-v8a) render
every cell as "0 (none (accepted gap))" -- the ledger's own instrument
string, unedited, so the matrix can never show a friendlier value than the
ledger actually records.
"""
import argparse
import sys

from render_expectations import load_ledger, LedgerError


def build_matrix(legs):
    """Return (columns, rows) where rows is [(leg, {column: cell_text})]."""
    columns = sorted({pair for table in legs.values() for pair in table["expect"]})
    rows = []
    for leg in sorted(legs):
        table = legs[leg]
        cells = {}
        for col in columns:
            value = table["expect"].get(col)
            if value is None:
                cells[col] = "n/a"
            elif value == 1:
                cells[col] = "1"
            else:
                cells[col] = f"0 ({table['instrument']})"
        rows.append((leg, cells))
    return columns, rows


def render_markdown(columns, rows):
    header = "| leg | " + " | ".join(columns) + " |"
    sep = "|---" * (len(columns) + 1) + "|"
    lines = [header, sep]
    for leg, cells in rows:
        lines.append("| " + leg + " | " + " | ".join(cells[c] for c in columns) + " |")
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ledger", metavar="PATH", default=None, help="Ledger path (default: render_expectations default)")
    ap.add_argument("--out", metavar="PATH", default=None, help="Write the matrix here instead of stdout")
    args = ap.parse_args(argv)

    try:
        legs = load_ledger(args.ledger)
    except (LedgerError, FileNotFoundError) as exc:
        print(f"::error::{exc}", file=sys.stderr)
        return 1

    columns, rows = build_matrix(legs)
    text = render_markdown(columns, rows)

    if args.out:
        with open(args.out, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)

    return 0


if __name__ == "__main__":
    sys.exit(main())
