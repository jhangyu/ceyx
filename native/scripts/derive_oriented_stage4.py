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
    python3 native/scripts/derive_oriented_stage4.py --selfcheck
    python3 native/scripts/derive_oriented_stage4.py --selfcheck --mutate-coeff  # red
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

# Classes that MUST carry the block.  The Vulkan split kernel joined the set in
# Task 7, so all three are now mandatory and there are no optional classes left:
# an absent block is a hard failure, not a note.
REQUIRED_CLASSES = ("DngRenderStage4", "DngRenderStage4ScaledPreAvg",
                    "DngRenderStage4Android")
OPTIONAL_CLASSES = ()

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


# --- Algebraic self-check of the affine coefficient table (Task 7b) ---------
# The generator no longer decides anything: the host computes six int32
# coefficients (ceyx_orient_affine_coeffs in native/include/ceyx_orient.h) and
# the kernel evaluates ux = a_x*x + b_x*y + c_x, uy = a_y*x + b_y*y + c_y.
# This reimplements that coefficient table in Python and compares the resulting
# affine map against the EXIF table transcribed from ceyx_orient.cpp:104-245,
# for every orientation and every pixel of a non-square frame.  It cannot prove
# the Vulkan lowering is correct (that is the on-device gate) but it does prove
# the COEFFICIENTS are, which is the part a transcription slip would break.
ORIENT_TABLE = {
    1: lambda x, y, w, h: (x, y),
    2: lambda x, y, w, h: (w - 1 - x, y),
    3: lambda x, y, w, h: (w - 1 - x, h - 1 - y),
    4: lambda x, y, w, h: (x, h - 1 - y),
    5: lambda x, y, w, h: (y, x),
    6: lambda x, y, w, h: (y, h - 1 - x),
    7: lambda x, y, w, h: (w - 1 - y, h - 1 - x),
    8: lambda x, y, w, h: (w - 1 - y, x),
}


def affine_coeffs(o: int, uw: int, uh: int) -> "tuple[int, int, int, int, int, int]":
    """Mirror of ceyx_orient_affine_coeffs(); returns (a_x,b_x,c_x,a_y,b_y,c_y).

    Invalid orientations fall through to the identity row, matching the C
    helper's `default:` and ceyx_orient_rgba's "invalid -> 1".
    """
    table = {
        1: (1, 0, 0, 0, 1, 0),
        2: (-1, 0, uw - 1, 0, 1, 0),
        3: (-1, 0, uw - 1, 0, -1, uh - 1),
        4: (1, 0, 0, 0, -1, uh - 1),
        5: (0, 1, 0, 1, 0, 0),
        6: (0, 1, 0, -1, 0, uh - 1),
        7: (0, -1, uw - 1, -1, 0, uh - 1),
        8: (0, -1, uw - 1, 1, 0, 0),
    }
    return table.get(o, (1, 0, 0, 0, 1, 0))


# Red-state control: when set, exactly one coefficient of one orientation is
# perturbed, so --selfcheck MUST report failure.  A check never observed
# failing is not evidence.
MUTATE_COEFF = False


def affine_form(x: int, y: int, o: int, uw: int, uh: int) -> "tuple[int, int]":
    """The kernel's arithmetic: two integer multiply-adds, nothing else."""
    a_x, b_x, c_x, a_y, b_y, c_y = affine_coeffs(o, uw, uh)
    if MUTATE_COEFF and o == 6:
        c_y += 1
    return (a_x * x + b_x * y + c_x, a_y * x + b_y * y + c_y)


def selfcheck() -> int:
    """Exhaustive compare of the affine coefficient table vs the EXIF table."""
    uw, uh = 7, 5          # non-square and both odd, so axis swaps cannot hide
    failures = 0
    for o in range(1, 9):
        # For transposing orientations the output frame is (H, W).
        ow, oh = (uh, uw) if o >= 5 else (uw, uh)
        for y in range(oh):
            for x in range(ow):
                got = affine_form(x, y, o, uw, uh)
                want = ORIENT_TABLE[o](x, y, uw, uh)
                if got != want:
                    failures += 1
                    if failures <= 5:
                        print(f"SELFCHECK FAIL o={o} (x={x},y={y}) "
                              f"got={got} want={want}", file=sys.stderr)
        if failures == 0:
            print(f"SELFCHECK MATCH o={o}")
    # Out-of-range orientations must degrade to the identity, matching
    # ceyx_orient_rgba's "invalid -> 1".
    for o in (-1, 0, 9, 255):
        for y in range(uh):
            for x in range(uw):
                if affine_form(x, y, o, uw, uh) != (x, y):
                    failures += 1
                    print(f"SELFCHECK FAIL invalid o={o} not identity",
                          file=sys.stderr)
                    break
    if failures == 0:
        print("SELFCHECK MATCH invalid->identity")
        print(f"SELFCHECK OK 8/8 orientations exhaustive on {uw}x{uh}")
        print("SELFCHECK FORM=affine (ux=a_x*x+b_x*y+c_x, uy=a_y*x+b_y*y+c_y)")
    else:
        print(f"SELFCHECK FAILURES={failures}", file=sys.stderr)
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="assert every block is byte-identical (exit 1 on drift)")
    ap.add_argument("--selfcheck", action="store_true",
                    help="algebraically verify the affine coefficient table "
                         "against the EXIF table")
    ap.add_argument("--mutate-coeff", action="store_true",
                    help="red-state control: perturb one coefficient so "
                         "--selfcheck must fail")
    ap.add_argument("--print", dest="do_print", action="store_true",
                    help="print the reference block text")
    ap.add_argument("--file", default=DEFAULT_GENERATOR,
                    help="generator source to inspect")
    args = ap.parse_args()

    if not args.check and not args.do_print and not args.selfcheck:
        ap.error("one of --check / --selfcheck / --print is required")

    if args.mutate_coeff:
        global MUTATE_COEFF
        MUTATE_COEFF = True

    if args.selfcheck:
        rc = selfcheck()
        if rc or not (args.check or args.do_print):
            return rc

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
