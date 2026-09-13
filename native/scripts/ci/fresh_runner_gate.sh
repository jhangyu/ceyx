#!/bin/sh
# fresh_runner_gate.sh -- WI-45 (USER RULING F): the local-gate leg for
# environment-dependent defects.
#
# WHY THIS EXISTS: five environment-dependency defects landed in one day
# (LibRaw provenance guard, the linkage table, a leader's own "spot-verified
# four of seven", the alias-table wiring, the expected-additions ledger
# validator) -- each one passed on a developer machine and failed on a
# fresh CI runner. The common shape, precisely: a path exists on the dev
# machine's disk but is ABSENT from `git ls-files` -- a sibling repo
# checkout (e.g. ../Halcyon) and a gitignored vendored third-party tree
# (e.g. native/third_party/libraw/, fetched by a build step that runs
# after the local guards) are the same defect wearing different clothes.
#
# The existing local gate recipe (docs/logs/2026-09-13/pyci-plan.md, never
# committed -- docs/ is never version-controlled in this repo) bind-mounts
# the LIVE host checkout into a container (`-v "$PWD":/w`). That mount
# carries every already-fetched, gitignored vendored tree and every
# sibling-adjacent fact a laptop happens to have -- none of which a fresh
# `actions/checkout` on a CI runner ever sees. This script closes that gap
# by running the same guard/selftest block against a `git worktree add`
# checkout instead of the live tree: a worktree contains ONLY git-tracked
# content (gitignored/untracked files never appear in it), so it is the
# closest mechanically-verifiable stand-in for a fresh runner checkout this
# repo can produce without a real CI run.
#
# WHAT THIS SIMULATES: absence of (1) any sibling directory next to the
# repo root (the ../Halcyon class -- WI-42's defect) and (2) any gitignored
# vendored/fetched tree inside the repo (the native/third_party/libraw/
# class -- WI-44's defect, still open on this tip; see the exclusion
# printed below).
#
# WHAT THIS DOES NOT SIMULATE (named, not silent): actual OS/toolchain
# differences (this always runs on the host OS, e.g. macOS here vs Linux
# on the `linux` legs -- the existing `docker run ubuntu:22.04` recipe in
# pyci-plan.md still owns that axis and is not superseded by this script),
# GitHub Actions env vars/secrets, and network-fetch behaviour. This is the
# MINIMAL version (USER RULING F): no configuration, no matrix, no
# per-platform variants -- one hardcoded, self-printing check list. The
# polished version is parking-lot guard (e).
#
# Run with: sh native/scripts/ci/fresh_runner_gate.sh   (from repo root)

set -eu

REPO_ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$REPO_ROOT"

TIP=$(git rev-parse HEAD)
WORKTREE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ceyx-fresh-runner-gate.XXXXXX")
# mktemp already created the directory; `git worktree add` refuses to
# checkout into an existing non-empty directory, and refuses an existing
# EMPTY one too (it wants to create the leaf itself) -- so hand it a path
# one level inside, under the mktemp-owned parent, which we still fully
# control for cleanup.
WORKTREE_PATH="$WORKTREE_DIR/checkout"

cleanup() {
    git worktree remove --force "$WORKTREE_PATH" >/dev/null 2>&1 || true
    rm -rf "$WORKTREE_DIR"
}
trap cleanup EXIT INT TERM

echo "FRESH_RUNNER_GATE: tip=$TIP date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "FRESH_RUNNER_GATE: creating worktree at $WORKTREE_PATH"
git worktree add --quiet --detach "$WORKTREE_PATH" "$TIP"

# ---- Declare what is simulated as absent -- mechanically verified, not
# assumed. A leg whose absence claim is wrong is worse than no leg.
SIBLING_HALCYON="$(dirname "$WORKTREE_PATH")/Halcyon"
LIBRAW_TREE="$WORKTREE_PATH/native/third_party/libraw"

echo "FRESH_RUNNER_GATE: SIMULATED_ABSENT sibling-checkout ($SIBLING_HALCYON)"
if [ -e "$SIBLING_HALCYON" ]; then
    echo "FRESH_RUNNER_GATE: ABSENCE_CLAIM_FALSE -- $SIBLING_HALCYON exists, leg is not trustworthy" >&2
    exit 2
fi
echo "FRESH_RUNNER_GATE: ABSENCE_CONFIRMED sibling-checkout"

# native/third_party/libraw/PROVENANCE.md is the one file this repo DOES
# track under that path (.gitignore:72-74) -- its presence is expected and
# does not represent the fetched vendor tree; anything else there does.
LIBRAW_UNEXPECTED=$(find "$LIBRAW_TREE" -type f ! -name PROVENANCE.md 2>/dev/null | head -1 || true)
echo "FRESH_RUNNER_GATE: SIMULATED_ABSENT vendored-tree ($LIBRAW_TREE, excluding tracked PROVENANCE.md)"
if [ -n "$LIBRAW_UNEXPECTED" ]; then
    echo "FRESH_RUNNER_GATE: ABSENCE_CLAIM_FALSE -- $LIBRAW_UNEXPECTED exists, leg is not trustworthy" >&2
    exit 2
fi
echo "FRESH_RUNNER_GATE: ABSENCE_CONFIRMED vendored-tree (only PROVENANCE.md, if anything, is tracked)"

# ---- Fixed, self-printing check list (minimal version: no discovery, no
# config -- see USER RULING F). Any check left out of this list is a
# silent-scope regression; add it here, do not special-case it below.
CHECKS="
native/scripts/check_workflow_bashisms.py
native/scripts/ci/check_no_test_execution_in_ci.py
native/scripts/ci/check_shell_prohibition.py
native/scripts/ci_conventions_check.py
native/scripts/ci/check_cmake_sources_tracked.py
native/scripts/ci/check_expected_additions.py
"
echo "FRESH_RUNNER_GATE: EXCLUDED_FROM_LEG none -- all 5 named local guards plus check_expected_additions.py run"
echo "FRESH_RUNNER_GATE: EXCLUDED_FROM_LEG note -- 'ci.py selftest' and 'pytest native/scripts/' are NOT run by"
echo "FRESH_RUNNER_GATE: EXCLUDED_FROM_LEG note -- this minimal leg (parking-lot polish item); this leg covers"
echo "FRESH_RUNNER_GATE: EXCLUDED_FROM_LEG note -- only the absence-sensitive local guards named above"

cd "$WORKTREE_PATH"
OVERALL_RC=0
for check in $CHECKS; do
    [ -n "$check" ] || continue
    echo "FRESH_RUNNER_GATE: RUN $check"
    python3 "$check"
    RC=$?
    echo "FRESH_RUNNER_GATE: RC($check)=$RC"
    if [ "$RC" -ne 0 ]; then
        OVERALL_RC=1
    fi
done

echo "FRESH_RUNNER_GATE: OVERALL_RC=$OVERALL_RC tip=$TIP"
exit "$OVERALL_RC"
