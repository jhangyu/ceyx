#!/usr/bin/env python3
"""R1-T3 queue-ownership key audit driver.

Runs decodes against a libdng_decoder_native.dylib that has
tests/probe_queue_affinity.cpp linked in (strong overrides of the weak Halide
hooks halide_metal_acquire_context / halide_metal_release_context), collects
the probe's per-entry log, and emits ONE verdict line:

    affinity|key=<USER_CONTEXT|THREAD|NEITHER>|evidence=<path>

VERDICT RULES — pre-registered, printed into the artifact BEFORE any run.
Mirrors native/tests/probe_concurrent_raw.py:6-13. They may not be edited once
a number exists; a re-run with different binaries, corpus or concurrency is a
different experiment and must be labelled as such.

  USER_CONTEXT  iff  (a) every observed entry carries a non-null user_context,
                     (b) in the 5-way concurrent run the number of distinct
                         non-null user_context values is exactly 5 (one per
                         in-flight decode, none shared), and
                     (c) in a serial run exactly 1 distinct value is seen.
  THREAD        iff  USER_CONTEXT does not hold, AND
                     (a) the serial run shows exactly 1 distinct thread id,
                     (b) the 5-way run shows exactly 5 distinct thread ids
                         (so no entry arrives on a thread that does not own a
                         decode, and no decode is split across threads).
  NEITHER       otherwise  =>  STOP. A per-decode queue cannot be handed out
                     safely; report the finding, never guess a key.

INSTRUMENT VALIDATION (AC2): the artifact lists, per run, every distinct
entry-point kind the probe observed. An absence claim about a key is only
admissible if all of device_malloc, copy_to_device, run, device_sync,
copy_to_host are demonstrably present in that list.

Usage:
  probe_queue_affinity.py --raw-probe <probe_concurrent_raw>
                          --ffi-harness <dng_ffi_harness>
                          --dng <file.dng> --arw <file.ARW> [--arw ...]
                          --logdir <dir> --out <artifact>
Exit 0 iff every decode subprocess exited 0. The verdict never affects the
exit code — the judge is the reader, not the script.
"""

import argparse
import collections
import datetime
import os
import re
import subprocess
import sys

ENTRY_RE = re.compile(
    r"^entry\|ev=(?P<ev>\w+)\|fn=(?P<fn>[^|]*)\|create=(?P<create>\d+)"
    r"\|user_context=(?P<uc>[^|]*)\|thread=(?P<th>[^|]*)\|seq=(?P<seq>\d+)$"
)

REQUIRED_KINDS = [
    "device_malloc",
    "copy_to_device",
    "run",
    "device_sync",
    "copy_to_host",
]


def parse_log(path):
    entries = []
    if not os.path.exists(path):
        return entries
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = ENTRY_RE.match(line.strip())
            if m:
                entries.append(m.groupdict())
    return entries


def kind_of(fn):
    """Normalise a backtrace symbol to a bare entry-point kind."""
    if not fn.startswith("_halide_metal_") and not fn.startswith("halide_metal_"):
        return fn
    return fn.split("halide_metal_", 1)[1]


def summarise(entries):
    acquires = [e for e in entries if e["ev"] == "acquire"]
    ucs = collections.Counter(e["uc"] for e in acquires)
    ths = collections.Counter(e["th"] for e in acquires)
    kinds = collections.Counter(kind_of(e["fn"]) for e in acquires)
    nonnull_uc = {u for u in ucs if u not in ("0x0", "(nil)", "0")}
    return {
        "acquires": len(acquires),
        "releases": len(entries) - len(acquires),
        "uc_counts": ucs,
        "th_counts": ths,
        "kinds": kinds,
        "distinct_uc": len(ucs),
        "distinct_uc_nonnull": len(nonnull_uc),
        "null_uc_entries": sum(c for u, c in ucs.items() if u in ("0x0", "(nil)", "0")),
        "distinct_threads": len(ths),
    }


