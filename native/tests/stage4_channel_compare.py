#!/usr/bin/env python3
"""Stage-4 per-channel hard gate: byte-exact comparison of two
interleaved RGB8 raw dumps (6000x4000x3 by default).

Usage:
  stage4_channel_compare.py <old.raw> <new.raw> [--width W --height H]

Prints per-channel mismatch counts (the pre-check lesson: aggregate PSNR
masks channel collapse — Gotcha #92/#96 family shows up as millions of G/B
mismatches). Exit 0 iff every channel has 0 mismatches.
"""
import argparse
import sys

import numpy as np


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("--width", type=int, default=6000)
    ap.add_argument("--height", type=int, default=4000)
    args = ap.parse_args()

    n = args.width * args.height * 3
    old = np.fromfile(args.old, dtype=np.uint8)
    new = np.fromfile(args.new, dtype=np.uint8)
    if old.size != n or new.size != n:
        print(f"[stage4-channel-cmp] SIZE MISMATCH old={old.size} new={new.size} expected={n}")
        return 1

    old = old.reshape(-1, 3)
    new = new.reshape(-1, 3)
    total = old.shape[0]
    ok = True
    for ch, name in enumerate("RGB"):
        diff = old[:, ch] != new[:, ch]
        mism = int(diff.sum())
        maxd = int(np.abs(old[:, ch].astype(np.int16) - new[:, ch].astype(np.int16)).max())
        print(f"[stage4-channel-cmp channel] {name} mismatches={mism}/{total} max_abs_diff={maxd}")
        if mism != 0:
            ok = False
    # Channel-collapse tripwire (R==G aliasing family): the alias-pixel count
    # must be identical between the two dumps.
    alias_old = int((old[:, 0] == old[:, 1]).sum())
    alias_new = int((new[:, 0] == new[:, 1]).sum())
    print(f"[stage4-channel-cmp alias] R==G pixels old={alias_old} new={alias_new}")
    if alias_old != alias_new:
        ok = False
    print(f"[stage4-channel-cmp VERDICT] {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
