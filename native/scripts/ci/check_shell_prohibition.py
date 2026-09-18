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
4. Rule 4 (WI-4b, `[obsolete-allowlist]`): the other half of the same
   safety catch. Rule 3's `[stale-allowlist]` only fires when a step's
   `- name:` no longer exists at all; a migrated step keeps its exact name
   and changes only its BODY (shell -> one-line `python3`), so
   `[stale-allowlist]` stays silent while the allowlist keeps a permission
   for shell that is gone. For every allowlist entry whose step DOES still
   exist: if that step's body is already compliant (exactly one code line
   matching Rule 1's `PYTHON_BODY_RE`), the exemption is dead and it is an
   `[obsolete-allowlist]` failure naming the step and instructing the
   ratchet. This converts "remember to ratchet after a migration" from
   scheduling discipline into a mechanical check.

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

# Dual-mode import, deliberately -- read this before changing it.
#
# This module is invoked TWO different ways and both must work:
#   (a) as a bare script -- `python3 native/scripts/ci/check_shell_prohibition.py`,
#       which is exactly what .github/workflows/build.yml's "verify-native-tests"
#       job runs. A bare script has NO parent package (`__package__` is "" or
#       None) -- Python puts the script's OWN directory (native/scripts/ci/) on
#       sys.path[0], not native/scripts/, so `ci` is not importable as a
#       package from there, and a *relative* import (`from . import x`) is a
#       hard `ImportError: attempted relative import with no known parent
#       package` in this mode. There is no such thing as a relative import
#       with no package.
#   (b) as a package member -- imported by `ci.py selftest`'s in-process
#       discovery (identity `ci.*`), OR by `python3 -m unittest discover
#       -s native/scripts/ci/tests -t .` (identity `native.scripts.ci.*`).
#       Here a *relative* import is required, not merely permitted: an
#       absolute `import ci.x` binds whichever identity happened to load
#       first in THIS PROCESS and silently succeeds against the wrong copy
#       of the module for any OTHER identity active in the same process
#       (push-1's near-miss, tmp/verify/pyci-push1-gate-VERDICT.md -- a
#       `mock.patch("ci.report.error")` bound to a different module object
#       than the one the code under test actually imported).
#
# These two failure modes point in OPPOSITE directions (absolute imports
# break (b), relative imports break (a)), so the fix is not "pick one" but
# "detect which mode this run is in and import accordingly". `__package__`
# is the discriminator: it is falsy ONLY for a bare-script run, in every
# package-member mode it is a real (possibly empty-string-for-top-level-only
# in unusual layouts, but never here) dotted name. This does NOT reintroduce
# the push-1 defect: that defect was two DIFFERENT identities of the SAME
# module coexisting in ONE process; a bare-script invocation is always a
# brand-new interpreter process with exactly one identity for this module
# (whatever `import ci.x` resolves to right here, right now), so there is no
# second copy for anything to disagree with.
if not __package__:
    import ci.allowlist as allowlist  # noqa: E402
    import ci.workflow_scan as workflow_scan  # noqa: E402
else:
    from . import allowlist  # noqa: E402
    from . import workflow_scan  # noqa: E402

WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

PYTHON_BODY_RE = re.compile(r"^(python3|python|pwsh -c python)\s")

# Guard (d): an inline `python3 -c "..."` / `python -c ...` invocation matches
# PYTHON_BODY_RE's interpreter prefix but carries its actual code on the
# command line, not in any file this repo's guards scan -- shell-carried
# Python, invisible to every file-scanning check this campaign built (P-15).
# Scope is `-c` ONLY (ruling, SIGNOFF-LEDGER "(d) flags -c only NOT -m") --
# `python -m <module>` is deliberately NOT flagged by this predicate.
_INLINE_DASH_C_RE = re.compile(r"^(python3|python|pwsh -c python)\s+-c\b")

_SHELL_GLOB_PATTERNS = ("*.sh", "*.bat", "*.ps1", "*.cmd")


def _is_rule1_compliant(code: list) -> bool:
    """The ONE predicate for 'is this run: body an acceptable one-line python
    invocation', used identically by Rule 1 (skip) and the obsolete-allowlist
    reverse sentinel `check_obsolete_entries` (fail) -- deliberately the SAME
    function so the two can never diverge (2026-09-13 finding: narrowing one
    copy of a duplicated predicate silently loosens whichever consumer reads
    the other copy in the opposite direction)."""
    return (
        len(code) == 1
        and bool(PYTHON_BODY_RE.match(code[0]))
        and not bool(_INLINE_DASH_C_RE.match(code[0]))
    )


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
    if not __package__:
        import ci.run as ci_run  # noqa
    else:
        from . import run as ci_run  # noqa

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
            compliant = _is_rule1_compliant(code)
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


def _steps_by_key(workflow_files) -> dict:
    """(workflow, step_name) -> the step's RunStep, for every `run:` step
    that currently exists in any workflow file. Used by
    `check_obsolete_entries` (WI-4b); built once in `main()`, independent
    of Rule 1/Rule 3's own internal loops so this does not risk changing
    either rule's existing behaviour or text."""
    steps = {}
    for path in workflow_files:
        text = path.read_text()
        for step in workflow_scan.iter_run_steps(text, path.name):
            steps[(step.workflow, step.step_name)] = step
    return steps


def check_obsolete_entries(steps_by_key: dict, entries) -> list:
    """WI-4b, Rule 4: for every allowlist entry whose step still EXISTS,
    flag it if that step's body is already a compliant one-line `python3`
    call -- the exemption it grants is dead. An entry whose step no longer
    exists at all is Rule 3's `[stale-allowlist]` concern, not this one;
    the two rules are deliberately disjoint (this function is silent on a
    missing step) so neither masks the other."""
    failures: list = []
    for entry in entries:
        step = steps_by_key.get((entry.workflow, entry.step_name))
        if step is None:
            continue
        code = workflow_scan.code_lines(step)
        compliant = _is_rule1_compliant(code)
        if not compliant:
            continue
        failures.append(
            f"{step.workflow}:{step.start_line}: [obsolete-allowlist] '{step.step_name}' is "
            "already a one-line python3 body; its exemption is dead. Remove the entry and "
            "decrement ALLOWLIST_SIZE_EXPECTED (ratchet)."
        )
    return failures


def main(argv=None, repo_root: Path = REPO_ROOT, workflows_dir=None) -> int:
    failures: list = []
    workflows_dir = workflows_dir if workflows_dir is not None else (repo_root / ".github" / "workflows")
    workflow_files = sorted(workflows_dir.glob("*.yml"))

    matched_entries = _rule1_run_bodies(failures, workflow_files)
    _rule2_no_new_shell_files(failures, repo_root)
    _rule3_allowlist_is_loud(failures, matched_entries, workflow_files)
    failures.extend(check_obsolete_entries(_steps_by_key(workflow_files), allowlist.MUST_STAY))

    if failures:
        for f in failures:
            print(f, file=sys.stderr)
        print(f"SHELL_PROHIBITION_RESULT=FAIL ({len(failures)} failure(s))")
        return 1

    print("SHELL_PROHIBITION_RESULT=PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
