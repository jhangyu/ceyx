#!/usr/bin/env python3
"""Mechanical checker: bash-only syntax in workflow `run:` steps that execute
under POSIX `sh`, not `bash`, PLUS (WI-25, push 9 closeout) a second,
independent instrument for the campaign's central property -- "every `run:`
body is either a compliant one-line `python3 ...` invocation, or explicitly
allowlisted."

Root cause of the round-6 CI failure (task #17/#18, run 34706875811): the
Linux leg of `.github/workflows/linux_build.yml` runs inside `container:
ubuntu:22.04`, and every step banner in that job resolves to `shell: sh -e
{0}` (dash), never `bash`. The step "Fail if no shared library was produced"
used `shopt -s nullglob`, a bash array (`libs=(...)`), and `${#libs[@]}` --
none of which dash supports -- and died with `shopt: not found`, exit 127,
*after* the artifact had already been staged correctly (a false failure, not
a real one).

WI-25 retarget (push 9 closeout, plan `WI-25: retarget the bashism checker;
final ratchet; delete dead shell`): the checker's *value* inverts from "find
bashisms in linux_build.yml only" to "police the residual must-stay shell"
across every workflow file. Two things changed:

1. `find_bashisms()`'s bash-only-construct scan now runs against EVERY
   workflow file under `.github/workflows/`, not just `linux_build.yml`.
   This was previously scoped narrowly because `android_build.yml` /
   `macos_build.yml` were believed to carry the same bash-only constructs
   deliberately (they run on hosted bash runners, so `shopt`/arrays would be
   CORRECT there, not a defect) -- but a fresh census at WI-25 time (every
   workflow file, same `_BASHISM_PATTERN`) found ZERO such constructs
   outside `linux_build.yml` today: the campaign's own shell-to-python
   migration removed them. Widening is therefore safe now and is re-verified
   by `test_real_workflow_dir_is_clean_after_round6_fix`, which scans every
   workflow file, not a named subset.
2. A NEW, independently-implemented check --
   `find_non_compliant_bodies()` -- fails if any workflow file contains a
   `run:` body that is neither a compliant one-line `python3 ...`/`python
   ...`/`pwsh -c python ...` invocation NOR listed in
   `native/scripts/ci/allowlist.py`'s `MUST_STAY`. This is DELIBERATELY the
   same property `native/scripts/ci/check_shell_prohibition.py`'s Rule 1
   already checks -- kept as a second, separately-written instrument rather
   than importing that module's check function, per the 2026-09-06 lesson
   this campaign's own handover cites: a manual's own checklist (here, a
   single checker) can lead an operator past the only check that detects a
   silent failure, so the property is worth two independent codepaths, not
   one codepath run twice.

This script remains a COMMITTED but NOT-wired-into-CI pre-push check (same
deliberate-unwired posture as `ci_conventions_check.py` -- see that file's
docstring for the rationale: keep the workflow YAML itself the single source
of truth for what CI runs, and run mechanical checks by hand or via a local
git hook before push).

Detected bashisms: `shopt`, `declare`, `mapfile`, `readarray`, `[[ ... ]]`,
`PIPESTATUS`, `${#name[`, array-assignment `name=(...)`, process substitution
`<(...)`. Lines that are pure comments (after stripping leading whitespace)
are ignored.

`find_non_compliant_bodies()` reuses `native/scripts/ci/workflow_scan.py`'s
line-based `run:` body parser -- which does NOT collapse a YAML folded
scalar (`run: >`) into one line; a step that is semantically one Python
invocation but is written across several physical lines under `run: >`
therefore shows up as "non-compliant" here exactly as it does in
`check_shell_prohibition.py`'s Rule 1 (same instrument-limitation, found
independently by both checks -- see WI-25's own handover note on the three
`*_dist_android.yml` carrier-build steps and `webp_dist_windows.yml`'s
carrier step, all pre-existing allowlist entries at WI-25 time).

Exit 0 iff BOTH checks are clean. Prints `CLEAN` on success; otherwise one
`<file>:<line>: [bashism] <text>` or `<file>:<line>: [non-python-body]
<step name>: <n> code lines` line per hit, then exits 1.

Run: python3 native/scripts/check_workflow_bashisms.py
"""
from __future__ import annotations

