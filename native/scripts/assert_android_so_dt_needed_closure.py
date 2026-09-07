#!/usr/bin/env python3
"""DT_NEEDED closure assertion for the packaged Android .so set.

CI hardening added after the T8b live-load episode
(tmp/verify/orient_prod_t8b_so_refresh.txt, device PHY110 serial cc5bf709):
android_build.yml's "Fail if the required Android .so set is incomplete"
step only checked that a FIXED list of files existed, never that every
transitive DT_NEEDED entry of every packaged .so was actually satisfiable
on-device. The shipped decoder carried a libomp.so DT_NEEDED that Android
does not provide -- a real device dlopen() is what caught it (root-caused
and fixed in 68dd3a5: OpenMP was linked in violating the desktop-ON /
mobile-OFF policy). This script generalises that catch into a standing gate:
for EVERY .so file in a directory, every DT_NEEDED entry must be explained
by either the bundled set (shipped alongside it) or a whitelist of libraries
the Android platform itself provides.

The whitelist is a MEASUREMENT, not an assumption -- assuming libomp.so was
system-provided is exactly the mistake that let the bug through undetected
for weeks (the same misclassification was recorded, at the time, for
libz.so too -- see the DT_NEEDED table in orient_prod_t8b_so_refresh.txt
around "SYSTEM libz.so (provided by the platform/NDK)", written before the
libomp.so miss was caught, i.e. from the same unverified-assumption pass).
DEFAULT_SYSTEM_WHITELIST below was instead built from a real
`adb shell ls /system/lib64/<name>` run against that same device
(cc5bf709 / PHY110) -- see tmp/verify/orient_prod_t11c_device_whitelist.txt
for the raw output this list transcribes. Do not add a name to this default
without a matching measurement recorded the same way (device `ls`, or if no
device is available, the NDK's own sysroot listing with a comment saying so
explicitly -- never a plausibility guess).

Usage:
    assert_android_so_dt_needed_closure.py \
        --readelf <path-to-llvm-readelf> \
        --so-dir <directory containing the packaged .so files> \
        --bundled libheif.so,libde265.so,libdng_decoder_native.so[,libc++_shared.so] \
        [--whitelist name1,name2,...]   # overrides DEFAULT_SYSTEM_WHITELIST; for
                                          # local negative-control testing ONLY --
                                          # CI must never pass this flag, so the
                                          # measured default is always what runs there.

Prints one line per .so file (name, DT_NEEDED, and any entries outside the
allowed set), then a final `CLOSURE_RESULT=ok` or
`CLOSURE_RESULT=fail:<so>:<missing-lib>[,<so>:<missing-lib>...]` naming
EVERY offending entry, not just the first (this repo's documented failure
class for single-name error reporting).
"""
import argparse
import subprocess
import sys
from pathlib import Path

# Measured on-device (adb shell ls /system/lib64/<name>), device PHY110
# serial cc5bf709, see tmp/verify/orient_prod_t11c_device_whitelist.txt.
# libomp.so and libc++_shared.so were measured ABSENT on that same device
# and must never be added here without a fresh measurement showing present.
DEFAULT_SYSTEM_WHITELIST = frozenset({
    "libc.so",
    "libm.so",
    "libdl.so",
    "liblog.so",
    "libvulkan.so",
    "libandroid.so",
    "libz.so",
})


def dt_needed(readelf: str, so_path: Path) -> list[str]:
    proc = subprocess.run(
        [readelf, "-d", str(so_path)], capture_output=True, text=True
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"{readelf} -d failed on {so_path} (rc={proc.returncode}): {proc.stderr}"
        )
    needed = []
    for line in proc.stdout.splitlines():
        if "(NEEDED)" in line and "Shared library:" in line:
            name = line.split("[", 1)[1].split("]", 1)[0]
            needed.append(name)
    return needed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--readelf", required=True)
    ap.add_argument("--so-dir", required=True)
    ap.add_argument("--bundled", required=True,
                     help="comma-separated bundled .so names shipped alongside the set")
    ap.add_argument(
        "--whitelist",
        default=",".join(sorted(DEFAULT_SYSTEM_WHITELIST)),
        help="comma-separated system .so whitelist; DO NOT override in CI -- "
             "local negative-control testing only",
    )
    args = ap.parse_args()

    bundled = {n.strip() for n in args.bundled.split(",") if n.strip()}
    whitelist = {n.strip() for n in args.whitelist.split(",") if n.strip()}
    allowed = bundled | whitelist

    so_dir = Path(args.so_dir)
    so_files = sorted(so_dir.glob("*.so"))
    if not so_files:
        print(f"CLOSURE_RESULT=fail:no-so-files-in:{so_dir}")
        return 1

    all_missing = []
    for so in so_files:
        try:
            needed = dt_needed(args.readelf, so)
        except RuntimeError as exc:
            print(f"CLOSURE_RESULT=fail:readelf-error:{so.name}")
            print(str(exc), file=sys.stderr)
            return 1
        missing = [n for n in needed if n not in allowed]
        for m in missing:
            all_missing.append(f"{so.name}:{m}")
        print(f"{so.name}: DT_NEEDED={needed} missing={missing}")

    if all_missing:
        print(f"CLOSURE_RESULT=fail:{','.join(all_missing)}")
        return 1
    print("CLOSURE_RESULT=ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
