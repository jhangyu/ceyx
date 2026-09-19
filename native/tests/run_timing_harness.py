#!/usr/bin/env python3
"""The ONE decode-stage timing harness (CPU phases + GPU stages, w1..wN).

WHY THIS FILE EXISTS
--------------------
Every measurement round of the CPU/GPU lever campaign grew its own throwaway
runner under native/scripts/tmp: a GPU-line parser here, an interleaved A/B
driver there, a median computation copy-pasted into each. They disagreed on
which iteration counts as cold, on whether overlapping GPU intervals are summed
or unioned, and on whether a run with zero instrument lines is a zero or a void.
Those are not stylistic differences -- they change the verdict. This module is
their single replacement; the tmp runners it supersedes are deleted.

WHAT IT MEASURES
----------------
Two instrument streams, both stderr, both off unless their env gate is set:

  [RawTiming]        one line per decode, per lane: the CPU phases
                     (raw_unpack_ms, auto_exposure_ms, total_ms, ...).
                     Gate CEYX_RAW_TIMING_LOG=1. Emitted from the SHARED decode
                     path (native/src/pipeline/raw_timing_log.cpp, called by
                     raw_gpu_pipeline.cpp's decode_file_*_into), which is why
                     these phases are now visible at every lane width. Before
                     that hoist they came from the FFI entry only, and
                     probe_concurrent_raw -- the multi-lane driver -- bypasses
                     it, so w2+ CPU attribution did not exist.

  [CEYX_GPU_TIMING]  one line per Metal command buffer: device-side GPU busy
                     time. Gate CEYX_GPU_TIMING=1. Within a lane, even
                     submission index = Stage 3 (raw_bayer_demosaic), odd =
                     Stage 4 (dng_render_stage4). That mapping was PROVEN by a
                     differential experiment (--max-long-edge collapses Stage 4
                     only), not assumed; see raw_gpu_timing_probe.cpp.

RULES THIS HARNESS FIXES (so a later run cannot quietly pick a different one)
----------------------------------------------------------------------------
1. Cold iteration: the FIRST decode of each (arm, width) run is excluded from
   every median, and is reported separately so the exclusion is auditable.
2. GPU occupancy is the UNION of [gpu_start_s, gpu_end_s] intervals, never
   their sum: at w8 several lanes' command buffers overlap in wall time and
   summing double-counts.
3. A record the instrument marked valid=0 is COUNTED and never averaged. Metal
   leaves both timestamps at 0 for a command buffer that never executed;
   subtracting yields ~1.9e9 ms, an outlier large enough to destroy any mean it
   enters silently.
4. Positive control: a run declared instrumented that produced ZERO lines of an
   enabled stream is VOID, not zero. A missing instrument and a fast decode are
   indistinguishable in the numbers and must not be in the summary.
5. Every run records the dylib's dwarfdump UUID and its own exit code, captured
   from the subprocess object (never from a shell pipeline, where the RC read
   back belongs to the last command in the pipe).

USAGE
-----
  # single-arm sweep
  python3 native/tests/run_timing_harness.py sweep \
      --widths 1,2,4,8 --decodes 24 --sample image_samples/raw_sample.arw \
      --artifact native/tests/tmp/my-run.log

  # interleaved A/B, two dylibs, alternating arms to cancel thermal drift
  python3 native/tests/run_timing_harness.py sweep --widths 1,2 --decodes 24 \
      --arm-a /path/to/before.dylib --arm-b /path/to/after.dylib \
      --rounds 3 --artifact native/tests/tmp/ab.log

  # re-summarise artifacts produced earlier (no decoding)
  python3 native/tests/run_timing_harness.py report native/tests/tmp/*.log

  # parser self-check against the canned log (no build, no GPU, no sample)
  python3 native/tests/run_timing_harness.py selfcheck

DYLIB SWAPPING (--arm-a/--arm-b) IS DESTRUCTIVE ON A SHARED TREE
----------------------------------------------------------------
probe_concurrent_raw links libdng_decoder_native.dylib by rpath out of
native/build, so an A/B arm is realised by copying a dylib over that path. The
harness backs the live file up first and restores it in a finally block, but if
someone else is building or measuring in the same tree at the same time they
will get the wrong binary. Coordinate the window before using two arms.
"""

