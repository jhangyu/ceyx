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

WHAT THIS SIMULATES: absence of (1) the ../Halcyon sibling checkout
(WI-42's defect) and (2) EVERY gitignored vendored/fetched tree under
native/third_party/ -- not a hand-written list of two. Parking-lot guard
(e), FULL BAR: the set of vendored roots to assert absent is DERIVED, via
`git check-ignore` (never a hand-rolled .gitignore parser -- see
`discover_vendored_roots()` below), from (a) whatever this machine has
actually fetched on disk and (b) whatever git already tracks under
native/third_party/ (so a tree nobody has fetched here yet, e.g. Halide
on a clean clone, is still named and checked). This closes the UNPRINTED
gap recorded in docs/logs/2026-09-13/pyci-ruling-G-guard-plan.md SS(e)3c:
the minimal version's two-path hardcode (native/third_party/libraw/ and
../Halcyon) covered WI-44's actual defect but SILENTLY excluded every
other vendored tree (dng_sdk subtrees, the partial libjpeg-turbo tree,
the heif/webp/jxl -dist-* directories, the libomp/lcms2/jpegturbo
prebuilt dylibs, and Halide) from its absence claim without ever saying
so. A `git worktree` checkout mechanically excludes all of them either
way -- the gap was in the REPORT, not in the coverage -- but an unnamed
absence is not a declared one, which is the property this whole script
exists to enforce elsewhere; see VENDORED_ROOTS_ASSERTED_ABSENT below for
the printed, auditable list this run actually checked.

DESIGN HAZARD (read before touching discover_vendored_roots(), it cost a
round to find): .gitignore carries several negations that re-include
WHOLE committed directories (e.g. `!native/third_party/heif-dist-
android-arm64-v8a/`), because those dists are reviewed, pinned INPUTS
this repo cannot rebuild on every machine. `git check-ignore` on such a
negated path correctly reports "not ignored" -- which is why this script
asks git per-candidate-path rather than assuming every native/third_party
child is a vendored tree. Do not special-case those names; let
`git check-ignore` decide, exactly as git itself resolves negation and
precedence.

WHAT THIS DOES NOT SIMULATE (named, not silent): actual OS/toolchain
differences (this always runs on the host OS, e.g. macOS here vs Linux
on the `linux` legs -- the existing `docker run ubuntu:22.04` recipe in
pyci-plan.md still owns that axis and is not superseded by this script),
GitHub Actions env vars/secrets, and network-fetch behaviour. Also named,
not silent: the sibling-checkout leg above only checks ../Halcyon by
name, not "any sibling directory" generically -- that remains a
narrower-than-the-property hardcode, unlike the vendored-tree leg, which
this round widened. This is still the leg named in USER RULING F: no
configuration, no matrix, no per-platform variants -- the check LIST
(`CHECKS` below) is one hardcoded, self-printing list; only the
vendored-root DISCOVERY inside it has been generalized this round.

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

# Seeded on top of whatever the discovery below finds on disk/in the tree:
# fetch targets this repo builds that may not exist on the machine running
# this leg (e.g. a clean clone that has never run `build_deps.py fetch
# halide`) and that carry no tracked placeholder (PROVENANCE.md or
# otherwise) for the tree-scan half of discovery to find. Keep this list to
# "known fetch targets with nothing tracked under them" only -- everything
# else is derived, not hand-maintained; see docstring DESIGN HAZARD note.
_SEEDED_CANDIDATES = ("halide",)


def discover_vendored_roots(repo_root: Path) -> list[Path]:
    """Return every path under native/third_party/ that `git check-ignore`
    says is ignored -- i.e. a vendored/fetched tree, not tracked source.

    Candidates to ask git about come from THREE sources, unioned, so the
    result does not depend on what happens to be fetched on the machine
    running this leg:
      1. directories that actually exist on disk under native/third_party/
         (whatever this machine has fetched), one level deep and one level
         under each of those (covers dng_sdk/targets, libjpeg-turbo/java,
         etc. -- trees whose PARENT is tracked but a CHILD is ignored);
      2. directories git already tracks under native/third_party/ (covers
         a vendored root that is fully or partly negated back in, e.g.
         libomp/, so its ignored siblings are still named even when the
         only thing on disk is the tracked PROVENANCE.md/dylib);
      3. `_SEEDED_CANDIDATES`, for fetch targets with no tracked trace at
         all on a clean clone (Halide).

    Ignored-ness is decided ENTIRELY by `git check-ignore` -- this function
    never reads or interprets .gitignore patterns itself, per the guard
    plan's instruction to ask git rather than hand-roll ignore semantics
    (negations/precedence/depth are exactly what check-ignore resolves).
    """
    third_party = repo_root / "native" / "third_party"
    candidates: set[str] = set(_SEEDED_CANDIDATES)

    if third_party.is_dir():
        for child in sorted(third_party.iterdir()):
            if not child.is_dir():
                continue
            candidates.add(child.name)
            for grandchild in sorted(child.iterdir()):
                if grandchild.is_dir():
                    candidates.add(f"{child.name}/{grandchild.name}")

    tracked = run.run(
        ["git", "ls-tree", "-r", "-d", "--name-only", "HEAD", "native/third_party"],
        cwd=repo_root,
    )
    if tracked.returncode == 0:
        for line in tracked.stdout.splitlines():
            line = line.strip()
            if not line:
                continue
            prefix = "native/third_party/"
            rel = line[len(prefix):] if line.startswith(prefix) else line
            parts = rel.split("/")
            if parts and parts[0]:
                candidates.add(parts[0])
                if len(parts) > 1 and parts[1]:
                    candidates.add(f"{parts[0]}/{parts[1]}")

    ignored_roots: list[Path] = []
    for name in sorted(candidates):
        rel_path = f"native/third_party/{name}"
        # HOW TO RE-DERIVE THIS, not just trust it -- re-verify with:
        #   git check-ignore -v native/third_party/libraw       (RC=1, silent)
        #   git check-ignore -v native/third_party/libraw/      (RC=1, silent)
        #   git check-ignore -v --no-index native/third_party/libraw/  (RC=0, prints match)
        # A plain (index-aware) `git check-ignore` on a directory that has
        # even ONE tracked file inside it (e.g. libraw/'s tracked
        # PROVENANCE.md) answers "not ignored" for the directory itself --
        # it is reporting on the INDEX ENTRY, not on the ignore PATTERN.
        # --no-index asks the pattern-matching question this function
        # actually needs. The trailing slash is independently required:
        # git's directory-only patterns (the common shape here, `foo/`)
        # only match a queried path that itself ends in `/`. Drop either
        # flag and this silently regresses to under-reporting roots like
        # libraw/ that are the exact regression this file exists to catch --
        # re-run the three commands above against any future .gitignore
        # change before "simplifying" this call.
        check = run.run(
            ["git", "check-ignore", "--no-index", "--quiet", f"{rel_path}/"],
            cwd=repo_root,
        )
        if check.returncode == 0:
            ignored_roots.append(Path(rel_path))
    return ignored_roots


