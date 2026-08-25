#!/usr/bin/env python3
"""Generic RAW gate: provenance, corpus, architecture, every test binary, then
the DNG regression.

A MISSING test binary is a FAILURE, not a skip - silent coverage loss is the
failure mode this runner exists to prevent.

Every exit code comes from CompletedProcess.returncode. Output text is never
parsed to decide pass/fail: a trailing grep or a wrapper can report the wrong
sign, and both directions of that mistake have bitten this project before.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_raw_corpus import load_corpus  # noqa: E402

TEST_BINARIES = [
    "test_raw_contract_abi",
    "test_raw_layout_contract",
    "test_raw_file_router",
    "test_libraw_frontend",
    "test_libraw_adapter",
    "test_raw_render_params",
    "test_raw_bayer_kernel",
    "test_raw_xtrans_kernel",
    "test_raw_linear_rgb_kernel",
    "test_raw_end_to_end",
    "test_raw_hardening",
]
MANIFEST_CONSUMERS = {"test_libraw_frontend", "test_libraw_adapter",
                      "test_raw_end_to_end", "test_raw_hardening"}


def run(name, cmd):
    started = time.monotonic()
    proc = subprocess.run(cmd, cwd=str(REPO), capture_output=True, text=True)
    elapsed = time.monotonic() - started
    status = "PASS" if proc.returncode == 0 else "FAIL"
    print("[RawMatrix] %-28s rc=%d %5.1fs -> %s" % (name, proc.returncode, elapsed, status))
    if proc.returncode != 0:
        sys.stdout.write(proc.stdout[-4000:])
        sys.stderr.write(proc.stderr[-4000:])
    return proc.returncode == 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest",
                        default="dng_processor/native/tests/raw_corpus_manifest.json")
    parser.add_argument("--build-dir", default="dng_processor/native/build")
    parser.add_argument("--dng-repeat", type=int, default=1,
                        help="repeat count forwarded to run_decode_matrix.py's "
                             "--repeat; the RAW test binaries always run once "
                             "by design and are unaffected by this flag")
    parser.add_argument("--skip-dng", action="store_true",
                        help="iteration only; a gate run must not use this. "
                             "Forces a non-zero (2) exit code so the omission "
                             "cannot be mistaken for a passing gate.")
    args = parser.parse_args()

    samples = load_corpus(REPO / args.manifest)
    print("[RawMatrix] manifest lists %d samples" % len(samples))

    cases = []
    ok = True

    ok &= run("provenance",
              [sys.executable, "dng_processor/native/scripts/verify_raw_provenance.py"])
    cases.append("provenance")

    ok &= run("corpus",
              [sys.executable, "dng_processor/native/tests/verify_raw_corpus.py",
               "--manifest", args.manifest])
    cases.append("corpus")

    ok &= run("architecture-gates",
              ["bash", "dng_processor/native/scripts/check_raw_architecture_gates.sh"])
    cases.append("architecture-gates")

    build_dir = REPO / args.build_dir
    for name in TEST_BINARIES:
        binary = build_dir / name
        if not binary.is_file():
            print("[RawMatrix] %-28s -> FAIL (binary missing: %s)" % (name, binary))
            ok = False
            cases.append(name)
            continue
        cmd = [str(binary)]
        if name in MANIFEST_CONSUMERS:
            cmd += ["--manifest", args.manifest]
        ok &= run(name, cmd)
        cases.append(name)

    if args.skip_dng:
        print("[RawMatrix] WARNING --skip-dng was used; this is NOT a gate run")
        if not ok:
            print("[RawMatrix] FAIL (%d cases attempted)" % len(cases))
            return 1
        print("[RawMatrix] ALL PASS (%d cases, DNG-REGRESSION-SKIPPED)" % len(cases))
        return 2

    ok &= run("dng-regression",
              [sys.executable, "dng_processor/native/tests/run_decode_matrix.py",
               "--repeat", str(args.dng_repeat)])
    cases.append("dng-regression")

    if not ok:
        print("[RawMatrix] FAIL (%d cases attempted)" % len(cases))
        return 1
    print("[RawMatrix] ALL PASS (%d cases)" % len(cases))
    return 0


if __name__ == "__main__":
    sys.exit(main())
