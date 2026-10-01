#!/usr/bin/env python3
"""Verifies the vendored LibRaw/RawSpeed tree matches PROVENANCE.md.

Exit 0 + "[Provenance] ALL PASS" when every pinned revision, patch hash and
license record agrees with the working tree; exit 1 + "[Provenance] FAIL ..."
otherwise. Read-only.
"""
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
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
    # native/scripts/deps/fetch_libraw.py strip_git(), invoked via
    # `build_deps.py fetch libraw`) so that PROVENANCE.md can be tracked
    # inside the otherwise-untracked vendor tree; the resolved revision is
    # recorded in a .vendor-rev sidecar file instead of read via git.
    vendor_rev = repo / ".vendor-rev"
    if vendor_rev.is_file():
        return vendor_rev.read_text(encoding="utf-8").strip()
    out = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                         capture_output=True, text=True)
    return out.stdout.strip() if out.returncode == 0 else ""


_HUNK_HEADER = re.compile(r"^@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@")


def parse_patch_hunks(patch_text):
    """Yields (target_relpath, pre_image, post_image, n_added, n_removed) per
    hunk in a unified diff. pre_image is the hunk's context + removed lines,
    post_image its context + added lines, both as stripped content lines in
    order (blank lines kept: they anchor position even though they are no
    marker). Hunk bodies are consumed by the @@ line counts, so a removed
    line that itself starts with "-- " is never mistaken for a file header."""
    path = None
    hunk = None  # [pre, post, n_added, n_removed, old_left, new_left]
    for line in patch_text.splitlines():
        if hunk is not None and (hunk[4] > 0 or hunk[5] > 0):
            tag, text = line[:1], line[1:].strip()
            if tag == "+":
                hunk[1].append(text)
                hunk[2] += 1
                hunk[5] -= 1
            elif tag == "-":
                hunk[0].append(text)
                hunk[3] += 1
                hunk[4] -= 1
            elif tag in (" ", ""):
                hunk[0].append(text)
                hunk[1].append(text)
                hunk[4] -= 1
                hunk[5] -= 1
            # "\\ No newline at end of file" and anything else: ignored
            continue
        if hunk is not None:
            yield path, hunk[0], hunk[1], hunk[2], hunk[3]
            hunk = None
        if line.startswith("+++ "):
            path = line[len("+++ "):].split("\t")[0]
            if path.startswith("b/"):
                path = path[2:]
        else:
            m = _HUNK_HEADER.match(line)
            if m:
                hunk = [[], [], 0, 0, int(m.group(1) or 1), int(m.group(2) or 1)]
    if hunk is not None:
        yield path, hunk[0], hunk[1], hunk[2], hunk[3]


def _contains_block(file_lines, block):
    """True iff `block` occurs as a contiguous run of lines in `file_lines`."""
    n = len(block)
    if n == 0:
        return True
    first = block[0]
    return any(file_lines[i] == first and file_lines[i:i + n] == block
               for i in range(len(file_lines) - n + 1))


def _scratch_git_reverse_check(patch_path, root):
    """Runs `git apply --check --reverse` in a throwaway git repo that holds
    copies (shutil.copy2 follows symlinks: content, not links) of the files
    the patch touches. Returns (status, detail); status is "applied",
    "rejected" (git examined the files and the reverse does not apply) or
    "unavailable" (git missing / a target file absent)."""
    rels = sorted({rel for rel, _, _, _, _ in parse_patch_hunks(
        patch_path.read_text(encoding="utf-8", errors="replace"))})
    missing = [rel for rel in rels if not (root / rel).is_file()]
    if not rels or missing:
        return "unavailable", "target file missing: " + (missing[0] if missing else "none")
    try:
        with tempfile.TemporaryDirectory(prefix="provenance-scratch-") as scratch:
            for rel in rels:
                dest = Path(scratch) / rel
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(root / rel, dest)
            subprocess.run(["git", "init", "-q"], cwd=scratch, check=True,
                           capture_output=True)
            done = subprocess.run(
                ["git", "apply", "--check", "--reverse", "--verbose",
                 str(patch_path.resolve())],
                cwd=scratch, capture_output=True, text=True)
    except (OSError, subprocess.SubprocessError) as exc:
        return "unavailable", "git unavailable: " + str(exc)[:80]
    output = (done.stdout + done.stderr).strip()
    if done.returncode == 0 and "Skipped patch" not in output:
        return "applied", ""
    lines = output.splitlines()
    first = next((ln for ln in lines if ln.startswith("error:")),
                 lines[0] if lines else "no output")[:140]
    return "rejected", first


