#!/usr/bin/env python3
"""R1-T1 parallel-decode bench driver.

Pre-registered thresholds and full rationale live in
docs/logs/2026-09-05/parallel-bench-expectations.md — read that file
before touching this one. Summary of the two things NOT obvious from the
CLI:

  * test_concurrent_decode itself emits no wall-clock timing, so wall_ms
    is measured externally in this script with time.perf_counter()
    bracketing the whole subprocess call (mirrors probe_concurrent_raw.py's
    external-timing pattern, since here even the single wall_ms line is
    absent from the target binary's stdout).
  * completions_ms (per-decode completion offsets, for staircase shape) is
    derived from decode_<index>.raw dump mtimes in --out-dir, because the
    binary writes each dump immediately after that decode completes and
    before draining the next queue slot (test_concurrent_decode.cpp:279-288).
    This is filesystem-observable without modifying the binary.

Both metrics MUST come from a single test_concurrent_decode PROCESS
launched with --threads=N (one process, N in-flight decode threads),
never from N separate OS processes — the property under test (Halide's
process-wide Metal serialization) only manifests within one process.

Usage:
  run_parallel_bench.py --binary <path> --corpus <files...> --repeats 5 --out <file>

Exit 0 iff every decode in every timed repeat succeeded. The STAIRCASE /
OVERLAPPED / INCONCLUSIVE verdict is written into --out but never affects
the exit code — per the pre-registration file, "the judge is the reader,
not the script."
"""
import argparse
import os
import statistics
import subprocess
import sys
import time

RATIO_STAIRCASE = 4.0
RATIO_OVERLAPPED = 2.5
REPEATS_DEFAULT = 5


def run_one(binary, out_dir, threads, files):
    """Run one test_concurrent_decode process. Returns (wall_ms, completions_ms, rc)."""
    for name in os.listdir(out_dir):
        if name.startswith("decode_") and (name.endswith(".raw") or name.endswith(".dims")):
            os.remove(os.path.join(out_dir, name))
    cmd = [binary, out_dir, str(threads), *files]
    # Two independent clocks are needed and must not be mixed:
    #   perf_counter() -> monotonic, correct for measuring wall_ms duration,
    #       but its epoch is arbitrary (NOT comparable to file mtimes).
    #   time.time() -> wall-clock/epoch seconds, comparable to os.stat()
    #       st_mtime (also epoch-based), used only for completions_ms offsets.
    t0_perf = time.perf_counter()
    t0_wall = time.time()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True)
    except OSError as exc:
        # e.g. binary does not exist / is not executable — exercised by
        # --self-test-missing-binary. Python raises here rather than
        # returning a nonzero returncode, so surface it as one instead.
        t1_perf = time.perf_counter()
        return (t1_perf - t0_perf) * 1000.0, [], 127, "", str(exc)
    t1_perf = time.perf_counter()
    wall_ms = (t1_perf - t0_perf) * 1000.0
    _ = proc.stdout  # captured for diagnostics, not asserted on

    completions = []
    for i in range(len(files)):
        p = os.path.join(out_dir, f"decode_{i}.raw")
        if os.path.exists(p):
            mtime = os.stat(p).st_mtime
            completions.append((mtime - t0_wall) * 1000.0)
    completions.sort()
    return wall_ms, completions, proc.returncode, proc.stdout, proc.stderr


def median_or_none(xs):
    return statistics.median(xs) if xs else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--corpus", nargs="+", required=True,
                     help="corpus files; index 0 is used alone for the N=1 "
                          "sample, the full list is used for the N=5 sample "
                          "(len(corpus) must be >= 5 for the N=5 sample).")
    ap.add_argument("--repeats", type=int, default=REPEATS_DEFAULT)
    ap.add_argument("--out", required=True)
    ap.add_argument("--self-test-missing-binary", action="store_true",
                     help="instrument-validation mode: run against a "
                          "nonexistent binary and exit non-zero.")
    ap.add_argument("--workdir", default="tmp/parallel_bench_workdir",
                     help="project-relative scratch dir for decode dumps "
                          "(never /tmp — repo hook forbids writes there).")
    args = ap.parse_args()
    os.makedirs(args.workdir, exist_ok=True)

    lines = []
    lines.append(f"TEST_START={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}")

    if args.self_test_missing_binary:
        wall_ms, completions, rc, out, err = run_one(
            "/nonexistent/test_concurrent_decode", args.workdir, 1, [args.corpus[0]])
        lines.append(f"selftest|binary=missing|rc={rc}|stderr={err.strip()!r}")
        with open(args.out, "w") as fh:
            fh.write("\n".join(lines) + "\n")
        if rc == 0:
            sys.exit("FAIL: nonexistent binary unexpectedly returned rc=0")
        print(f"selftest OK: nonexistent binary rc={rc} (non-zero, as expected); "
              f"script itself now exits non-zero too, per AC wording")
        sys.exit(1)

    single_file = [args.corpus[0]]
    batch_files = args.corpus[:5]
    if len(batch_files) < 5:
        sys.exit(f"need >=5 corpus files for the N=5 sample, got {len(batch_files)}")

    any_failure = False
    results = {1: [], 5: []}

    out_dir = args.workdir
    for concurrency, files in ((1, single_file), (5, batch_files)):
        # Warmup: one discarded run, absorbs the per-process Metal
        # library compile cost (metal_v21.cpp:667) so it does not land
        # inside a timed sample.
        wall_ms, completions, rc, out, err = run_one(
            args.binary, out_dir, concurrency, files)
        lines.append(f"bench|concurrency={concurrency}|rep=warmup|wall_ms={wall_ms:.3f}|rc={rc}")
        if rc != 0:
            any_failure = True
            lines.append(f"bench|concurrency={concurrency}|rep=warmup|stderr={err.strip()!r}")

        for rep in range(1, args.repeats + 1):
            wall_ms, completions, rc, out, err = run_one(
                args.binary, out_dir, concurrency, files)
            comp_str = ",".join(f"{c:.3f}" for c in completions)
            lines.append(
                f"bench|concurrency={concurrency}|rep={rep}|wall_ms={wall_ms:.3f}|"
                f"completions_ms={comp_str}")
            if rc != 0:
                any_failure = True
                lines.append(f"bench|concurrency={concurrency}|rep={rep}|rc={rc}|stderr={err.strip()!r}")
            else:
                results[concurrency].append(wall_ms)

    wall1 = median_or_none(results[1])
    wall5 = median_or_none(results[5])
    if wall1 is None or wall5 is None or wall1 <= 0:
        verdict = "INCONCLUSIVE"
        ratio = float("nan")
    else:
        ratio = wall5 / wall1
        if ratio >= RATIO_STAIRCASE:
            verdict = "STAIRCASE"
        elif ratio < RATIO_OVERLAPPED:
            verdict = "OVERLAPPED"
        else:
            verdict = "INCONCLUSIVE"

    lines.append(f"bench|verdict={verdict}|ratio={ratio:.4f}" if ratio == ratio
                 else f"bench|verdict={verdict}|ratio=nan")
    lines.append(f"TEST_END={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}")

    with open(args.out, "w") as fh:
        fh.write("\n".join(lines) + "\n")

    print(f"VERDICT: {verdict} ratio={ratio}")
    sys.exit(0 if not any_failure else 1)


if __name__ == "__main__":
    main()
