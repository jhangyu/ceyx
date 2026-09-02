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

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_raw_corpus import load_corpus  # noqa: E402

TEST_BINARIES = [
    "test_raw_contract_abi",
    "test_raw_layout_contract",
    "test_raw_file_router",
    "test_libraw_frontend",
    "test_libraw_adapter",
    "test_raw_render_params",
    "test_raw_auto_exposure",
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
                        default="native/tests/raw_corpus_manifest.json")
    parser.add_argument("--build-dir", default="native/build")
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
              [sys.executable, "native/scripts/verify_raw_provenance.py"])
    cases.append("provenance")

    ok &= run("corpus",
              [sys.executable, "native/tests/verify_raw_corpus.py",
               "--manifest", args.manifest])
    cases.append("corpus")

    ok &= run("architecture-gates",
              [sys.executable, "native/scripts/check_raw_architecture_gates.py"])
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

    # Scaled decode gate for the LibRaw path (contract AC-2). The binary is
    # mandatory (a missing binary is silent coverage loss); the SAMPLES are
    # owner-supplied and untracked, so an absent-sample run is a SKIP, not a
    # failure — the harness returns 2 for "no usable file", which we tolerate.
    sized_bin = build_dir / "test_raw_sized_decode"
    if not sized_bin.is_file():
        print("[RawMatrix] %-28s -> FAIL (binary missing: %s)"
              % ("raw-sized-decode", sized_bin))
        ok = False
    else:
        raw_layouts = {"bayer2x2", "xtrans6x6", "linear_rgb"}
        sized_files = [
            s["path"] for s in samples
            if s.get("expect_error") == "kRawSuccess"
            and s.get("expect_layout") in raw_layouts
            and s.get("extension") != "dng"
            and (REPO / s["path"]).is_file()
        ]
        if not sized_files:
            print("[RawMatrix] %-28s -> SKIP (no raw sample present)"
                  % "raw-sized-decode")
        else:
            proc = subprocess.run([str(sized_bin), *sized_files],
                                  cwd=str(REPO), capture_output=True, text=True)
            status = "PASS" if proc.returncode == 0 else \
                ("SKIP" if proc.returncode == 2 else "FAIL")
            print("[RawMatrix] %-28s rc=%d -> %s"
                  % ("raw-sized-decode", proc.returncode, status))
            if proc.returncode not in (0, 2):
                sys.stdout.write(proc.stdout[-4000:])
                sys.stderr.write(proc.stderr[-4000:])
                ok = False
    cases.append("raw-sized-decode")

    if args.skip_dng:
        print("[RawMatrix] WARNING --skip-dng was used; this is NOT a gate run")
        if not ok:
            print("[RawMatrix] FAIL (%d cases attempted)" % len(cases))
            return 1
        print("[RawMatrix] ALL PASS (%d cases, DNG-REGRESSION-SKIPPED)" % len(cases))
        return 2

    ok &= run("dng-regression",
              [sys.executable, "native/tests/run_decode_matrix.py",
               "--repeat", str(args.dng_repeat)])
    cases.append("dng-regression")

    if not ok:
        print("[RawMatrix] FAIL (%d cases attempted)" % len(cases))
        return 1
    print("[RawMatrix] ALL PASS (%d cases)" % len(cases))
    return 0


if __name__ == "__main__":
    sys.exit(main())