import argparse
import glob
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BUILD_DIR = os.path.join(REPO, "native", "build")
LIVE_DYLIB = os.path.join(BUILD_DIR, "libdng_decoder_native.dylib")
PROBE_BIN = os.path.join(BUILD_DIR, "probe_concurrent_raw")
# .txt, NOT .log, deliberately: .gitignore line 5 ignores *.log repo-wide, so a
# canned fixture named .log is untrackable -- it would work on the machine that
# wrote it and be missing for everyone else, turning the self-check into an
# error on a fresh clone. Caught exactly that way during T13.
SELFCHECK_LOG = os.path.join(REPO, "native", "tests", "data",
                             "timing_harness_selfcheck.txt")

# Both parsers read key=value pairs by NAME. Column position is never used: the
# C++ emitters are free to append keys (and raw_timing_log.cpp did exactly that
# when it added lane= and schema=) without breaking anything here.
RAW_TIMING_PREFIX = "[RawTiming]"
GPU_TIMING_PREFIX = "[CEYX_GPU_TIMING]"
# Artifact-internal record the harness writes around each run, so `report` can
# re-derive per-arm/per-width grouping from the file alone.
RUN_HEADER_RE = re.compile(r"^#RUN arm=(\S+) width=(\d+) round=(\d+)$")

# CPU phases summarised, in table order. Adding a key here is all that is
# needed to surface a new phase the C++ side starts emitting.
CPU_PHASES = ("raw_unpack_ms", "auto_exposure_ms", "gpu_process_ms", "total_ms")


# --------------------------------------------------------------------------
# parsing
# --------------------------------------------------------------------------

def parse_key_values(text):
    """Split 'a=1 b=2' into {'a': '1', 'b': '2'}. Tokens without '=' ignored."""
    out = {}
    for token in text.split():
        if "=" in token:
            key, value = token.split("=", 1)
            out[key] = value
    return out


class RunRecords(object):
    """Everything one (arm, width, round) run produced.

    cpu_decodes is ordered as emitted, which is what makes the cold-iteration
    rule applicable: element 0 of each lane's sequence is that lane's first
    decode.
    """

    def __init__(self, arm, width, round_index):
        self.arm = arm
        self.width = width
        self.round_index = round_index
        self.cpu_decodes = []        # list of dict(str->str)
        self.gpu_records = []        # list of dict(str->str), valid==1 only
        self.gpu_invalid = 0
        self.return_code = None   # type: int | None
        self.wall_ms = None       # type: float | None

    def key(self):
        return (self.arm, self.width)


def ingest_line(line, record):
    """Feed one stderr/stdout line into a RunRecords. Returns True if used."""
    stripped = line.strip()
    if stripped.startswith(RAW_TIMING_PREFIX):
        record.cpu_decodes.append(
            parse_key_values(stripped[len(RAW_TIMING_PREFIX):]))
        return True
    if stripped.startswith(GPU_TIMING_PREFIX):
        fields = parse_key_values(stripped[len(GPU_TIMING_PREFIX):])
        # Rule 3: counted, never averaged.
        if fields.get("valid") == "1":
            record.gpu_records.append(fields)
        else:
            record.gpu_invalid += 1
        return True
    if stripped.startswith("PROBE "):
        fields = parse_key_values(stripped[len("PROBE "):])
        if "wall_ms" in fields:
            record.wall_ms = float(fields["wall_ms"])
        return True
    return False


def drop_cold_per_lane(decodes):
    """Rule 1: drop each lane's first decode. Returns (kept, dropped).

    Per LANE, not per run: at w8 the run's first eight decodes are eight
    different lanes each paying its own first-decode cost (page faults, arena
    allocation, queue creation), so dropping only the globally-first one would
    leave seven cold samples in the median.
    """
    seen = set()
    kept, dropped = [], []
    for fields in decodes:
        lane = fields.get("lane", "?")
        if lane in seen:
            kept.append(fields)
        else:
            seen.add(lane)
            dropped.append(fields)
    return kept, dropped


