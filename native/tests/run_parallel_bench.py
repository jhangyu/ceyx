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
import re
import statistics
import subprocess
import sys
import time

RATIO_STAIRCASE = 4.0
RATIO_OVERLAPPED = 2.5
REPEATS_DEFAULT = 5
WALL_MS_RE = re.compile(r"wall_ms=([0-9.]+)")


def run_one(binary, out_dir, threads, files, driver="test_concurrent_decode"):
    """Run one decode-bench process. Returns (wall_ms, completions_ms, rc, stdout, stderr).

    driver="test_concurrent_decode": DNG-only, usage
        `<binary> <out_dir> <threads> <file>...`; wall_ms is measured
        externally (the binary emits no timing) and completions_ms is
        derived from decode_<i>.raw dump mtimes in out_dir.
    driver="probe_concurrent_raw": production ARW/RAW route
        (raw_decode_and_process, probe_concurrent_raw.cpp:48), usage
        `<binary> <threads> <file>...` (no out_dir argument); the binary
        prints its OWN `PROBE threads=N files=M wall_ms=W` line, which is
        used instead of external timing (matches native/tests/
        probe_concurrent_raw.py's established parsing precedent). This
        driver has no per-decode dump mechanism, so completions_ms is
        always [] — no staircase SHAPE evidence from this driver, only the
        aggregate ratio. Documented limitation, not engineered around.
    """
    if driver == "test_concurrent_decode":
        for name in os.listdir(out_dir):
            if name.startswith("decode_") and (name.endswith(".raw") or name.endswith(".dims")):
                os.remove(os.path.join(out_dir, name))
        cmd = [binary, out_dir, str(threads), *files]
    elif driver == "probe_concurrent_raw":
        cmd = [binary, str(threads), *files]
    else:
        raise ValueError(f"unknown driver {driver!r}")

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
    wall_ms_external = (t1_perf - t0_perf) * 1000.0

    if driver == "probe_concurrent_raw":
        m = WALL_MS_RE.search(proc.stdout)
        if proc.returncode == 0 and not m:
            # Binary succeeded but its self-reported timing line is
            # missing — a real instrument failure, must not silently fall
            # back to the (less precise, includes process-spawn overhead)
            # external timer.
            return wall_ms_external, [], 1, proc.stdout, \
                proc.stderr + "\n[run_parallel_bench] no wall_ms= in stdout"
        wall_ms = float(m.group(1)) if m else wall_ms_external
        return wall_ms, [], proc.returncode, proc.stdout, proc.stderr

    completions = []
    for i in range(len(files)):
        p = os.path.join(out_dir, f"decode_{i}.raw")
        if os.path.exists(p):
            mtime = os.stat(p).st_mtime
            completions.append((mtime - t0_wall) * 1000.0)
    completions.sort()
    return wall_ms_external, completions, proc.returncode, proc.stdout, proc.stderr


