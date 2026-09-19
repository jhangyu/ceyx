#!/usr/bin/env python3
"""Nikon High Efficiency (HE / HE*) decode gate.

Unlike test_libraw_frontend, an absent required sample is a FAILURE here, not
a silent skip: a zero-coverage run must never look like a full run. Every skip
is printed as an explicit SKIP-DECLARED line.

Exit 0 only after printing "[NikonHeGate] ALL PASS".
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
REQUIRED_SAMPLE_IDS = ["nikon_z8_high_efficiency", "nikon_z8_high_efficiency_star"]
DECODER_NAME_MARKER = "decoder=nikon_he_load_raw()"
EXPECTED_BACKEND_MARKER = "backend=libraw_native"
SMOKE_LINE_PATTERN = re.compile(r"^\[LibRawSmoke\] .*$", re.MULTILINE)
DIMENSION_PATTERN = re.compile(r"\braw=(\d+)x(\d+)\b")
FILTER_MASK_PATTERN = re.compile(r"\bfilters=(\d+)\b")
HASH_LINE_PATTERN = re.compile(r"^HASH .*\bw=(\d+) h=(\d+) fnv1a=(0x[0-9a-f]+) rc=(-?\d+)",
                               re.MULTILINE)
EXPOSURE_LINE_PATTERN = re.compile(r"^EV .*\brc=(-?\d+) .*\bev=([-+0-9.eE]+)", re.MULTILINE)
MAXIMUM_PLAUSIBLE_EXPOSURE_VALUE = 4.0
MAXIMUM_EXPOSURE_PAIR_DIFFERENCE = 0.5

failures = []


def fail(message):
    failures.append(message)
    print("[NikonHeGate] FAIL " + message)


def run_binary(binary_path, sample_path):
    completed = subprocess.run([str(binary_path), str(sample_path)],
                               cwd=str(REPOSITORY_ROOT), capture_output=True, text=True)
    return completed.returncode, completed.stdout


def check_libraw_smoke(binary_path, sample):
    return_code, output = run_binary(binary_path, sample["absolute_path"])
    if return_code != 0:
        fail("%s: libraw_smoke returned %d" % (sample["id"], return_code))
        return
    smoke_lines = SMOKE_LINE_PATTERN.findall(output)
    if not smoke_lines:
        fail("%s: no anchored [LibRawSmoke] line in output" % sample["id"])
        return
    line = smoke_lines[0]
    if DECODER_NAME_MARKER not in line:
        fail("%s: decoder is not nikon_he_load_raw(): %s" % (sample["id"], line))
    if EXPECTED_BACKEND_MARKER not in line:
        fail("%s: backend is not libraw_native: %s" % (sample["id"], line))
    dimensions = DIMENSION_PATTERN.search(line)
    if not dimensions:
        fail("%s: no raw=<W>x<H> field in %s" % (sample["id"], line))
    elif (int(dimensions.group(1)), int(dimensions.group(2))) != (
            sample["expect_width"], sample["expect_height"]):
        fail("%s: dimensions %sx%s != manifest %dx%d"
             % (sample["id"], dimensions.group(1), dimensions.group(2),
                sample["expect_width"], sample["expect_height"]))
    filter_mask = FILTER_MASK_PATTERN.search(line)
    if not filter_mask or int(filter_mask.group(1)) == 0:
        fail("%s: filters mask is zero or absent (not a Bayer layout)" % sample["id"])


def check_pipeline_hash(binary_path, sample):
    hashes = []
    for attempt in (1, 2):
        return_code, output = run_binary(binary_path, sample["absolute_path"])
        if return_code != 0:
            fail("%s: raw_corpus_hash_baseline returned %d on attempt %d"
                 % (sample["id"], return_code, attempt))
            return
        match = HASH_LINE_PATTERN.search(output)
        if not match:
            fail("%s: no anchored HASH line on attempt %d" % (sample["id"], attempt))
            return
        width, height, digest, decode_return_code = match.groups()
        if decode_return_code != "0":
            fail("%s: pipeline decode rc=%s" % (sample["id"], decode_return_code))
            return
        if (int(width), int(height)) != (sample["expect_width"], sample["expect_height"]):
            fail("%s: pipeline dimensions %sx%s != manifest %dx%d"
                 % (sample["id"], width, height,
                    sample["expect_width"], sample["expect_height"]))
        hashes.append(digest)
    if hashes[0] != hashes[1]:
        fail("%s: non-deterministic decode, fnv1a %s != %s"
             % (sample["id"], hashes[0], hashes[1]))


def check_exposure(binary_path, sample):
    return_code, output = run_binary(binary_path, sample["absolute_path"])
    if return_code != 0:
        fail("%s: raw_corpus_ev_gate returned %d" % (sample["id"], return_code))
        return None
    match = EXPOSURE_LINE_PATTERN.search(output)
    if not match:
        fail("%s: no anchored EV line" % sample["id"])
        return None
    decode_return_code, exposure_text = match.groups()
    if decode_return_code != "0":
        fail("%s: exposure gate decode rc=%s" % (sample["id"], decode_return_code))
        return None
    exposure_value = float(exposure_text)
    if abs(exposure_value) > MAXIMUM_PLAUSIBLE_EXPOSURE_VALUE:
        fail("%s: auto exposure %.4f EV outside +/-%.1f -- decoded plane is "
             "not a plausible photograph"
             % (sample["id"], exposure_value, MAXIMUM_PLAUSIBLE_EXPOSURE_VALUE))
    return exposure_value


def load_samples(manifest_path, allow_missing_samples):
    with open(manifest_path, encoding="utf-8") as handle:
        entries = {entry["id"]: entry for entry in json.load(handle)["samples"]}
    present = []
    for sample_id in REQUIRED_SAMPLE_IDS:
        entry = entries.get(sample_id)
        if entry is None:
            fail("manifest has no entry with id %s" % sample_id)
            continue
        absolute_path = REPOSITORY_ROOT / entry["path"]
        if not absolute_path.is_file():
            if allow_missing_samples:
                print("[NikonHeGate] SKIP-DECLARED %s (file absent)" % sample_id)
            else:
                fail("required sample absent: %s" % entry["path"])
            continue
        for key in ("expect_width", "expect_height"):
            if not isinstance(entry.get(key), int) or entry[key] <= 0:
                fail("%s: manifest %s must be a positive integer" % (sample_id, key))
        present.append({"id": sample_id, "absolute_path": absolute_path,
                        "expect_width": entry.get("expect_width", 0),
                        "expect_height": entry.get("expect_height", 0)})
    return present


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", default="native/tests/raw_corpus_manifest.json")
    parser.add_argument("--build-dir", default="native/build")
    parser.add_argument("--allow-missing-samples", action="store_true")
    arguments = parser.parse_args()

    build_directory = REPOSITORY_ROOT / arguments.build_dir
    smoke_binary = build_directory / "libraw_smoke"
    hash_binary = build_directory / "raw_corpus_hash_baseline"
    exposure_binary = build_directory / "raw_corpus_ev_gate"
    for binary in (smoke_binary, hash_binary, exposure_binary):
        if not binary.is_file():
            fail("required binary missing (build it, do not skip): %s" % binary)

    samples = load_samples(REPOSITORY_ROOT / arguments.manifest,
                           arguments.allow_missing_samples)
    if not samples:
        fail("no HE/HE* sample was exercised at all")

    exposures = {}
    if not failures:
        for sample in samples:
            check_libraw_smoke(smoke_binary, sample)
            check_pipeline_hash(hash_binary, sample)
            exposures[sample["id"]] = check_exposure(exposure_binary, sample)

    paired = all(exposures.get(sample_id) is not None for sample_id in REQUIRED_SAMPLE_IDS)
    pair_note = ""
    if paired:
        difference = abs(exposures[REQUIRED_SAMPLE_IDS[0]] - exposures[REQUIRED_SAMPLE_IDS[1]])
        if difference > MAXIMUM_EXPOSURE_PAIR_DIFFERENCE:
            fail("HE and HE* exposures differ by %.3f EV (limit %.1f); either they "
                 "are not the same scene or one decode is wrong"
                 % (difference, MAXIMUM_EXPOSURE_PAIR_DIFFERENCE))
    else:
        print("[NikonHeGate] SKIP-DECLARED exposure-pair "
              "(no same-scene HE/HE* pair)")
        pair_note = " (no exposure pair)"

    if failures:
        print("[NikonHeGate] %d FAILURES" % len(failures))
        return 1
    print("[NikonHeGate] ALL PASS %d samples%s" % (len(samples), pair_note))
    return 0


if __name__ == "__main__":
    sys.exit(main())