def _tracked_files_under(repo_root: Path, rel_path: Path) -> set[str]:
    """Files git actually tracks under `rel_path` (POSIX-relative strings),
    generalizing the old single-file `PROVENANCE.md` carve-out: a negated
    vendored subtree can legitimately re-track more than one filename
    (e.g. a whole committed .dist directory), and hand-listing each such
    exception name is the exact hand-maintained-list defect this section
    exists to remove.
    """
    result = run.run(
        ["git", "ls-files", "--", rel_path.as_posix()],
        cwd=repo_root,
    )
    if result.returncode != 0:
        return set()
    return {line.strip() for line in result.stdout.splitlines() if line.strip()}


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

        print(f"FRESH_RUNNER_GATE: SIMULATED_ABSENT sibling-checkout ({sibling_halcyon})")
        if sibling_halcyon.exists():
            print(
                f"FRESH_RUNNER_GATE: ABSENCE_CLAIM_FALSE -- {sibling_halcyon} exists, "
                "leg is not trustworthy",
                file=sys.stderr,
            )
            return 2
        print("FRESH_RUNNER_GATE: ABSENCE_CONFIRMED sibling-checkout")

        # FULL BAR (guard (e)): the vendored roots to assert absent are
        # DERIVED via git, not a hand-written list of two -- see
        # discover_vendored_roots()'s docstring for why and how. Discovery
        # runs against REPO_ROOT (the real machine's disk + tracked tree),
        # because the worktree -- being checkout-only -- can never itself
        # reveal what an untracked, gitignored fetch would have looked like.
        vendored_roots = discover_vendored_roots(REPO_ROOT)
        print(
            f"FRESH_RUNNER_GATE: VENDORED_ROOTS_ASSERTED_ABSENT count={len(vendored_roots)} "
            f"paths={[p.as_posix() for p in vendored_roots]}"
        )
        if not vendored_roots:
            print(
                "FRESH_RUNNER_GATE: ABSENCE_CLAIM_FALSE -- discover_vendored_roots() "
                "returned zero paths; a leg that asserts nothing is not trustworthy",
                file=sys.stderr,
            )
            return 2

        for rel_root in vendored_roots:
            target = worktree_path / rel_root
            tracked_ok = _tracked_files_under(REPO_ROOT, rel_root)
            print(f"FRESH_RUNNER_GATE: SIMULATED_ABSENT vendored-tree ({rel_root.as_posix()})")
            unexpected = None
            if target.exists():
                for p in target.rglob("*"):
                    if not p.is_file():
                        continue
                    rel_str = p.relative_to(worktree_path).as_posix()
                    if rel_str not in tracked_ok:
                        unexpected = p
                        break
            if unexpected is not None:
                print(
                    f"FRESH_RUNNER_GATE: ABSENCE_CLAIM_FALSE -- {unexpected} exists and is "
                    "not a tracked exception, leg is not trustworthy",
                    file=sys.stderr,
                )
                return 2
            print(
                f"FRESH_RUNNER_GATE: ABSENCE_CONFIRMED vendored-tree ({rel_root.as_posix()}, "
                f"{len(tracked_ok)} tracked exception file(s) allowed)"
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
        print(
            "FRESH_RUNNER_GATE: EXCLUDED_FROM_LEG note -- the sibling-checkout leg above "
            "only asserts absence of ../Halcyon by name, not any sibling directory generically"
        )
        print(
            "FRESH_RUNNER_GATE: EXCLUDED_FROM_LEG note -- OS/toolchain differences, GitHub "
            "Actions env vars/secrets, and network-fetch behaviour are not simulated (docker "
            "ubuntu:22.04 recipe in pyci-plan.md owns the OS/toolchain axis)"
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
