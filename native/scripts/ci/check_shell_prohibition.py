#!/usr/bin/env python3
"""The shell-prohibition guard (WI-4, USER ORDER, first-class).

Three rules, evaluated at run time against whatever exists on disk today
(so future additions are covered, not just today's files), and every rule
runs even if an earlier one already failed -- accumulate-all-failures,
mirroring Halcyon `phases.py:70-85`:

1. Rule 1 (run bodies): every `run:` body in `.github/workflows/*.yml` must
   have exactly one non-comment code line matching
   `^(python3|python|pwsh -c python)\\s`, OR the owning step must be on
   MUST_STAY (matched by (workflow, step_name) exactly). Anything else is a
   `[shell-body]` failure.
2. Rule 2 (no new shell files): a repo-wide glob for `*.sh`/`*.bat`/`*.ps1`/
   `*.cmd` (excluding `.git/`, `build*/`, `native/third_party/`, `tmp/`),
   minus GRANDFATHERED_SHELL_FILES by exact path. Any other hit is a
   `[new-shell-file]` failure.
3. Rule 3 (the allowlist is loud): print `SHELL_ALLOWLIST_SIZE=<n>` and one
   `ALLOWED <workflow> :: <step name> -- <reason>` line per entry, on EVERY
   run, pass or fail (no-silent-caps). An allowlist entry whose
   (workflow, step_name) no longer exists in ANY workflow is a
   `[stale-allowlist]` failure -- the ratchet's own safety catch: without
   this, a migrated step's leftover entry rots invisibly instead of being
   forced out in the same commit that migrates it.

Run with: python3 native/scripts/ci/check_shell_prohibition.py
"""

from __future__ import annotations

import glob
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))

# Relative imports, not `import ci.allowlist`/`import ci.workflow_scan`:
# this package is importable under TWO module identities (`ci.*` when
# native/scripts is the top-level dir, as `ci.py selftest` sets up, and
# `native.scripts.ci.*` when the repo root is -- e.g. `python3 -m unittest
# discover -s native/scripts/ci/tests -t .`). An absolute `import ci.x`
# binds whichever identity happened to be resolved first and is therefore
# import-order-dependent; a relative import resolves against THIS module's
# own real package at runtime under either identity (push-1 gate finding,
# tmp/verify/pyci-push1-gate-VERDICT.md).
from . import allowlist  # noqa: E402
from . import workflow_scan  # noqa: E402

WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

PYTHON_BODY_RE = re.compile(r"^(python3|python|pwsh -c python)\s")

_SHELL_GLOB_PATTERNS = ("*.sh", "*.bat", "*.ps1", "*.cmd")


def _is_excluded(rel_path: str) -> bool:
    parts = rel_path.split("/")
    # ".git/", "native/third_party/", "tmp/" -- any PATH COMPONENT match, not
    # just a leading prefix: a scratch dir nested at native/scripts/tmp/ is
    # exactly as much "tmp/" as a top-level one (this project's own scratch
    # lane convention, CLAUDE.md), and native/third_party/ is a component
    # test too so it also excludes matches under a differently-rooted checkout.
    if any(p in (".git", "third_party", "tmp") for p in parts[:-1]):
        return True
    # "build*/" -- any path component starting with "build"
    if any(p.startswith("build") for p in parts[:-1]):
        return True
    return False


def _git_ignored_relpaths(repo_root: Path, candidates: list) -> set:
    """Returns the subset of `candidates` (repo-root-relative paths) that git
    would treat as ignored. Rule 2 is "no NEW shell file" -- a file .gitignore
    already keeps out of the tree (a CocoaPods/Gradle/Flutter-ephemeral
    artifact) was never something this campaign could have prohibited, so it
    is excluded by the same mechanism the repository itself uses to say
    "this isn't tracked content". Returns an empty set (no filtering) if
    `repo_root` is not inside a git work tree at all -- e.g. an isolated
    fixture directory a test plants -- so tests are unaffected by this.
    """
    if not candidates:
        return set()
    from . import run as ci_run  # relative -- see the import-order note above

    probe = ci_run.run(["git", "-C", os.fspath(repo_root), "rev-parse", "--is-inside-work-tree"])
    if probe.returncode != 0:
        return set()
    result = ci_run.run(["git", "-C", os.fspath(repo_root), "check-ignore", *candidates])
    if result.returncode not in (0, 1):
        return set()
    return {line for line in result.stdout.splitlines() if line}


