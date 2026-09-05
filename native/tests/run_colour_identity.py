#!/usr/bin/env python3
"""R1-T4 — colour-identity harness (AC5 machinery).

USER DECREE 2026-09-05: parallelization must not affect decoded colour.
This script is the judge: it drives test_concurrent_decode at threads=1
(serial) and threads=5 (concurrent5) over a fixed corpus, hashes every
decode_<i>.raw dump with sha256, and compares against a recorded baseline
manifest. The criterion is bit-identity (sha256), never PSNR.

Usage:
  run_colour_identity.py --binary <test_concurrent_decode> \
      --corpus <file...> --baseline <manifest.json> [--record] \
      --out <artifact> [--corrupt-dump-index N]

--record is meant to be used exactly once, on the pre-change tree. It
(re-)writes <baseline> with the serial-run sha256 of every corpus file
and also stashes a copy of every serial dump alongside the manifest (in an
untracked tmp/ location) so a future mismatch can be diagnosed with
stage4_channel_compare.py. Re-recording later defeats the point of AC5 and
is a process violation even though the script itself does not refuse it
(the plan's discipline, not a code lock, is what forbids it).

--corrupt-dump-index N is a self-test-only knob: after the serial run
completes and dumps are hashed, it flips one byte of decode_<N>.raw before
comparing against the baseline, to demonstrate the instrument can fail.
Never used in a real gate run.

Output per input:
  colour|file=<name>|mode=<serial|concurrent5>|sha256=<hex>|match=<YES|NO|RECORDED>
Final line:
  colour|verdict=<IDENTICAL|DIFFERENT>

Exit 0 iff every match is YES or RECORDED (i.e. no NO). --record with an
empty/absent baseline always exits 0 by construction (nothing to mismatch
against on the recording run itself, since baseline == what was just
measured).
"""
import argparse
import hashlib
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_dims(path):
    with open(path) as fh:
        w, h = fh.read().split()
    return int(w), int(h)


def run_decode(binary, out_dir, threads, files, repeat=1):
    cmd = [binary, out_dir, str(threads)]
    if repeat > 1:
        cmd += ["--repeat", str(repeat)]
    cmd += list(files)
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True)
    except OSError as exc:
        sys.exit(f"FATAL: could not execute binary {binary!r}: {exc}")
    return proc


def hash_dumps(out_dir, files):
    """Returns {index: (sha256, width, height)} for decode_<i>.raw, i in
    range(len(files)) — dumps are index-comparable across serial/concurrent
    runs by construction of test_concurrent_decode (arg position, not
    completion order)."""
    result = {}
    for i in range(len(files)):
        raw_path = os.path.join(out_dir, f"decode_{i}.raw")
        dims_path = os.path.join(out_dir, f"decode_{i}.dims")
        if not os.path.exists(raw_path):
            sys.exit(f"FATAL: expected dump missing: {raw_path}")
        w, h = read_dims(dims_path)
        result[i] = (sha256_file(raw_path), w, h, raw_path)
    return result


def corrupt_one_byte(path):
    with open(path, "r+b") as fh:
        fh.seek(0)
        b = fh.read(1)
        fh.seek(0)
        fh.write(bytes([b[0] ^ 0xFF]))