import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))

_BASHISM_PATTERN = re.compile(
    r"(^|[^A-Za-z_])(shopt|declare|mapfile|readarray)\b"
    r"|\[\["
    r"|\bPIPESTATUS\b"
    r"|\$\{#[A-Za-z_][A-Za-z0-9_]*\["
    r"|\b[A-Za-z_][A-Za-z0-9_]*=\("
    r"|<\("
)

# Independent second instrument for the same property `check_shell_
# prohibition.py`'s Rule 1 already checks -- written separately, not
# imported, per the module docstring's D2026-09-06 rationale.
_PYTHON_BODY_RE = re.compile(r"^(python3|python|pwsh -c python)\s")


def _default_target_files(workflows_dir=DEFAULT_WORKFLOWS_DIR):
    """Every workflow file that currently exists -- WI-25 widens both checks
    from the round-6-era `("linux_build.yml",)` default to the whole
    directory, derived by listing it (never a hardcoded name list, so a
    future workflow file is covered without editing this function)."""
    return tuple(sorted(p.name for p in pathlib.Path(workflows_dir).glob("*.yml")))


def find_bashisms(workflows_dir, target_files=None):
    """Return a list of (path, line_no, line_text) for every bash-only hit
    in the given target filenames (default: every workflow file, WI-25)."""
    if target_files is None:
        target_files = _default_target_files(workflows_dir)
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


def find_non_compliant_bodies(workflows_dir, target_files=None):
    """WI-25's new assertion: a `run:` body that is neither a compliant
    one-line python invocation nor allowlisted is a failure. Returns a list
    of (path, line_no, step_name, code_line_count) for every such body.

    Independently re-derives compliance from `workflow_scan.iter_run_steps`/
    `code_lines` and `ci.allowlist.MUST_STAY` -- the same primitives
    `check_shell_prohibition.py` uses, but this function's own comparison
    logic is written fresh here rather than calling into that module,
    per the module docstring's "two instruments, one property" rationale."""
    from ci import allowlist, workflow_scan  # noqa: E402  (sys.path set at import time)

    if target_files is None:
        target_files = _default_target_files(workflows_dir)
    lookup = {(e.workflow, e.step_name) for e in allowlist.MUST_STAY}

    hits = []
    for name in target_files:
        path = pathlib.Path(workflows_dir) / name
        if not path.exists():
            continue
        text = path.read_text()
        for step in workflow_scan.iter_run_steps(text, path.name):
            code = workflow_scan.code_lines(step)
            compliant = len(code) == 1 and bool(_PYTHON_BODY_RE.match(code[0]))
            if compliant:
                continue
            if (step.workflow, step.step_name) in lookup:
                continue
            hits.append((path, step.start_line, step.step_name, len(code)))
    return hits


def main():
    bashism_hits = find_bashisms(DEFAULT_WORKFLOWS_DIR)
    non_compliant_hits = find_non_compliant_bodies(DEFAULT_WORKFLOWS_DIR)

    if not bashism_hits and not non_compliant_hits:
        print("CLEAN")
        return 0

    for path, lineno, text in bashism_hits:
        print(f"{path.relative_to(REPO_ROOT)}:{lineno}: [bashism] {text}")
    for path, lineno, step_name, n in non_compliant_hits:
        print(
            f"{path.relative_to(REPO_ROOT)}:{lineno}: [non-python-body] "
            f"{step_name}: {n} code lines, not allowlisted"
        )
    return 1


if __name__ == "__main__":
    sys.exit(main())