def _iter_shell_files(repo_root: Path):
    candidates = []
    for pattern in _SHELL_GLOB_PATTERNS:
        for match in glob.glob(str(repo_root / "**" / pattern), recursive=True):
            rel = os.path.relpath(match, repo_root).replace(os.sep, "/")
            if _is_excluded(rel):
                continue
            candidates.append(rel)
    ignored = _git_ignored_relpaths(repo_root, candidates)
    for rel in candidates:
        if rel in ignored:
            continue
        yield rel


def _allowlist_lookup():
    return {(e.workflow, e.step_name): e.reason for e in allowlist.MUST_STAY}


def _rule1_run_bodies(failures: list, workflow_files):
    lookup = _allowlist_lookup()
    matched_entries = set()
    for path in workflow_files:
        text = path.read_text()
        for step in workflow_scan.iter_run_steps(text, path.name):
            key = (step.workflow, step.step_name)
            code = workflow_scan.code_lines(step)
            compliant = len(code) == 1 and bool(PYTHON_BODY_RE.match(code[0]))
            if compliant:
                continue
            if key in lookup:
                matched_entries.add(key)
                continue
            failures.append(
                f"{step.workflow}:{step.start_line}: [shell-body] "
                f"{step.step_name}: {len(code)} code lines"
            )
    return matched_entries


def _rule2_no_new_shell_files(failures: list, repo_root: Path = REPO_ROOT):
    grandfathered = set(allowlist.GRANDFATHERED_SHELL_FILES)
    for rel in sorted(_iter_shell_files(repo_root)):
        if rel in grandfathered:
            continue
        failures.append(f"{rel}: [new-shell-file] shell file outside the grandfathered list")


def _rule3_allowlist_is_loud(failures: list, matched_entries: set, workflow_files):
    # Every (workflow, step_name) that currently exists in any workflow.
    live_steps = set()
    for path in workflow_files:
        text = path.read_text()
        for step in workflow_scan.iter_run_steps(text, path.name):
            live_steps.add((step.workflow, step.step_name))

    print(f"SHELL_ALLOWLIST_SIZE={len(allowlist.MUST_STAY)}")
    for entry in allowlist.MUST_STAY:
        print(f"ALLOWED {entry.workflow} :: {entry.step_name} -- {entry.reason}")
        if (entry.workflow, entry.step_name) not in live_steps:
            failures.append(
                f"allowlist:{entry.workflow}:{entry.step_name}: [stale-allowlist] "
                f"no such step exists in any workflow"
            )

    if len(allowlist.MUST_STAY) != allowlist.ALLOWLIST_SIZE_EXPECTED:
        failures.append(
            f"[allowlist-size-mismatch] len(MUST_STAY)={len(allowlist.MUST_STAY)} != "
            f"ALLOWLIST_SIZE_EXPECTED={allowlist.ALLOWLIST_SIZE_EXPECTED}"
        )


def main(argv=None, repo_root: Path = REPO_ROOT, workflows_dir=None) -> int:
    failures: list = []
    workflows_dir = workflows_dir if workflows_dir is not None else (repo_root / ".github" / "workflows")
    workflow_files = sorted(workflows_dir.glob("*.yml"))

    matched_entries = _rule1_run_bodies(failures, workflow_files)
    _rule2_no_new_shell_files(failures, repo_root)
    _rule3_allowlist_is_loud(failures, matched_entries, workflow_files)

    if failures:
        for f in failures:
            print(f, file=sys.stderr)
        print(f"SHELL_PROHIBITION_RESULT=FAIL ({len(failures)} failure(s))")
        return 1

    print("SHELL_PROHIBITION_RESULT=PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