def non_degenerate_check(path, width, height):
    """Cheap sanity check that a dump is not a zero-filled or otherwise
    degenerate buffer that would hash stably while meaning nothing (e.g. a
    new decode route that "succeeds" but writes garbage/zeros). Not a
    colour judgement -- just: does this look like real pixel data at the
    size it claims to be?"""
    size = os.path.getsize(path)
    expected = width * height * 3
    if size != expected:
        return False, f"size={size} expected={expected}"
    with open(path, "rb") as fh:
        sample = fh.read(min(size, 1 << 20))
    distinct = len(set(sample))
    if distinct < 8:
        return False, f"distinct_byte_values={distinct} (looks zero-filled/degenerate)"
    return True, f"distinct_byte_values={distinct} sample_bytes={len(sample)}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--corpus", nargs="+", required=True)
    ap.add_argument("--baseline", required=True,
                     help="manifest.json path (read, and written if --record)")
    ap.add_argument("--record", action="store_true",
                     help="record the serial-run hashes as the new baseline "
                          "(pre-change tree only; use exactly once)")
    ap.add_argument("--append", action="store_true",
                     help="append new corpus files to an EXISTING baseline "
                          "without touching already-recorded rows (used for "
                          "the ARW addendum after R1-T3's dual-route lands, "
                          "still on the pre-R1-T2 tree). Mutually exclusive "
                          "with --record.")
    ap.add_argument("--out", required=True)
    ap.add_argument("--append-out", action="store_true",
                     help="R2-T3 (d): open --out in append mode instead of "
                          "write/truncate mode. Distinct from --append above "
                          "(which controls baseline MANIFEST rows) -- this "
                          "flag only changes how the --out ARTIFACT file is "
                          "opened. Without it, 'w' mode silently wipes any "
                          "pre-existing content the caller wrote first (e.g. "
                          "gate-haiku's prepended TEST_START/HEAD/PORCELAIN "
                          "attestation header) -- exactly what happened in "
                          "both R1 gate runs. Does not change any "
                          "measurement semantics or line formats.")
    ap.add_argument("--corrupt-dump-index", type=int, default=None,
                     help="self-test only: flip one byte of decode_<N>.raw "
                          "after the serial run, before comparing")
    ap.add_argument("--baseline-dumps-dir",
                     default=None,
                     help="where recorded baseline dump copies live "
                          "(untracked); required with --record, and used to "
                          "feed stage4_channel_compare.py on mismatch")
    ap.add_argument("--stage4-compare",
                     default=os.path.join(os.path.dirname(__file__),
                                           "stage4_channel_compare.py"))
    args = ap.parse_args()

    files = [os.path.abspath(f) for f in args.corpus]
    for f in files:
        if not os.path.exists(f):
            sys.exit(f"FATAL: corpus file does not exist: {f}")

    if args.record and args.append:
        sys.exit("FATAL: --record and --append are mutually exclusive")

    lines = []
    lines.append(f"BINARY={args.binary}")
    lines.append(f"CORPUS={' '.join(files)}")
    lines.append(f"RECORD={args.record}")
    lines.append(f"APPEND={args.append}")
    lines.append("")

    overall_ok = True
    baseline = {}
    existing_entries = []
    index_offset = 0
    if args.append:
        if not os.path.exists(args.baseline):
            sys.exit(f"FATAL: --append requires an existing baseline at "
                      f"{args.baseline}")
        with open(args.baseline) as fh:
            manifest = json.load(fh)
        existing_entries = manifest.get("corpus", [])
        index_offset = (max((e["index"] for e in existing_entries), default=-1)
                         + 1)
        lines.append(f"[append] existing baseline has {len(existing_entries)} "
                      f"frozen row(s), new rows start at index {index_offset}")
    elif os.path.exists(args.baseline) and not args.record:
        with open(args.baseline) as fh:
            manifest = json.load(fh)
        for entry in manifest.get("corpus", []):
            baseline[entry["index"]] = entry["sha256_output"]
    elif not args.record:
        sys.exit(f"FATAL: baseline {args.baseline} does not exist and "
                  f"neither --record nor --append was given")

    tmp_root = tempfile.mkdtemp(prefix="colour_identity_")
    serial_dir = os.path.join(tmp_root, "serial")
    concurrent_dir = os.path.join(tmp_root, "concurrent5")
    os.makedirs(serial_dir)
    os.makedirs(concurrent_dir)

    # --- serial (threads=1) ---
    proc = run_decode(args.binary, serial_dir, 1, files)
    lines.append("[serial run]")
    lines.append(f"cmd={proc.args}")
    lines.append(proc.stdout.strip())
    lines.append(proc.stderr.strip())
    rc = proc.returncode
    lines.append(f"RC={rc}")
    if rc != 0:
        sys.exit(_flush(args.out, lines, ok=False, append=args.append_out))

    serial_hashes = hash_dumps(serial_dir, files)

    if args.corrupt_dump_index is not None:
        idx = args.corrupt_dump_index
        target = serial_hashes[idx][3]
        lines.append(f"[self-test] corrupting one byte of {target}")
        corrupt_one_byte(target)
        new_sha = sha256_file(target)
        serial_hashes[idx] = (new_sha, serial_hashes[idx][1],
                               serial_hashes[idx][2], target)

    # --- concurrent5 (threads=5), pad with --repeat so >= 5 slots exist ---
    repeat = max(1, math.ceil(5 / len(files)))
    proc = run_decode(args.binary, concurrent_dir, 5, files, repeat=repeat)
    lines.append("")
    lines.append("[concurrent5 run]")
    lines.append(f"cmd={proc.args} (repeat={repeat}, total_slots={len(files) * repeat})")
    lines.append(proc.stdout.strip())
    lines.append(proc.stderr.strip())
    rc = proc.returncode
    lines.append(f"RC={rc}")
    if rc != 0:
        sys.exit(_flush(args.out, lines, ok=False, append=args.append_out))

    concurrent_hashes = hash_dumps(concurrent_dir, files)

    lines.append("")
    lines.append("[comparison]")

    recording = args.record or args.append
    manifest_entries = []
    for i, f in enumerate(files):
        idx = index_offset + i if args.append else i
        name = os.path.basename(f)
        s_sha, s_w, s_h, s_path = serial_hashes[i]
        c_sha, c_w, c_h, c_path = concurrent_hashes[i]

        if recording:
            match_s = "RECORDED"
            baseline[idx] = s_sha
            manifest_entries.append({
                "index": idx,
                "file": name,
                "sha256_input": sha256_file(f),
                "width": s_w,
                "height": s_h,
                "sha256_output": s_sha,
            })
            ok_nd, detail_nd = non_degenerate_check(s_path, s_w, s_h)
            lines.append(f"[non-degenerate-check {name}] {'OK' if ok_nd else 'FAIL'} {detail_nd}")
            if not ok_nd:
                overall_ok = False
        else:
            match_s = "YES" if s_sha == baseline.get(idx) else "NO"
            if match_s == "NO":
                overall_ok = False

        match_c = "YES" if c_sha == baseline.get(idx, s_sha if recording else None) else "NO"
        if recording:
            # On the recording/append run, concurrent5 must match what was
            # just recorded from serial (the "stable before trusted"
            # self-check from R1-T4 AC3), not RECORDED itself.
            match_c = "YES" if c_sha == s_sha else "NO"
            ok_nd_c, detail_nd_c = non_degenerate_check(c_path, c_w, c_h)
            lines.append(f"[non-degenerate-check {name} concurrent5] {'OK' if ok_nd_c else 'FAIL'} {detail_nd_c}")
            if not ok_nd_c:
                overall_ok = False
        if match_c == "NO":
            overall_ok = False

        lines.append(f"colour|file={name}|mode=serial|sha256={s_sha}|match={match_s}")
        lines.append(f"colour|file={name}|mode=concurrent5|sha256={c_sha}|match={match_c}")

        if match_s == "NO" or match_c == "NO":
            if args.baseline_dumps_dir:
                baseline_dump = os.path.join(args.baseline_dumps_dir,
                                              f"decode_{idx}.raw")
                if os.path.exists(baseline_dump):
                    offending = c_path if match_c == "NO" else s_path
                    cmp_proc = subprocess.run(
                        [sys.executable, args.stage4_compare, baseline_dump,
                         offending, "--width", str(s_w), "--height", str(s_h)],
                        capture_output=True, text=True)
                    lines.append(f"[stage4-compare {name}] {cmp_proc.stdout.strip()}")
                    lines.append(f"[stage4-compare {name}] rc={cmp_proc.returncode}")

    if recording:
        if not args.baseline_dumps_dir:
            sys.exit(_flush(args.out, lines + [
                "FATAL: --record/--append requires --baseline-dumps-dir"],
                ok=False, append=args.append_out))
        os.makedirs(args.baseline_dumps_dir, exist_ok=True)
        for i, f in enumerate(files):
            idx = index_offset + i if args.append else i
            shutil.copyfile(serial_hashes[i][3],
                             os.path.join(args.baseline_dumps_dir,
                                          f"decode_{idx}.raw"))
        all_entries = existing_entries + manifest_entries if args.append \
            else manifest_entries
        all_entries = sorted(all_entries, key=lambda e: e["index"])
        with open(args.baseline, "w") as fh:
            json.dump({
                "schema_version": 1,
                "corpus": all_entries,
            }, fh, indent=2)
            fh.write("\n")
        mode = "append" if args.append else "record"
        lines.append(f"[{mode}] wrote {args.baseline} ({len(all_entries)} total "
                      f"row(s), {len(manifest_entries)} new) and baseline "
                      f"dumps to {args.baseline_dumps_dir}")

    verdict = "IDENTICAL" if overall_ok else "DIFFERENT"
    lines.append("")
    lines.append(f"colour|verdict={verdict}")

    shutil.rmtree(tmp_root, ignore_errors=True)
    return _flush(args.out, lines, ok=overall_ok, append=args.append_out)


def _flush(out_path, lines, ok, append=False):
    with open(out_path, "a" if append else "w") as fh:
        fh.write("\n".join(lines) + "\n")
    for ln in lines:
        print(ln)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
