#!/usr/bin/env python3
"""Validates the RAW corpus manifest and the files it points at.

  --schema-only        validate manifest structure only (no files needed)
  --generate-malformed derive the malformed fixtures from the first Bayer sample

Exit 0 + "[Corpus] ALL PASS <n> samples" when every present sample hashes
correctly and every required category is represented.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
MANIFEST = Path(__file__).resolve().parent / "raw_corpus_manifest.json"

REQUIRED_KEYS = ["id", "path", "sha256", "vendor", "camera", "extension",
                 "expect_route", "expect_layout", "expect_backend",
                 "expect_error", "license", "notes"]
VALID_LAYOUTS = {"bayer2x2", "xtrans6x6", "monochrome", "linear_rgb",
                 "linear_ycbcr", "other_cfa", "layered", "multi_frame",
                 "unsupported"}
VALID_BACKENDS = {"rawspeed3", "libraw_native", "dng_sdk", "unknown"}
VALID_ERRORS = {"kRawSuccess", "kRawErrProbeFailed", "kRawErrParseFailed",
                "kRawErrUnpackFailed", "kRawErrLayoutUnsupported",
                "kRawErrMetadataInvalid"}


def load_corpus(manifest_path=MANIFEST):
    """Importable by run_raw_matrix.py."""
    with open(manifest_path, encoding="utf-8") as handle:
        return json.load(handle)["samples"]


def check_schema(samples, failures):
    seen_ids = set()
    for entry in samples:
        for key in REQUIRED_KEYS:
            if key not in entry:
                failures.append("sample %r missing key %r" % (entry.get("id"), key))
        sid = entry.get("id", "")
        if sid in seen_ids:
            failures.append("duplicate id %r" % sid)
        seen_ids.add(sid)
        if entry.get("expect_layout") not in VALID_LAYOUTS:
            failures.append("%s: bad expect_layout %r" % (sid, entry.get("expect_layout")))
        if entry.get("expect_backend") not in VALID_BACKENDS:
            failures.append("%s: bad expect_backend %r" % (sid, entry.get("expect_backend")))
        if entry.get("expect_error") not in VALID_ERRORS:
            failures.append("%s: bad expect_error %r" % (sid, entry.get("expect_error")))
        path = entry.get("path", "")
        if path.startswith("/") or ".." in path:
            failures.append("%s: path %r escapes the repository" % (sid, path))


def check_categories(samples, failures):
    bayer_vendors = {s["vendor"] for s in samples if s["expect_layout"] == "bayer2x2"}
    if len(bayer_vendors) < 3:
        failures.append("need >=3 Bayer vendors, manifest has %d" % len(bayer_vendors))
    xtrans = [s for s in samples if s["expect_layout"] == "xtrans6x6"]
    if len(xtrans) < 2:
        failures.append("need >=2 X-Trans samples, manifest has %d" % len(xtrans))
    if len({s["camera"] for s in xtrans}) < 2:
        failures.append("the X-Trans samples must be different camera generations")
    if not [s for s in samples if s["expect_backend"] == "libraw_native"
            and s["expect_error"] == "kRawSuccess"]:
        failures.append("need >=1 natural LibRaw-native fallback sample")
    if not [s for s in samples if s["expect_error"] == "kRawErrLayoutUnsupported"]:
        failures.append("need >=1 unsupported-layout sample")
    malformed = [s for s in samples if s["id"].startswith("malformed_")]
    if len(malformed) < 3:
        failures.append("need >=3 malformed fixtures, manifest has %d" % len(malformed))
    if not [s for s in samples if s["expect_backend"] == "rawspeed3"]:
        failures.append("need >=1 sample with expect_backend == 'rawspeed3'")


def generate_malformed(samples):
    base = next((s for s in samples
                 if s["expect_layout"] == "bayer2x2"
                 and s.get("expect_route") == "generic"
                 and (REPO / s["path"]).is_file()), None)
    if base is None:
        print("[Corpus] FAIL cannot generate: no present Bayer sample")
        return 1
    src = REPO / base["path"]
    data = bytearray(src.read_bytes())

    variants = {
        ".trunc.raw": bytes(data[:int(len(data) * 0.6)]),
    }
    # Patch a 64-byte window in the IFD region to an implausible pitch, and a
    # separate window to zero, without touching the magic bytes (so the probe
    # still routes generic and the failure lands in unpack/metadata).
    pitch = bytearray(data)
    pitch[64:128] = b"\xff" * 64
    variants[".pitch.raw"] = bytes(pitch)
    cfa = bytearray(data)
    cfa[128:192] = b"\x00" * 64
    variants[".cfa.raw"] = bytes(cfa)

    for suffix, payload in variants.items():
        out = src.with_name(src.name + suffix)
        out.write_bytes(payload)
        print("[Corpus] generated %s sha256=%s"
              % (out.name, hashlib.sha256(payload).hexdigest()))
    print("[Corpus] copy the printed digests into raw_corpus_manifest.json")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", default=str(MANIFEST))
    parser.add_argument("--schema-only", action="store_true")
    parser.add_argument("--generate-malformed", action="store_true")
    args = parser.parse_args()

    samples = load_corpus(Path(args.manifest))
    failures = []

    check_schema(samples, failures)
    if failures:
        for f in failures:
            print("[Corpus] FAIL " + f)
        return 1
    if args.schema_only:
        print("[Corpus] SCHEMA PASS")
        return 0
    if args.generate_malformed:
        return generate_malformed(samples)

    present = 0
    for entry in samples:
        path = REPO / entry["path"]
        if not path.is_file():
            print("[Corpus] SKIP %s (missing file)" % entry["id"])
            continue
        present += 1
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if not entry["sha256"]:
            failures.append("%s: file present but sha256 not recorded (actual %s)"
                            % (entry["id"], digest))
        elif digest != entry["sha256"]:
            failures.append("%s: sha256 mismatch (recorded %s, actual %s)"
                            % (entry["id"], entry["sha256"], digest))
        else:
            print("[Corpus] %s sha256 -> PASS" % entry["id"])

    check_categories(samples, failures)

    if failures:
        for f in failures:
            print("[Corpus] FAIL " + f)
        return 1
    print("[Corpus] ALL PASS %d samples" % present)
    return 0


if __name__ == "__main__":
    sys.exit(main())
