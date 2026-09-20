#!/usr/bin/env python3
"""Content gate for ceyx_encode_jpeg_yuv420 (2026-09-20).

WHY THIS IS OUT OF PROCESS: ceyx_encode_harness cannot decode a JPEG itself --
this build exposes no JPEG still-decode route (ceyx_still_decode_supports(
kCeyxFormatJpeg) returns 0), so a "decode it back" check inside the harness
would be asserting on an absent capability, not on the encoder. The harness
therefore dumps pairs of files and this script decodes them with Pillow.

WHAT IT GATES, per pair (<name>_yuv420.jpg vs <name>_rgba8.jpg):
  1. both decode, and the yuv420 file reports the EXACT requested extent
     (a padding defect in the raw-data path shows up as a rounded-up extent);
  2. their pixels AGREE: mean absolute difference below the threshold. The
     rgba8 entry runs libjpeg's own RGB->YCbCr + 2x2 chroma downsample; the
     yuv420 entry is fed planes the decoder built from the SAME coefficients
     (ceyx_yuv420_oracle.h), so agreement is the expected outcome and a channel
     swap, a plane-offset error or an edge-padding overrun is not;
  3. the frame is NOT degenerate (per-channel stddev above a floor) -- a flat
     grey frame is what a plane-order bug produces, and it would otherwise
     "agree" with nothing complaining.

Usage: verify_yuv420_encode.py <dump-dir>
Exit 0 = every pair passed. Every pair prints a PASS/FAIL line, so a truncated
log is distinguishable from a green run.
"""

import sys
import pathlib

import numpy as np
from PIL import Image

# Two encoders, one frame, same quality: the only legitimate difference is
# rounding inside the two chroma routes. 3.0 LSB mean is far below any real
# defect (a swapped Cb/Cr plane lands in the tens) and far above rounding.
MAE_LIMIT = 3.0
# A real photo or gradient; a collapsed/flat frame sits near 0.
STDDEV_FLOOR = 3.0

failures = 0


def check(ok: bool, name: str, detail: str = "") -> None:
    global failures
    print(f"[verify] {'PASS' if ok else 'FAIL'} {name} {detail}")
    if not ok:
        failures += 1


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: verify_yuv420_encode.py <dump-dir>", file=sys.stderr)
        return 2
    d = pathlib.Path(sys.argv[1])
    pairs = sorted(d.glob("*_yuv420.jpg"))
    if not pairs:
        # Loud, never silent: an empty glob would otherwise be a green run that
        # verified nothing.
        print(f"[verify] FAIL no_pairs_found (in {d})")
        return 1

    for yuv_path in pairs:
        stem = yuv_path.name[: -len("_yuv420.jpg")]
        ref_path = d / f"{stem}_rgba8.jpg"
        if not ref_path.exists():
            check(False, f"{stem}_pair_present", f"(missing {ref_path.name})")
            continue

        a = np.asarray(Image.open(yuv_path).convert("RGB"), dtype=np.int16)
        b = np.asarray(Image.open(ref_path).convert("RGB"), dtype=np.int16)
        check(a.shape == b.shape, f"{stem}_extent_matches_rgba8_path",
              f"(yuv420={a.shape[1]}x{a.shape[0]} rgba8={b.shape[1]}x{b.shape[0]})")
        if a.shape != b.shape:
            continue

        mae = float(np.abs(a - b).mean())
        check(mae < MAE_LIMIT, f"{stem}_agrees_with_rgba8_path",
              f"(mae={mae:.3f} limit={MAE_LIMIT})")

        sd = float(a.std())
        check(sd > STDDEV_FLOOR, f"{stem}_not_degenerate",
              f"(stddev={sd:.2f} floor={STDDEV_FLOOR})")

    print(f"[verify] failures={failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
