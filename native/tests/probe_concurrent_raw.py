#!/usr/bin/env python3
"""Task 1 go/no-go probe driver.

Runs probe_concurrent_raw at concurrency 1, 2 and 4 (3 repeats each, median
taken) over the ARW corpus and emits the pre-registered verdict.

Pre-registered 2026-09-03, before any number existed:
    speedup(4) >= 2.0  -> MUTEX-BOUND   (proceed with the plan)
    speedup(4) <  1.3  -> HALIDE-BOUND  (stop; re-scope with the user)
    otherwise          -> INCONCLUSIVE  (stop; user decision)
Do not edit these thresholds after a number exists.
"""
import argparse
import re
import resource
import statistics
import subprocess
import sys

WALL_RE = re.compile(r"wall_ms=([0-9.]+)")
CONCURRENCIES = (1, 2, 4)
REPEATS = 3


def run_once(binary, threads, files):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    proc = subprocess.run([binary, str(threads), *files],
                          capture_output=True, text=True)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if proc.returncode != 0:
        sys.exit(f"probe failed rc={proc.returncode}: {proc.stderr}")
    m = WALL_RE.search(proc.stdout)
    if not m:
        sys.exit(f"no wall_ms in probe output: {proc.stdout!r}")
    cpu = ((after.ru_utime - before.ru_utime) +
           (after.ru_stime - before.ru_stime))
    return float(m.group(1)) / 1000.0, cpu


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="native/build/probe_concurrent_raw")
    ap.add_argument("--head", required=True,
                    help="ceyx git HEAD the binary was built from")
    ap.add_argument("--out", required=True)
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    rows = []
    for n in CONCURRENCIES:
        samples = [run_once(args.binary, n, args.files) for _ in range(REPEATS)]
        wall = statistics.median(s[0] for s in samples)
        cpu = statistics.median(s[1] for s in samples)
        rows.append((n, wall, cpu))

    base = rows[0][1]
    lines = [
        "# Task 1 — Halide serialisation go/no-go probe",
        "",
        f"Built from ceyx HEAD `{args.head}`.",
        f"Corpus: {len(args.files)} files. {REPEATS} repeats per level, median.",
        "Path under test: RAW/ARW (raw_gpu_pipeline.cpp) — already lock-free.",
        "",
        "concurrency | wall_s | cpu_s | speedup",
        "---|---|---|---",
    ]
    for n, wall, cpu in rows:
        lines.append(f"{n} | {wall:.3f} | {cpu:.3f} | {base / wall:.3f}")

    s4 = base / rows[-1][1]
    if s4 >= 2.0:
        verdict = "MUTEX-BOUND"
    elif s4 < 1.3:
        verdict = "HALIDE-BOUND"
    else:
        verdict = "INCONCLUSIVE"
    lines += ["", f"VERDICT: {verdict}", ""]
    with open(args.out, "w") as fh:
        fh.write("\n".join(lines))
    print(f"VERDICT: {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