def median_of(decodes, key):
    values = []
    for fields in decodes:
        if key in fields:
            try:
                values.append(float(fields[key]))
            except ValueError:
                pass
    return statistics.median(values) if values else None


def stage_medians(gpu_records):
    """Rule: even submission index = Stage 3, odd = Stage 4, within a lane."""
    stage3, stage4 = [], []
    for fields in gpu_records:
        try:
            submission = int(fields["submission"])
            busy = float(fields["busy_ms"])
        except (KeyError, ValueError):
            continue
        (stage3 if submission % 2 == 0 else stage4).append(busy)
    return (statistics.median(stage3) if stage3 else None,
            statistics.median(stage4) if stage4 else None,
            len(stage3), len(stage4))


def interval_union_ms(gpu_records):
    """Rule 2: union, not sum, of GPU busy intervals."""
    intervals = []
    for fields in gpu_records:
        try:
            intervals.append((float(fields["gpu_start_s"]),
                              float(fields["gpu_end_s"])))
        except (KeyError, ValueError):
            continue
    if not intervals:
        return 0.0
    intervals.sort()
    total = 0.0
    low, high = intervals[0]
    for start, end in intervals[1:]:
        if start > high:
            total += high - low
            low, high = start, end
        else:
            high = max(high, end)
    total += high - low
    return total * 1000.0


def lanes_seen(decodes):
    return sorted({fields.get("lane", "?") for fields in decodes})


# --------------------------------------------------------------------------
# summarising
# --------------------------------------------------------------------------