def median_or_none(xs):
    return statistics.median(xs) if xs else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--corpus", nargs="+", required=True,
                     help="corpus files; the full list (first 5) is the N=5 "
                          "batch. N=1 baseline depends on --baseline-mode.")
    ap.add_argument("--baseline-mode", choices=("matched", "single", "per-file"),
                     default="per-file",
                     help="per-file (default, AC1-correct methodology, lead-"
                          "opus correction 2026-09-05): decode each of the 5 "
                          "batch files INDIVIDUALLY (threads=1, one process "
                          "per file) and pool all (file x repeat) wall_ms "
                          "samples into ONE list; the median of that pooled "
                          "list is the single-decode denominator. This is "
                          "what AC1 ('5-way batch wall time < 2.5x SINGLE-"
                          "decode time') actually means. matched (INVALID for "
                          "verdict purposes, see expectations.md experiments "
                          "2A/2B): N=1 sample is threads=1 over the SAME "
                          "5-file batch in ONE process (aggregate serial "
                          "work, not a single-decode time) — the frozen "
                          "thresholds cannot be applied to this denominator, "
                          "kept only for the raw wall-time record. single "
                          "(experiment-1 methodology, kept for reference "
                          "only): N=1 sample is one file (corpus[0]) alone — "
                          "confounded when corpus files differ in size.")
    ap.add_argument("--driver", choices=("test_concurrent_decode", "probe_concurrent_raw"),
                     default="test_concurrent_decode",
                     help="test_concurrent_decode (default): DNG-only, "
                          "`<binary> <out_dir> <threads> <file>...`, external "
                          "timing, per-decode dump-based completions_ms. "
                          "probe_concurrent_raw: production ARW/RAW route "
                          "(raw_decode_and_process), `<binary> <threads> "
                          "<file>...`, self-reported wall_ms, NO "
                          "completions_ms (binary has no per-decode dump "
                          "mechanism — documented limitation, see "
                          "expectations.md experiment 4).")
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
            "/nonexistent/test_concurrent_decode", args.workdir, 1, [args.corpus[0]], driver=args.driver)
        lines.append(f"selftest|binary=missing|rc={rc}|stderr={err.strip()!r}")
        with open(args.out, "w") as fh:
            fh.write("\n".join(lines) + "\n")
        if rc == 0:
            sys.exit("FAIL: nonexistent binary unexpectedly returned rc=0")
        print(f"selftest OK: nonexistent binary rc={rc} (non-zero, as expected); "
              f"script itself now exits non-zero too, per AC wording")
        sys.exit(1)

    batch_files = args.corpus[:5]
    if len(batch_files) < 5:
        sys.exit(f"need >=5 corpus files for the N=5 sample, got {len(batch_files)}")
    lines.append(f"bench|baseline_mode={args.baseline_mode}")

    any_failure = False
    results = {1: [], 5: []}
    out_dir = args.workdir

    # N=5 concurrent batch arm — unchanged across all baseline modes.
    wall_ms, completions, rc, out, err = run_one(args.binary, out_dir, 5, batch_files, driver=args.driver)
    lines.append(f"bench|concurrency=5|rep=warmup|wall_ms={wall_ms:.3f}|rc={rc}")
    if rc != 0:
        any_failure = True
        lines.append(f"bench|concurrency=5|rep=warmup|stderr={err.strip()!r}")
    for rep in range(1, args.repeats + 1):
        wall_ms, completions, rc, out, err = run_one(args.binary, out_dir, 5, batch_files, driver=args.driver)
        comp_str = ",".join(f"{c:.3f}" for c in completions)
        lines.append(f"bench|concurrency=5|rep={rep}|wall_ms={wall_ms:.3f}|completions_ms={comp_str}")
        if rc != 0:
            any_failure = True
            lines.append(f"bench|concurrency=5|rep={rep}|rc={rc}|stderr={err.strip()!r}")
        else:
            results[5].append(wall_ms)

    # N=1 baseline arm — shape depends on --baseline-mode.
    if args.baseline_mode == "per-file":
        # One shared warmup (first file), discarded, then REPEATS timed
        # single-file decodes for EACH of the 5 files, pooled into one list.
        wall_ms, completions, rc, out, err = run_one(args.binary, out_dir, 1, [batch_files[0]], driver=args.driver)
        lines.append(f"bench|concurrency=1|rep=warmup|wall_ms={wall_ms:.3f}|rc={rc}")
        if rc != 0:
            any_failure = True
            lines.append(f"bench|concurrency=1|rep=warmup|stderr={err.strip()!r}")
        for f in batch_files:
            for rep in range(1, args.repeats + 1):
                wall_ms, completions, rc, out, err = run_one(args.binary, out_dir, 1, [f], driver=args.driver)
                lines.append(
                    f"bench|concurrency=1|file={f}|rep={rep}|wall_ms={wall_ms:.3f}|"
                    f"completions_ms={','.join(f'{c:.3f}' for c in completions)}")
                if rc != 0:
                    any_failure = True
                    lines.append(f"bench|concurrency=1|file={f}|rep={rep}|rc={rc}|stderr={err.strip()!r}")
                else:
                    results[1].append(wall_ms)
        lines.append(f"bench|concurrency=1|pooled_samples={len(results[1])}|"
                     f"denominator=median_of_pooled_per_file_samples")
    else:
        baseline_files = batch_files if args.baseline_mode == "matched" else [args.corpus[0]]
        lines.append(f"bench|baseline_files={','.join(baseline_files)}")
        wall_ms, completions, rc, out, err = run_one(args.binary, out_dir, 1, baseline_files, driver=args.driver)
        lines.append(f"bench|concurrency=1|rep=warmup|wall_ms={wall_ms:.3f}|rc={rc}")
        if rc != 0:
            any_failure = True
            lines.append(f"bench|concurrency=1|rep=warmup|stderr={err.strip()!r}")
        for rep in range(1, args.repeats + 1):
            wall_ms, completions, rc, out, err = run_one(args.binary, out_dir, 1, baseline_files, driver=args.driver)
            comp_str = ",".join(f"{c:.3f}" for c in completions)
            lines.append(f"bench|concurrency=1|rep={rep}|wall_ms={wall_ms:.3f}|completions_ms={comp_str}")
            if rc != 0:
                any_failure = True
                lines.append(f"bench|concurrency=1|rep={rep}|rc={rc}|stderr={err.strip()!r}")
            else:
                results[1].append(wall_ms)

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
    if args.baseline_mode == "matched":
        lines.append("bench|WARNING=matched baseline denominator is aggregate "
                     "serial work for N files, not a single-decode time; the "
                     "frozen STAIRCASE/OVERLAPPED thresholds (calibrated for a "
                     "single-decode denominator) do NOT apply to this ratio — "
                     "see expectations.md experiments 2A/2B annotation.")
    lines.append(f"TEST_END={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}")

    with open(args.out, "w") as fh:
        fh.write("\n".join(lines) + "\n")

    print(f"VERDICT: {verdict} ratio={ratio}")
    sys.exit(0 if not any_failure else 1)


if __name__ == "__main__":
    main()
