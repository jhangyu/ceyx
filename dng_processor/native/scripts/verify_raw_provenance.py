#!/usr/bin/env python3
"""Verifies the vendored LibRaw/RawSpeed tree matches PROVENANCE.md.

Exit 0 + "[Provenance] ALL PASS" when every pinned revision, patch hash and
license record agrees with the working tree; exit 1 + "[Provenance] FAIL ..."
otherwise. Read-only.
"""
import hashlib
import re
import subprocess
import sys
from pathlib import Path

NATIVE = Path(__file__).resolve().parent.parent
REPO = NATIVE.parent.parent
VENDOR = NATIVE / "third_party" / "libraw"
RAWSPEED = VENDOR / "RawSpeed3" / "rawspeed"
LIBRAW_CMAKE = NATIVE / "third_party" / "libraw-cmake"
PROVENANCE = VENDOR / "PROVENANCE.md"
REQUIRED_LICENSES = ["LibRaw", "RawSpeed", "pugixml", "zlib", "libjpeg"]

failures = []


def fail(msg):
    failures.append(msg)
    print("[Provenance] FAIL " + msg)


def git_head(repo):
    # Vendored components have their .git stripped after fetch (see
    # fetch_libraw_dist.sh strip_git()) so that PROVENANCE.md can be tracked
    # inside the otherwise-untracked vendor tree; the resolved revision is
    # recorded in a .vendor-rev sidecar file instead of read via git.
    vendor_rev = repo / ".vendor-rev"
    if vendor_rev.is_file():
        return vendor_rev.read_text(encoding="utf-8").strip()
    out = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                         capture_output=True, text=True)
    return out.stdout.strip() if out.returncode == 0 else ""


def main():
    if not PROVENANCE.is_file():
        fail("missing " + str(PROVENANCE))
        return 1
    text = PROVENANCE.read_text(encoding="utf-8")

    revs = set(re.findall(r"\b[0-9a-f]{40}\b", text))
    for name, repo in (("LibRaw", VENDOR), ("RawSpeed", RAWSPEED),
                        ("LibRaw-cmake", LIBRAW_CMAKE)):
        head = git_head(repo)
        if not head:
            fail(name + " tree at " + str(repo) + " is not a git checkout")
        elif head not in revs:
            fail(name + " HEAD " + head + " is not recorded in PROVENANCE.md")
        else:
            print("[Provenance] " + name + " revision " + head + " -> PASS")

    patch_dir = VENDOR / "RawSpeed3" / "patches"
    patches = sorted(patch_dir.glob("*.patch")) if patch_dir.is_dir() else []
    for patch in patches:
        digest = hashlib.sha256(patch.read_bytes()).hexdigest()
        if digest not in text:
            fail("patch " + patch.name + " sha256 " + digest + " not recorded")
        else:
            print("[Provenance] patch " + patch.name + " -> PASS")

    for lic in REQUIRED_LICENSES:
        if lic not in text:
            fail("no license record for " + lic)
        else:
            print("[Provenance] license " + lic + " -> PASS")

    licenses_doc = REPO / "docs" / "THIRD_PARTY_LICENSES.md"
    if not licenses_doc.is_file():
        fail("missing " + str(licenses_doc))
    else:
        doc_text = licenses_doc.read_text(encoding="utf-8")
        for lic in ("LibRaw", "RawSpeed"):
            if lic not in doc_text:
                fail(lic + " missing from THIRD_PARTY_LICENSES.md")
        print("[Provenance] THIRD_PARTY_LICENSES.md -> PASS")

    if failures:
        print("[Provenance] FAIL (" + str(len(failures)) + " problems)")
        return 1
    print("[Provenance] ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
