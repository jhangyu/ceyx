#!/usr/bin/env python3
"""Mechanical checker: bash-only syntax in workflow `run:` steps that execute
under POSIX `sh`, not `bash`.

Root cause of the round-6 CI failure (task #17/#18, run 34706875811): the
Linux leg of `.github/workflows/linux_build.yml` runs inside `container:
ubuntu:22.04`, and every step banner in that job resolves to `shell: sh -e
{0}` (dash), never `bash`. The step "Fail if no shared library was produced"
used `shopt -s nullglob`, a bash array (`libs=(...)`), and `${#libs[@]}` --
none of which dash supports -- and died with `shopt: not found`, exit 127,
*after* the artifact had already been staged correctly (a false failure, not
a real one).

This script is a COMMITTED but NOT-wired-into-CI pre-push check (same
deliberate-unwired posture as ci_conventions_check.py -- see that file's
docstring for the rationale: keep the workflow YAML itself the single source
of truth for what CI runs, and run mechanical checks by hand or via a local
git hook before push). Scope is deliberately limited to
`linux_build.yml` -- the only workflow file whose job declares
`container:` and therefore resolves every `run:` step to POSIX `sh`, not
`bash` (confirmed by census in round-6 root-cause analysis: 24/24 step
banners in that job print `shell: sh -e {0}`). `android_build.yml` and
`macos_build.yml` contain the same bash-only constructs (array literals,
`shopt`) but run on hosted runners where the default shell is genuinely
bash, so they are correct as written and out of scope for this checker --
flagging them would be a false positive, not a caught defect.

Detected constructs: `shopt`, `declare`, `mapfile`, `readarray`, `[[ ... ]]`,
`PIPESTATUS`, `${#name[`, array-assignment `name=(...)`, process substitution
`<(...)`. Lines that are pure comments (after stripping leading whitespace)
are ignored.

Exit 0 iff no workflow file contains a bash-only construct. Prints `CLEAN`
on success; otherwise one `<file>:<line>: <text>` line per hit, then exits 1.

Run: python3 native/scripts/check_workflow_bashisms.py
"""
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"
# Only the containerised leg's workflow file is in scope -- see module
# docstring for why android_build.yml / macos_build.yml are excluded.
DEFAULT_TARGET_FILES = ("linux_build.yml",)

_BASHISM_PATTERN = re.compile(
    r"(^|[^A-Za-z_])(shopt|declare|mapfile|readarray)\b"
    r"|\[\["
    r"|\bPIPESTATUS\b"
    r"|\$\{#[A-Za-z_][A-Za-z0-9_]*\["
    r"|\b[A-Za-z_][A-Za-z0-9_]*=\("
    r"|<\("
)


def find_bashisms(workflows_dir, target_files=DEFAULT_TARGET_FILES):
    """Return a list of (path, line_no, line_text) for every bash-only hit
    in the given target filenames (default: only the containerised leg)."""
    hits = []
    for name in target_files:
        path = pathlib.Path(workflows_dir) / name
        if not path.exists():
            continue
        for i, line in enumerate(path.read_text().splitlines(), start=1):
            if line.lstrip().startswith("#"):
                continue
            if _BASHISM_PATTERN.search(line):
                hits.append((path, i, line))
    return hits


def main():
    hits = find_bashisms(DEFAULT_WORKFLOWS_DIR)
    if not hits:
        print("CLEAN")
        return 0
    for path, lineno, text in hits:
        print(f"{path.relative_to(REPO_ROOT)}:{lineno}: {text}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