def check_patch_applied(patch_path, reverse, root=RAWSPEED, later_patches=()):
    """Verifies the vendored tree reflects this patch's diff.

    Anchored per hunk, not per file: a forward-applied hunk's post-image
    (context + added lines) must occur as one contiguous block in the target
    file; a reverse-applied (see REVERSE_APPLIED_PATCHES) hunk's pre-image
    (context + removed lines) must. A hunk with nothing to add (pure
    deletion) has a post-image that is only context and so also matches the
    un-patched file; for it the opposite image (the pre-image, which holds
    the removed lines) must be ABSENT. Removed lines are never asserted
    absent file-wide: they may legitimately occur elsewhere in the file
    (a call site removed in one hunk while others are untouched, bare `}` /
    `#endif`), and only their position inside the hunk context means
    anything.

    Patches are stacked in order: a LATER patch in `later_patches` may have
    rewritten the very lines this patch added (e.g. 16/17 refactor what 14
    imported). A hunk whose image is gone but whose file a later patch also
    edits is superseded, not unapplied; that later patch is verified in its
    own right.

    `root` is the tree the diff's a/ b/ paths are relative to: RawSpeed3's
    own patches are rooted at RAWSPEED; project-authored LibRaw patches
    (patches/libraw/) are rooted at VENDOR (the LibRaw tree).
    """
    # Primary: `git apply -R --check` of a forward-applied patch, run in an
    # isolated scratch repo holding real copies (symlinks dereferenced) of
    # just the files the patch touches. Run in place it is void: git refuses
    # paths beyond a symlink, and silently skips gitignored paths while still
    # exiting 0. A primary rejection is not final -- patches are stacked and a
    # later patch may have rewritten this one's lines -- so the hunk check
    # below judges next; every verdict logs which path produced it.
    name = patch_path.name
    if not reverse:
        status, detail = _scratch_git_reverse_check(patch_path, root)
        if status == "applied":
            print("[Provenance] note: " + name + ": judged by primary-scratch-git")
            return True, ""
        print("[Provenance] note: " + name + ": primary-scratch-git " + status +
              " (" + detail + "); fallback hunk check judges")
    patch_text = patch_path.read_text(encoding="utf-8", errors="replace")
    cache = {}
    later_files = set()
    for later in later_patches:
        later_text = later.read_text(encoding="utf-8", errors="replace")
        later_files.update(rel for rel, _, _, _, _ in parse_patch_hunks(later_text))
    for relpath, pre, post, n_added, n_removed in parse_patch_hunks(patch_text):
        target = root / relpath
        if not target.is_file():
            return False, "fallback-hunk-check: target file missing: " + relpath
        if relpath not in cache:
            cache[relpath] = [ln.strip() for ln in target.read_text(
                encoding="utf-8", errors="replace").splitlines()]
        file_lines = cache[relpath]
        # (image that must be present, image that must be absent or None).
        # A pure-deletion (forward) / pure-addition (reverse) hunk has a
        # target image that is only context, which the un-patched file also
        # matches, so there the *other* image is the discriminator.
        if reverse:
            must_have, label_src = (pre, pre) if n_removed else (None, post)
            must_lack = None if n_removed else post
        else:
            must_have, label_src = (post, post) if n_added else (None, pre)
            must_lack = None if n_added else pre
        changed = set(pre) ^ set(post)
        label = next((ln for ln in label_src if ln in changed and ln), "")[:80]
        if must_have is not None and not _contains_block(file_lines, must_have):
            if relpath in later_files:
                continue  # superseded by a later patch's edit of this file
            return False, "fallback-hunk-check: " + relpath + " missing expected hunk: " + label
        if must_lack is not None and _contains_block(file_lines, must_lack):
            return False, "fallback-hunk-check: " + relpath + " still contains un-patched hunk: " + label
    print("[Provenance] note: " + name + ": judged by fallback-hunk-check")
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
    for index, patch in enumerate(patches):
        digest = hashlib.sha256(patch.read_bytes()).hexdigest()
        if digest not in text:
            fail("patch " + patch.name + " sha256 " + digest + " not recorded")
            continue
        print("[Provenance] patch " + patch.name + " sha256 -> PASS")
        reverse = patch.name in REVERSE_APPLIED_PATCHES
        ok, detail = check_patch_applied(patch, reverse,
                                         later_patches=patches[index + 1:])
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
    for index, patch in enumerate(project_patches):
        digest = hashlib.sha256(patch.read_bytes()).hexdigest()
        if digest not in text:
            fail("project patch " + patch.name + " sha256 " + digest + " not recorded")
            continue
        print("[Provenance] project patch " + patch.name + " sha256 -> PASS")
        ok, detail = check_patch_applied(patch, reverse=False, root=VENDOR,
                                         later_patches=project_patches[index + 1:])
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
