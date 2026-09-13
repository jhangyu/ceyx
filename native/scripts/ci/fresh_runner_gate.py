#!/usr/bin/env python3
"""fresh_runner_gate.py -- WI-45 (USER RULING F): the local-gate leg for
environment-dependent defects.

WHY THIS EXISTS: five environment-dependency defects landed in one day
(LibRaw provenance guard, the linkage table, a leader's own "spot-verified
four of seven", the alias-table wiring, the expected-additions ledger
validator) -- each one passed on a developer machine and failed on a
fresh CI runner. The common shape, precisely: a path exists on the dev
machine's disk but is ABSENT from `git ls-files` -- a sibling repo
checkout (e.g. ../Halcyon) and a gitignored vendored third-party tree
(e.g. native/third_party/libraw/, fetched by a build step that runs
after the local guards) are the same defect wearing different clothes.

The existing local gate recipe (docs/logs/2026-09-13/pyci-plan.md, never
committed -- docs/ is never version-controlled in this repo) bind-mounts
the LIVE host checkout into a container (`-v "$PWD":/w`). That mount
carries every already-fetched, gitignored vendored tree and every
sibling-adjacent fact a laptop happens to have -- none of which a fresh
`actions/checkout` on a CI runner ever sees. This script closes that gap
by running the same guard/selftest block against a `git worktree add`
checkout instead of the live tree: a worktree contains ONLY git-tracked
content (gitignored/untracked files never appear in it), so it is the
closest mechanically-verifiable stand-in for a fresh runner checkout this
repo can produce without a real CI run.

WHAT THIS SIMULATES: absence of (1) any sibling directory next to the
repo root (the ../Halcyon class -- WI-42's defect) and (2) any gitignored
vendored/fetched tree inside the repo (the native/third_party/libraw/
class -- WI-44's defect, historically caught this way against tip
bba2a752, the commit immediately before WI-44's fix).

WHAT THIS DOES NOT SIMULATE (named, not silent): actual OS/toolchain
differences (this always runs on the host OS, e.g. macOS here vs Linux
on the `linux` legs -- the existing `docker run ubuntu:22.04` recipe in
pyci-plan.md still owns that axis and is not superseded by this script),
GitHub Actions env vars/secrets, and network-fetch behaviour. This is the
MINIMAL version (USER RULING F): no configuration, no matrix, no
per-platform variants -- one hardcoded, self-printing check list. The
polished version is parking-lot guard (e).

This is a Python script, not a shell script, deliberately: an earlier
`.sh` version of this same leg failed its own `check_shell_prohibition.py`
guard once committed -- a new shell file in a campaign whose purpose is
removing shell from CI. The absence leg cannot be the one shell file the
campaign keeps. Process execution goes through `native/scripts/ci/run.py`
(the one sanctioned subprocess wrapper), not raw `subprocess`, so this
script also clears `check_no_test_execution_in_ci.py`'s
`[subprocess-outside-run]` rule.

Run with: python3 native/scripts/ci/fresh_runner_gate.py   (from repo root)
"""
from __future__ import annotations

import shutil
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run  # noqa: E402 -- native/scripts/ci/run.py, the one sanctioned
# subprocess wrapper. This script isn't invoked as part of the `ci` package
# (it also runs against a git-worktree COPY of itself, so a relative
# `from . import run` would break there), hence the sys.path shim.

REPO_ROOT = Path(__file__).resolve().parents[3]

# Fixed, self-printing check list (minimal version: no discovery, no
# config -- see USER RULING F). Any check left out of this list is a
# silent-scope regression; add it here, do not special-case it below.
CHECKS = [
    "native/scripts/check_workflow_bashisms.py",
    "native/scripts/ci/check_no_test_execution_in_ci.py",
    "native/scripts/ci/check_shell_prohibition.py",
    "native/scripts/ci_conventions_check.py",
    "native/scripts/ci/check_cmake_sources_tracked.py",
    "native/scripts/ci/check_expected_additions.py",
]