def run_case(name, argv, logpath, env_extra, out):
    if os.path.exists(logpath):
        os.remove(logpath)
    env = dict(os.environ)
    env["DNG_QUEUE_PROBE_LOG"] = logpath
    env.update(env_extra)
    out.write("\n## run: %s\n" % name)
    out.write("cmd: %s\n" % " ".join(argv))
    proc = subprocess.run(argv, env=env, capture_output=True, text=True)
    out.write("RC=%d\n" % proc.returncode)
    if proc.stdout.strip():
        out.write("stdout: %s\n" % proc.stdout.strip().replace("\n", " | "))
    if proc.stderr.strip():
        out.write("stderr(tail): %s\n" % proc.stderr.strip()[-400:].replace("\n", " | "))
    entries = parse_log(logpath)
    s = summarise(entries)
    out.write("log: %s (%d entries: %d acquire / %d release)\n"
              % (logpath, len(entries), s["acquires"], s["releases"]))
    out.write("entry-point kinds observed: %s\n"
              % ", ".join("%s=%d" % kv for kv in sorted(s["kinds"].items())))
    missing = [k for k in REQUIRED_KINDS
               if not any(k == kk or kk.endswith(k) for kk in s["kinds"])]
    out.write("required-kind coverage: %s\n"
              % ("ALL PRESENT" if not missing else "MISSING " + ",".join(missing)))
    out.write("distinct user_context: %d (non-null %d, null-valued entries %d)\n"
              % (s["distinct_uc"], s["distinct_uc_nonnull"], s["null_uc_entries"]))
    out.write("distinct thread ids: %d\n" % s["distinct_threads"])
    out.write("user_context histogram: %s\n"
              % ", ".join("%s x%d" % kv for kv in s["uc_counts"].most_common()))
    out.write("thread histogram: %s\n"
              % ", ".join("%s x%d" % kv for kv in s["th_counts"].most_common()))
    out.write("--- first 40 raw log lines ---\n")
    with open(logpath, "r", errors="replace") as fh:
        for i, line in enumerate(fh):
            if i >= 40:
                out.write("... (truncated; full log at %s)\n" % logpath)
                break
            out.write(line if line.endswith("\n") else line + "\n")
    out.flush()
    return proc.returncode, s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw-probe", required=True)
    ap.add_argument("--ffi-harness", required=True)
    ap.add_argument("--dng", required=True)
    ap.add_argument("--arw", action="append", default=[])
    ap.add_argument("--logdir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--concurrency", type=int, default=5)
    args = ap.parse_args()

    os.makedirs(args.logdir, exist_ok=True)
    rc_all = 0
    with open(args.out, "w") as out:
        out.write("# R1-T3 queue-ownership key audit\n")
        out.write("# generated: %sZ\n"
                  % datetime.datetime.now(datetime.timezone.utc).isoformat())
        out.write(__doc__ or "")
        out.write("\n" + "=" * 72 + "\n")
        out.write("PRE-REGISTRATION ENDS HERE. Everything below is measurement.\n")
        out.write("=" * 72 + "\n")

        cases = []
        cases.append((
            "serial-DNG (dng_ffi_harness, 1 decode)",
            [args.ffi_harness, args.dng, "1"],
            os.path.join(args.logdir, "affinity-dng-serial.log"),
        ))
        if args.arw:
            cases.append((
                "serial-ARW (probe_concurrent_raw threads=1, 1 file)",
                [args.raw_probe, "1", args.arw[0]],
                os.path.join(args.logdir, "affinity-arw-serial.log"),
            ))
            files = [args.arw[i % len(args.arw)] for i in range(args.concurrency)]
            cases.append((
                "concurrent-ARW (probe_concurrent_raw threads=%d, %d files)"
                % (args.concurrency, len(files)),
                [args.raw_probe, str(args.concurrency)] + files,
                os.path.join(args.logdir, "affinity-arw-concurrent5.log"),
            ))

        results = {}
        for name, argv, logpath in cases:
            rc, s = run_case(name, argv, logpath, {}, out)
            rc_all = rc_all or rc
            results[name] = s

        serial = [s for n, s in results.items() if n.startswith("serial")]
        conc = [s for n, s in results.items() if n.startswith("concurrent")]

        reasons = []
        key = "NEITHER"
        if serial and conc:
            c = conc[0]
            uc_ok = (all(s["null_uc_entries"] == 0 for s in serial + [c])
                     and all(s["distinct_uc_nonnull"] == 1 for s in serial)
                     and c["distinct_uc_nonnull"] == args.concurrency)
            th_ok = (all(s["distinct_threads"] == 1 for s in serial)
                     and c["distinct_threads"] == args.concurrency)
            reasons.append("uc rule: null-entries=%s serial-distinct=%s conc-distinct=%d (need %d)"
                           % ([s["null_uc_entries"] for s in serial + [c]],
                              [s["distinct_uc_nonnull"] for s in serial],
                              c["distinct_uc_nonnull"], args.concurrency))
            reasons.append("thread rule: serial-distinct=%s conc-distinct=%d (need %d)"
                           % ([s["distinct_threads"] for s in serial],
                              c["distinct_threads"], args.concurrency))
            if uc_ok:
                key = "USER_CONTEXT"
            elif th_ok:
                key = "THREAD"
        else:
            reasons.append("missing serial and/or concurrent run — cannot decide")

        out.write("\n" + "=" * 72 + "\nVERDICT REASONING\n")
        for r in reasons:
            out.write("  " + r + "\n")
        out.write("affinity|key=%s|evidence=%s\n" % (key, os.path.abspath(args.out)))
    print("affinity|key=%s|evidence=%s" % (key, os.path.abspath(args.out)))
    return 0 if rc_all == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
