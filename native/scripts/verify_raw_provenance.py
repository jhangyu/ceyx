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
REPO = NATIVE.parent
VENDOR = NATIVE / "third_party" / "libraw"
RAWSPEED = VENDOR / "RawSpeed3" / "rawspeed"
LIBRAW_CMAKE = NATIVE / "third_party" / "libraw-cmake"
PROVENANCE = VENDOR / "PROVENANCE.md"
PROJECT_PATCH_DIR = NATIVE / "patches" / "libraw"
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


def parse_patch_files(patch_text):
    """Yields (target_relpath, added_lines, removed_lines) per file in a
    unified diff. added/removed are the literal content lines (leading
    '+'/'-' stripped), excluding the '+++'/'---' header lines themselves and
    blank lines (blank-line changes are too common to be a useful marker)."""
    current_path = None
    added, removed = [], []
    for line in patch_text.splitlines():
        if line.startswith("+++ b/") or line.startswith("+++ "):
            if current_path is not None:
                yield current_path, added, removed
            current_path = line[len("+++ "):].split("\t")[0]
            if current_path.startswith("b/"):
                current_path = current_path[2:]
            added, removed = [], []
        elif line.startswith("+++") or line.startswith("---"):
            continue
        elif line.startswith("+"):
            content = line[1:].strip()
            if content:
                added.append(content)
        elif line.startswith("-"):
            content = line[1:].strip()
            if content:
                removed.append(content)
    if current_path is not None:
        yield current_path, added, removed


def check_patch_applied(patch_path, reverse, root=RAWSPEED):
    """Verifies the vendored tree reflects this patch's diff.

    For a forward-applied patch, the added lines must be present in the
    current source file (and, as a weaker signal, the removed lines absent).
    For a patch stored/applied in reverse (see REVERSE_APPLIED_PATCHES),
    the roles invert: the patch's "added" lines must be ABSENT (never
    applied forward) and its "removed" lines must be PRESENT (the tree is
    in the pre-patch state the reverse-apply restores).

    `root` is the tree the diff's a/ b/ paths are relative to: RawSpeed3's
    own patches are rooted at RAWSPEED; project-authored LibRaw patches
    (patches/libraw/) are rooted at VENDOR (the LibRaw tree).
    """
    patch_text = patch_path.read_text(encoding="utf-8", errors="replace")
    for relpath, added, removed in parse_patch_files(patch_text):
        target = root / relpath
        if not target.is_file():
            return False, "target file missing: " + relpath
        # Exact-line, not substring, membership: a removed line can be a
        # literal substring of a still-present (differently reformatted)
        # added line (e.g. "if (x) {" inside "} else if (x) {"), which would
        # falsely read as "still un-patched" under substring matching.
        file_lines = {ln.strip() for ln in
                      target.read_text(encoding="utf-8", errors="replace").splitlines()}
        added_set, removed_set = set(added), set(removed)
        # Lines that appear as both an added and a removed line (identical
        # after stripping, e.g. only their indentation changed, or the same
        # line recurs unmodified elsewhere in the hunk) are not a reliable
        # forward/reverse discriminator either way; drop them from both
        # sides before checking.
        common = added_set & removed_set
        added_set -= common
        removed_set -= common
        want_present = removed_set if reverse else added_set
        want_absent = added_set if reverse else removed_set
        for marker in want_present:
            if marker not in file_lines:
                return False, relpath + " missing expected line: " + marker[:80]
        for marker in want_absent:
            if marker in file_lines:
                return False, relpath + " still contains un-patched line: " + marker[:80]
    return True, ""


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
    # R2 fix (F5, round-1 review): hashing the .patch *file* only proves the
    # patch text on disk is unchanged; it says nothing about whether the
    # vendored RawSpeed3 tree the patch targets actually has it applied (or,
    # for the one patch stored reversed relative to its own diff direction,
    # un-applied). A tree with zero patches applied, or one applied in the
    # wrong direction, previously still printed ALL PASS. Instrument note
    # (round-1 review): `git apply --check` inside this stripped-`.git`
    # vendor tree returns rc=0 in BOTH directions for every patch here — do
    # not use it. This check instead parses each patch's own diff hunks and
    # greps the literal added/removed lines against the current vendored
    # source file, which is direction-discriminating and patch-content
    # driven (not a hardcoded marker list that could silently drift from the
    # patch files).
    # Which of LibRaw's patches are stored in the direction OPPOSITE to how the
    # vendored tree needs them. This is a property of the RawSpeed3 PIN, not a
    # constant: it was {"01.CameraMeta-extensibility.patch"} at de70ef5f and is
    # re-determined at every re-pin (Phase 19 Task 2, Step 5). At c835b05a all
    # four surviving patches were re-generated as forward diffs against the new
    # pin, so the set is empty.
    REVERSE_APPLIED_PATCHES = set()
    for patch in patches:
        digest = hashlib.sha256(patch.read_bytes()).hexdigest()
        if digest not in text:
            fail("patch " + patch.name + " sha256 " + digest + " not recorded")
            continue
        print("[Provenance] patch " + patch.name + " sha256 -> PASS")
        reverse = patch.name in REVERSE_APPLIED_PATCHES
        ok, detail = check_patch_applied(patch, reverse)
        if ok:
            state = "reverse-applied (pre-state)" if reverse else "forward-applied"
            print("[Provenance] patch " + patch.name + " tree state (" + state + ") -> PASS")
        else:
            fail("patch " + patch.name + " does not appear applied in the vendored tree: " + detail)

    # Project-authored LibRaw patches (patches/libraw/). Same two checks as the
    # RawSpeed3 set: the patch text must be the one recorded, AND the vendored
    # tree must actually reflect it. All of these are forward-applied; there is
    # no reverse case, because we author them against the pinned tree.
    project_patches = (sorted(PROJECT_PATCH_DIR.glob("*.patch"))
                       if PROJECT_PATCH_DIR.is_dir() else [])
    for patch in project_patches:
        digest = hashlib.sha256(patch.read_bytes()).hexdigest()
        if digest not in text:
            fail("project patch " + patch.name + " sha256 " + digest + " not recorded")
            continue
        print("[Provenance] project patch " + patch.name + " sha256 -> PASS")
        ok, detail = check_patch_applied(patch, reverse=False, root=VENDOR)
        if ok:
            print("[Provenance] project patch " + patch.name +
                  " tree state (forward-applied) -> PASS")
        else:
            fail("project patch " + patch.name +
                 " does not appear applied in the vendored tree: " + detail)

    for lic in REQUIRED_LICENSES:
        if lic not in text:
            fail("no license record for " + lic)
        else:
            print("[Provenance] license " + lic + " -> PASS")

    licenses_doc = REPO / "docs" / "legal" / "THIRD_PARTY_LICENSES.md"
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