def summarise(records, require_positive_control=True):
    """Collapse RunRecords into per-(arm,width) rows plus a VOID list.

    Returns (rows, voids). A row is a plain dict so `report` and `sweep` render
    identically -- there is one table renderer, not one per caller.
    """
    grouped = {}
    for record in records:
        grouped.setdefault(record.key(), []).append(record)

    rows, voids = [], []
    for (arm, width), group in sorted(grouped.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        cpu_all, gpu_all, invalid, return_codes = [], [], 0, []
        for record in group:
            cpu_all.extend(record.cpu_decodes)
            gpu_all.extend(record.gpu_records)
            invalid += record.gpu_invalid
            if record.return_code is not None:
                return_codes.append(record.return_code)

        # Rule 4: an enabled stream that produced nothing is VOID, not zero.
        if require_positive_control and not cpu_all:
            voids.append("arm=%s width=%d: zero [RawTiming] lines -- the CPU "
                         "instrument did not fire (wrong dylib, or gate unset). "
                         "VOID, not zero." % (arm, width))
            continue
        if require_positive_control and not gpu_all:
            voids.append("arm=%s width=%d: zero valid [CEYX_GPU_TIMING] records "
                         "-- the GPU instrument did not fire. VOID, not zero."
                         % (arm, width))
            continue

        kept, dropped = drop_cold_per_lane(cpu_all)
        s3, s4, n3, n4 = stage_medians(gpu_all)
        row = {
            "arm": arm,
            "width": width,
            "decodes_kept": len(kept),
            "decodes_cold_dropped": len(dropped),
            "lanes": len(lanes_seen(cpu_all)),
            "gpu_valid": len(gpu_all),
            "gpu_invalid": invalid,
            "stage3_median_ms": s3,
            "stage4_median_ms": s4,
            "stage3_n": n3,
            "stage4_n": n4,
            "gpu_union_ms": interval_union_ms(gpu_all),
            "return_codes": return_codes,
        }
        for phase in CPU_PHASES:
            row[phase] = median_of(kept, phase)
        rows.append(row)

    add_inflation(rows)
    return rows, voids


def add_inflation(rows):
    """Per arm, express every width's medians as a ratio to that arm's w1.

    Inflation is the quantity the campaign actually argues about ("is the CPU
    phase per decode getting worse as lanes are added?"), and computing it here
    rather than by hand in a report is the point of the harness. An arm with no
    w1 row gets None, never a silently-wrong baseline borrowed from another arm.
    """
    baselines = {row["arm"]: row for row in rows if row["width"] == 1}
    for row in rows:
        base = baselines.get(row["arm"])
        for phase in CPU_PHASES:
            key = phase + "_inflation_vs_w1"
            if base is None or not base.get(phase) or not row.get(phase):
                row[key] = None
            else:
                row[key] = row[phase] / base[phase]


def format_number(value, digits=3):
    return "n/a" if value is None else ("%.*f" % (digits, value))


def render_markdown(rows, voids):
    lines = []
    lines.append("| arm | width | lanes | n | cold dropped | raw_unpack_ms | "
                 "auto_exposure_ms | gpu_process_ms | total_ms | "
                 "unpack infl. | AE infl. | Stage3 ms | Stage4 ms | "
                 "GPU union ms | GPU invalid | RCs |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for row in rows:
        lines.append(
            "| %s | %d | %d | %d | %d | %s | %s | %s | %s | %s | %s | %s | %s | "
            "%s | %d | %s |" % (
                row["arm"], row["width"], row["lanes"], row["decodes_kept"],
                row["decodes_cold_dropped"],
                format_number(row["raw_unpack_ms"]),
                format_number(row["auto_exposure_ms"]),
                format_number(row["gpu_process_ms"]),
                format_number(row["total_ms"]),
                format_number(row["raw_unpack_ms_inflation_vs_w1"], 2),
                format_number(row["auto_exposure_ms_inflation_vs_w1"], 2),
                format_number(row["stage3_median_ms"]),
                format_number(row["stage4_median_ms"]),
                format_number(row["gpu_union_ms"], 1),
                row["gpu_invalid"],
                ",".join(str(code) for code in row["return_codes"]) or "n/a"))
    if voids:
        lines.append("")
        lines.append("VOID RUNS (positive control failed -- excluded from the "
                     "table above, NOT reported as zero):")
        for message in voids:
            lines.append("  - " + message)
    return "\n".join(lines)


# --------------------------------------------------------------------------
# running
# --------------------------------------------------------------------------

def dylib_uuid(path=LIVE_DYLIB):
    """Provenance that survives re-signing.

    A sha256 of a Mach-O changes on every codesign even when the code is
    identical, so it cannot answer "is this the binary I measured?". The
    dwarfdump UUID does.
    """
    if not os.path.exists(path):
        return "MISSING %s" % path
    proc = subprocess.run(["dwarfdump", "--uuid", path],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return proc.stdout.decode(errors="replace").strip() or "UUID-UNAVAILABLE"


def run_one(arm, width, round_index, sample, decodes, extra_args, handle):
    """One probe invocation. Writes the raw instrument lines into the artifact.

    The artifact keeps the RAW lines, not just the summary: a summary whose
    inputs were thrown away cannot be re-checked when its rule turns out wrong,
    and in this campaign the rule turned out wrong more than once.
    """
    record = RunRecords(arm, width, round_index)
    environment = dict(os.environ)
    environment["CEYX_RAW_TIMING_LOG"] = "1"
    environment["CEYX_GPU_TIMING"] = "1"
    command = [PROBE_BIN, str(width)] + list(extra_args) + [sample] * decodes

    handle.write("#RUN arm=%s width=%d round=%d\n" % (arm, width, round_index))
    handle.write("#PROVENANCE arm=%s time=%s %s\n"
                 % (arm, time.strftime("%Y-%m-%dT%H:%M:%S"), dylib_uuid()))
    handle.write("#COMMAND %s (sample x%d)\n"
                 % (" ".join([PROBE_BIN, str(width)] + list(extra_args) + [sample]),
                    decodes))
    handle.flush()

    # env=environment is NOT optional: without it the child inherits the
    # parent's environment, both instrument gates stay unset, and the run
    # produces a clean rc=0 with zero timing lines -- which is exactly what a
    # fast decode would look like if the positive control (rule 4) were not
    # there to call it VOID. This harness shipped with that bug for one run;
    # the control caught it. Do not "simplify" this argument away.
    proc = subprocess.run(command, cwd=REPO, env=environment,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # RC straight off the process object. Never from a shell pipeline: `cmd |
    # tail; RC=$?` reads tail's status, which is how this campaign has twice
    # recorded a green RC for a failing run.
    record.return_code = proc.returncode

    for stream in (proc.stdout, proc.stderr):
        for line in stream.decode(errors="replace").splitlines():
            if ingest_line(line, record):
                handle.write(line.strip() + "\n")
    handle.write("#RC arm=%s width=%d round=%d rc=%d cpu_lines=%d "
                 "gpu_valid=%d gpu_invalid=%d\n"
                 % (arm, width, round_index, record.return_code,
                    len(record.cpu_decodes), len(record.gpu_records),
                    record.gpu_invalid))
    handle.flush()
    return record


def install_arm(dylib_path):
    shutil.copy2(dylib_path, LIVE_DYLIB)


def command_sweep(args):
    if not os.path.exists(PROBE_BIN):
        print("probe_concurrent_raw not built: %s" % PROBE_BIN, file=sys.stderr)
        return 2
    widths = [int(w) for w in args.widths.split(",") if w.strip()]
    extra_args = args.probe_arg or []

    # Arms are alternated WITHIN each round rather than run back to back, so a
    # thermal or background-load drift over the session hits both arms equally
    # instead of landing entirely on whichever arm ran second.
    if args.arm_a or args.arm_b:
        if not (args.arm_a and args.arm_b):
            print("--arm-a and --arm-b must be given together", file=sys.stderr)
            return 2
        arms = [("A", args.arm_a), ("B", args.arm_b)]
    else:
        arms = [("live", None)]

    backup = None
    records = []
    os.makedirs(os.path.dirname(os.path.abspath(args.artifact)), exist_ok=True)
    try:
        if len(arms) > 1:
            backup = LIVE_DYLIB + ".timing-harness-backup"
            shutil.copy2(LIVE_DYLIB, backup)
        with open(args.artifact, "a") as handle:
            handle.write("\n#SESSION start=%s widths=%s decodes=%d sample=%s\n"
                         % (time.strftime("%Y-%m-%dT%H:%M:%S"), args.widths,
                            args.decodes, args.sample))
            for round_index in range(args.rounds):
                for arm_name, arm_path in arms:
                    if arm_path:
                        install_arm(arm_path)
                    for width in widths:
                        records.append(run_one(arm_name, width, round_index,
                                               args.sample, args.decodes,
                                               extra_args, handle))
    finally:
        if backup:
            shutil.copy2(backup, LIVE_DYLIB)
            os.remove(backup)

    rows, voids = summarise(records)
    table = render_markdown(rows, voids)
    print(table)
    with open(args.artifact, "a") as handle:
        handle.write("\n#SUMMARY\n" + table + "\n")
    # A session in which any run was VOID exits non-zero: a positive-control
    # failure must be impossible to miss by reading the tail of a log.
    return 1 if voids else 0


def records_from_artifacts(paths):
    """Reconstruct RunRecords from artifacts written by `sweep`."""
    records, current = [], None
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                header = RUN_HEADER_RE.match(line.strip())
                if header:
                    current = RunRecords(header.group(1), int(header.group(2)),
                                         int(header.group(3)))
                    records.append(current)
                    continue
                if line.startswith("#RC ") and current is not None:
                    fields = parse_key_values(line[len("#RC "):])
                    if "rc" in fields:
                        current.return_code = int(fields["rc"])
                    continue
                if current is not None:
                    ingest_line(line, current)
    return records


def command_report(args):
    paths = []
    for pattern in args.artifacts:
        expanded = sorted(glob.glob(pattern))
        paths.extend(expanded or [pattern])
    records = records_from_artifacts(paths)
    if not records:
        print("no #RUN blocks found in: %s" % ", ".join(paths), file=sys.stderr)
        return 2
    rows, voids = summarise(records)
    print(render_markdown(rows, voids))
    return 1 if voids else 0


# --------------------------------------------------------------------------
# self-check (ponytail: one runnable check that fails if parsing breaks)
# --------------------------------------------------------------------------

EXPECTED_SELFCHECK = {
    # Canned log: native/tests/data/timing_harness_selfcheck.log. Values below
    # are hand-computed from that file and are the assertion, so a change to
    # any parsing rule -- cold-drop, median, even/odd stage split, interval
    # UNION, invalid-record exclusion, positive control -- turns this red.
    ("live", 1): {
        "raw_unpack_ms": 10.0,       # lane 0: cold 90.0 dropped; median(10,11,9)
        "auto_exposure_ms": 5.0,
        "decodes_kept": 3,
        "decodes_cold_dropped": 1,
        "stage3_median_ms": 6.0,     # submissions 0,2,4 -> 5,6,7
        "stage4_median_ms": 3.0,     # submissions 1,3,5 -> 2,3,4
        "gpu_invalid": 1,
        # Interval UNION, hand-computed from the canned log's six valid
        # records: (100.000,100.005) overlaps (100.004,100.006) -> 6 ms;
        # (200.000,200.006) -> 6 ms; (200.500,200.503) -> 3 ms;
        # (300.000,300.007) contains (300.003,300.007) -> 7 ms.
        # Union 6+6+3+7 = 22 ms, against a SUM of busy_ms of 27 ms -- the two
        # differ precisely because of the overlaps, which is the property this
        # assertion is here to protect.
        "gpu_union_ms": 22.0,
    },
    ("live", 2): {
        # Two lanes, each with its own cold decode dropped (rule 1 is per lane).
        "raw_unpack_ms": 21.0,
        "decodes_kept": 2,
        "decodes_cold_dropped": 2,
        "lanes": 2,
    },
}


def command_selfcheck(_args):
    if not os.path.exists(SELFCHECK_LOG):
        print("canned log missing: %s" % SELFCHECK_LOG, file=sys.stderr)
        return 2
    records = records_from_artifacts([SELFCHECK_LOG])
    rows, voids = summarise(records)
    by_key = {(row["arm"], row["width"]): row for row in rows}

    failures = []
    for key, expectations in EXPECTED_SELFCHECK.items():
        row = by_key.get(key)
        if row is None:
            failures.append("missing row for arm=%s width=%d" % key)
            continue
        for field, expected in expectations.items():
            actual = row.get(field)
            if actual is None or abs(actual - expected) > 1e-6:
                failures.append("arm=%s width=%d %s: expected %s, got %s"
                                % (key[0], key[1], field, expected, actual))

    # Negative half: the canned log's third block has the CPU stream present
    # but the GPU stream absent, which rule 4 must turn into a VOID rather than
    # a row of zeros. Without this the positive control could be deleted and
    # every other assertion above would still pass.
    if not any("width=4" in message for message in voids):
        failures.append("positive control did not fire: arm=live width=4 has "
                        "no GPU records and should be VOID, got voids=%s" % voids)
    if ("live", 4) in by_key:
        failures.append("arm=live width=4 was summarised despite failing the "
                        "positive control")

    print(render_markdown(rows, voids))
    if failures:
        print("\nSELFCHECK FAIL (%d)" % len(failures))
        for message in failures:
            print("  - " + message)
        return 1
    print("\nSELFCHECK PASS (%d rows, %d voids, %d assertions)"
          % (len(rows), len(voids),
             sum(len(v) for v in EXPECTED_SELFCHECK.values()) + 2))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command")

    sweep = sub.add_parser("sweep", help="run probe_concurrent_raw sweeps")
    sweep.add_argument("--widths", default="1,2,4,8")
    sweep.add_argument("--decodes", type=int, default=24,
                       help="decodes per width (>=24 keeps the median stable)")
    sweep.add_argument("--sample", default="image_samples/raw_sample.arw")
    sweep.add_argument("--rounds", type=int, default=1,
                       help="repeat the whole arm set N times, interleaved")
    sweep.add_argument("--arm-a", help="dylib for arm A (enables A/B mode)")
    sweep.add_argument("--arm-b", help="dylib for arm B")
    sweep.add_argument("--probe-arg", action="append",
                       help="extra argument passed through to the probe")
    sweep.add_argument("--artifact", required=True)
    sweep.set_defaults(func=command_sweep)

    report = sub.add_parser("report", help="summarise existing artifacts")
    report.add_argument("artifacts", nargs="+")
    report.set_defaults(func=command_report)

    selfcheck = sub.add_parser("selfcheck", help="parser check, canned log")
    selfcheck.set_defaults(func=command_selfcheck)

    args = parser.parse_args(argv)
    if not getattr(args, "func", None):
        parser.print_help()
        return 2
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
