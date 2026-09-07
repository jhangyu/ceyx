#!/usr/bin/env python3
"""Derivation checker for the fused EXIF-orientation permutation block.

Productionization plan (docs/logs/2026-09-07/gpu_orient_productionization_plan.md)
Task 1 Step 1.6.

What this asserts
-----------------
The store-stage permutation (plan §1.2) must be BYTE-IDENTICAL text in every
Stage4 generator class that carries it.  That identity is what makes the fusion
a mechanical derivation rather than three hand-written permutations that can
drift apart.

Block delimiters (both lines are part of the contract, neither is part of the
compared text):

    // Fused EXIF orientation:                      <- start marker (prefix)
    ... compared text ...
    // ---- end fused orientation permutation ----  <- end marker

DEVIATION FROM THE PLAN TEXT (recorded deliberately): the plan's Step 1.6 named
`Expr s_r` as the end marker.  That cannot work, because the two classes differ
in what immediately follows the permutation:

  * `DngRenderStage4` clamps against the SOURCE extents and loads pixels;
  * `DngRenderStage4ScaledPreAvg` clamps against the UNORIENTED OUTPUT extents
    (`out_w`/`out_h`) because the permuted coordinate drives the box-filter cell
    geometry, not a direct pixel load.

Clamping the scaled kernel's output coordinate against the source extents would
be a silent no-op for every downscale (src >= out), i.e. an assertion that
cannot fail.  So the shared text ends at `uy` and each class supplies its own
clamp, which is the largest block that is genuinely identical.

Usage
-----
    python3 native/scripts/derive_oriented_stage4.py --check
    python3 native/scripts/derive_oriented_stage4.py --print
"""

from __future__ import annotations

import argparse
import difflib
import os
import re
import sys

DEFAULT_GENERATOR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "generators",
    "DngRenderGenerator.cpp",
)

START_MARKER = "// Fused EXIF orientation:"
END_MARKER = "// ---- end fused orientation permutation ----"

# Classes that MUST carry the block once their task has landed.  The Vulkan
# split kernel lands in Task 7; until then it is reported as absent, which is
# informational, not a failure.
REQUIRED_CLASSES = ("DngRenderStage4", "DngRenderStage4ScaledPreAvg")
OPTIONAL_CLASSES = ("DngRenderStage4Android",)

CLASS_RE = re.compile(r"^class\s+(\w+)\s*:\s*public\s+Halide::Generator", re.M)


def class_spans(text: str) -> "dict[str, tuple[int, int]]":
    """Map class name -> (start_offset, end_offset) in `text`."""
    matches = list(CLASS_RE.finditer(text))
    spans = {}
    for i, m in enumerate(matches):
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        spans[m.group(1)] = (m.start(), end)
    return spans


def extract_block(body: str) -> "str | None":
    """Return the permutation text between the markers, or None if absent."""
    lines = body.splitlines()
    start = None
    for i, line in enumerate(lines):
        if line.strip().startswith(START_MARKER):
            start = i
            break
    if start is None:
        return None
    for j in range(start + 1, len(lines)):
        if lines[j].strip() == END_MARKER:
            return "\n".join(lines[start + 1 : j]) + "\n"
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="assert every block is byte-identical (exit 1 on drift)")
    ap.add_argument("--print", dest="do_print", action="store_true",
                    help="print the reference block text")
    ap.add_argument("--file", default=DEFAULT_GENERATOR,
                    help="generator source to inspect")
    args = ap.parse_args()

    if not args.check and not args.do_print:
        ap.error("one of --check / --print is required")

    with open(args.file, "r", encoding="utf-8") as fh:
        text = fh.read()

    spans = class_spans(text)
    blocks = {}
    missing_required = []
    for name in REQUIRED_CLASSES + OPTIONAL_CLASSES:
        if name not in spans:
            if name in REQUIRED_CLASSES:
                missing_required.append(f"class {name} not found in {args.file}")
            continue
        lo, hi = spans[name]
        blk = extract_block(text[lo:hi])
        if blk is None:
            if name in REQUIRED_CLASSES:
                missing_required.append(
                    f"class {name} carries no permutation block "
                    f"(markers: {START_MARKER!r} .. {END_MARKER!r})")
            else:
                print(f"DERIVATION ABSENT {name} (optional; lands in Task 7)")
            continue
        blocks[name] = blk

    if args.do_print:
        if not blocks:
            print("no permutation block found", file=sys.stderr)
            return 1
        sys.stdout.write(next(iter(blocks.values())))
        if not args.check:
            return 0

    for msg in missing_required:
        print(f"DERIVATION FAIL: {msg}", file=sys.stderr)
    if missing_required:
        return 1

    ref_name, ref_blk = next(iter(blocks.items()))
    failed = False
    for name, blk in blocks.items():
        if blk == ref_blk:
            print(f"DERIVATION MATCH {name}")
        else:
            failed = True
            print(f"DERIVATION FAIL: {name} differs from {ref_name}",
                  file=sys.stderr)
            sys.stderr.writelines(difflib.unified_diff(
                ref_blk.splitlines(keepends=True),
                blk.splitlines(keepends=True),
                fromfile=ref_name, tofile=name))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
