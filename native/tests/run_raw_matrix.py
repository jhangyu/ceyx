#!/usr/bin/env python3
"""Generic RAW gate: provenance, corpus, architecture, every test binary, then
the DNG regression.

A MISSING test binary is a FAILURE, not a skip - silent coverage loss is the
failure mode this runner exists to prevent.

Pass/fail/skip of every case comes from CompletedProcess.returncode
(0 = pass, 2 = ran but incomplete -> skip, anything else = fail). Output text
is parsed for exactly one thing: a harness's "[<X> SUMMARY] ... skipped=<m>"
line, forwarded as a declared per-sample skip count (owner-supplied corpus).

Exit code (repo-wide convention): 0 = everything executed and passed,
1 = a case failed, 2 = a non-declared case was skipped (incomplete run).
The last stdout line is "[RawMatrix SUMMARY] executed=<n> skipped=<m> failed=<k>".
"""
import argparse
import re
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


# The callee runner's own "[MATRIX SUMMARY]" is excluded: its skips are
# already judged by its exit code (2 -> SKIP above); forwarding them would
# also list them as declared.
_SUMMARY_LINE_PATTERN = re.compile(
    r"^\[(?!MATRIX )[^\]]+ SUMMARY\] executed=\d+ skipped=(\d+) failed=\d+",
    re.MULTILINE)

executed_cases: list[str] = []
failed_cases: list[str] = []
skipped_cases: list[str] = []           # non-declared -> exit 2
declared_skip_counts: list[str] = []    # "<binary>:<count>" harness-internal


def _status_for(return_code: int) -> str:
    if return_code == 0:
        return "PASS"
    if return_code == 2:
        return "SKIP"
    return "FAIL"


def run(name, command):
    """Run one case; record it; return "PASS" | "FAIL" | "SKIP"."""
    started = time.monotonic()
    proc = subprocess.run(command, cwd=str(REPO), capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    elapsed = time.monotonic() - started
    status = _status_for(proc.returncode)
    print("[RawMatrix] %-28s rc=%d %5.1fs -> %s" % (name, proc.returncode, elapsed, status))
    if status != "PASS":
        sys.stdout.write(proc.stdout[-4000:])
        sys.stderr.write(proc.stderr[-4000:])
    summary = _SUMMARY_LINE_PATTERN.findall(proc.stdout)
    if summary and int(summary[-1]) > 0:
        declared_skip_counts.append("%s:%s" % (name, summary[-1]))
    if status == "PASS":
        executed_cases.append(name)
    elif status == "SKIP":
        skipped_cases.append(name)
    else:
        failed_cases.append(name)
    return status


def _finish():
    if declared_skip_counts:
        print("[RawMatrix DECLARED] count=%d cases=%s"
              % (len(declared_skip_counts), ",".join(declared_skip_counts)))
    if failed_cases:
        print("[RawMatrix] FAIL (failed: %s)" % ",".join(failed_cases))
    elif not skipped_cases:
        print("[RawMatrix] ALL PASS (executed=%d, skipped=0)" % len(executed_cases))
    line = "[RawMatrix SUMMARY] executed=%d skipped=%d failed=%d" % (
        len(executed_cases), len(skipped_cases), len(failed_cases))
    if skipped_cases:
        line += " skipped_cases=" + ",".join(skipped_cases)
    print(line, flush=True)
    if failed_cases:
        return 1
    if skipped_cases or not executed_cases:
        return 2
    return 0


def _binary(build_dir, name):
    exe = build_dir / (name + ".exe")
    return exe if exe.is_file() else build_dir / name


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
                             "Records dng-regression as a skipped case, so the "
                             "run exits 2 and cannot be mistaken for a gate.")
    args = parser.parse_args()

    samples = load_corpus(REPO / args.manifest)
    print("[RawMatrix] manifest lists %d samples" % len(samples))

    run("provenance", [sys.executable, "native/scripts/verify_raw_provenance.py"])
    run("corpus", [sys.executable, "native/tests/verify_raw_corpus.py",
                   "--manifest", args.manifest])
    run("architecture-gates",
        [sys.executable, "native/scripts/check_raw_architecture_gates.py"])

    build_dir = REPO / args.build_dir
    for name in TEST_BINARIES:
        binary = _binary(build_dir, name)
        if not binary.is_file():
            print("[RawMatrix] %-28s -> FAIL (binary missing: %s)" % (name, binary))
            failed_cases.append(name)
            continue
        command = [str(binary)]
        if name in MANIFEST_CONSUMERS:
            command += ["--manifest", args.manifest]
        run(name, command)

    # Scaled decode gate for the LibRaw path (contract AC-2). The binary is
    # mandatory (a missing binary is silent coverage loss). Owner-supplied
    # samples absent, or harness exit 2 ("no usable file"), is a non-declared
    # SKIP: visible, counted, and it makes this run exit 2.
    sized_binary = _binary(build_dir, "test_raw_sized_decode")
    if not sized_binary.is_file():
        print("[RawMatrix] %-28s -> FAIL (binary missing: %s)"
              % ("raw-sized-decode", sized_binary))
        failed_cases.append("raw-sized-decode")
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
            print("[RawMatrix] %-28s -> SKIP reason=no-raw-sample-present"
                  % "raw-sized-decode")
            skipped_cases.append("raw-sized-decode")
        else:
            run("raw-sized-decode", [str(sized_binary), *sized_files])

    if args.skip_dng:
        print("[RawMatrix] WARNING --skip-dng was used; this is NOT a gate run")
        print("[RawMatrix] %-28s -> SKIP reason=skip-dng-flag" % "dng-regression")
        skipped_cases.append("dng-regression")
        return _finish()

    run("dng-regression",
        [sys.executable, "native/tests/run_decode_matrix.py",
         "--repeat", str(args.dng_repeat)])
    return _finish()


if __name__ == "__main__":
    sys.exit(main())