def main() -> int:
    tip_result = run.run(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT)
    tip = tip_result.stdout.strip()
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    print(f"FRESH_RUNNER_GATE: tip={tip} date={now}")

    scratch_dir = Path(tempfile.mkdtemp(prefix="ceyx-fresh-runner-gate."))
    worktree_path = scratch_dir / "checkout"

    try:
        print(f"FRESH_RUNNER_GATE: creating worktree at {worktree_path}")
        add_result = run.run(
            ["git", "worktree", "add", "--quiet", "--detach", str(worktree_path), tip],
            cwd=REPO_ROOT,
        )
        if add_result.returncode != 0:
            print(
                f"FRESH_RUNNER_GATE: worktree creation FAILED rc={add_result.returncode} "
                f"stderr={add_result.stderr}",
                file=sys.stderr,
            )
            return 2

        # ---- Declare what is simulated as absent -- mechanically verified,
        # not assumed. A leg whose absence claim is wrong is worse than no leg.
        sibling_halcyon = worktree_path.parent / "Halcyon"
        libraw_tree = worktree_path / "native" / "third_party" / "libraw"

        print(f"FRESH_RUNNER_GATE: SIMULATED_ABSENT sibling-checkout ({sibling_halcyon})")
        if sibling_halcyon.exists():
            print(
                f"FRESH_RUNNER_GATE: ABSENCE_CLAIM_FALSE -- {sibling_halcyon} exists, "
                "leg is not trustworthy",
                file=sys.stderr,
            )
            return 2
        print("FRESH_RUNNER_GATE: ABSENCE_CONFIRMED sibling-checkout")

        # native/third_party/libraw/PROVENANCE.md is the one file this repo
        # DOES track under that path (.gitignore:72-74) -- its presence is
        # expected and does not represent the fetched vendor tree; anything
        # else there does.
        unexpected = None
        if libraw_tree.exists():
            for p in libraw_tree.rglob("*"):
                if p.is_file() and p.name != "PROVENANCE.md":
                    unexpected = p
                    break
        print(
            f"FRESH_RUNNER_GATE: SIMULATED_ABSENT vendored-tree ({libraw_tree}, "
            "excluding tracked PROVENANCE.md)"
        )
        if unexpected is not None:
            print(
                f"FRESH_RUNNER_GATE: ABSENCE_CLAIM_FALSE -- {unexpected} exists, "
                "leg is not trustworthy",
                file=sys.stderr,
            )
            return 2
        print(
            "FRESH_RUNNER_GATE: ABSENCE_CONFIRMED vendored-tree "
            "(only PROVENANCE.md, if anything, is tracked)"
        )

        print(
            "FRESH_RUNNER_GATE: EXCLUDED_FROM_LEG none -- all 5 named local guards "
            "plus check_expected_additions.py run"
        )
        print(
            "FRESH_RUNNER_GATE: EXCLUDED_FROM_LEG note -- 'ci.py selftest' and "
            "'pytest native/scripts/' are NOT run by"
        )
        print(
            "FRESH_RUNNER_GATE: EXCLUDED_FROM_LEG note -- this minimal leg "
            "(parking-lot polish item); this leg covers"
        )
        print(
            "FRESH_RUNNER_GATE: EXCLUDED_FROM_LEG note -- only the "
            "absence-sensitive local guards named above"
        )

        overall_rc = 0
        for check in CHECKS:
            print(f"FRESH_RUNNER_GATE: RUN {check}")
            result = run.run([sys.executable, check], cwd=worktree_path)
            rc = result.returncode
            if result.stdout:
                print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
            if result.stderr:
                print(result.stderr, end="" if result.stderr.endswith("\n") else "\n", file=sys.stderr)
            print(f"FRESH_RUNNER_GATE: RC({check})={rc}")
            if rc != 0:
                overall_rc = 1

        print(f"FRESH_RUNNER_GATE: OVERALL_RC={overall_rc} tip={tip}")
        return overall_rc
    finally:
        run.run(["git", "worktree", "remove", "--force", str(worktree_path)], cwd=REPO_ROOT)
        shutil.rmtree(scratch_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
