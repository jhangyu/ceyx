"""Tests for native/scripts/ci/fresh_runner_gate.py (parking-lot guard (e),
FULL BAR). Focused on `discover_vendored_roots()` and `_tracked_files_under()`
-- the pieces that replace the old two-path hardcode -- because `main()`
itself performs a real `git worktree add` in the shared tree and is exercised
manually (per WI-52's brief, coordinated through the lead) rather than in the
unit suite.
"""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

_MODULE_PATH = Path(__file__).resolve().parents[1] / "fresh_runner_gate.py"
_REPO_ROOT = Path(__file__).resolve().parents[4]

# fresh_runner_gate.py is deliberately NOT part of the `ci` package import
# graph (its own docstring/import comment explains why: it also runs against
# a git-worktree COPY of itself, where a relative `from . import run` would
# break). Load it the same way it loads itself -- by path -- rather than via
# `from .. import fresh_runner_gate`, which would silently depend on package
# machinery the module itself opts out of.
_spec = importlib.util.spec_from_file_location("fresh_runner_gate_under_test", _MODULE_PATH)
assert _spec is not None and _spec.loader is not None
fresh_runner_gate = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = fresh_runner_gate
_spec.loader.exec_module(fresh_runner_gate)


class DiscoverVendoredRootsTest(unittest.TestCase):
    """Runs against the REAL repo tree (read-only: no worktree, no writes) --
    the same discipline check_workflow_scan-style tests use when validating
    against real workflow YAML rather than fixtures, because the property
    under test (does `git check-ignore` agree with what THIS repo's
    .gitignore actually says today) cannot be faithfully faked without
    re-implementing the negation semantics the guard exists to avoid
    hand-rolling.
    """

    def test_libraw_is_asserted_absent(self) -> None:
        # WI-44's actual defect tree -- must never regress out of coverage.
        roots = fresh_runner_gate.discover_vendored_roots(_REPO_ROOT)
        names = {p.as_posix() for p in roots}
        self.assertIn("native/third_party/libraw", names)

    def test_halide_is_asserted_absent_even_if_never_fetched(self) -> None:
        # Seeded candidate: no PROVENANCE.md, no tracked placeholder -- this
        # is exactly the case a pure disk-scan/tree-scan union would miss on
        # a clean clone that has never run `build_deps.py fetch halide`.
        roots = fresh_runner_gate.discover_vendored_roots(_REPO_ROOT)
        names = {p.as_posix() for p in roots}
        self.assertIn("native/third_party/halide", names)

    def test_negated_tracked_directory_is_not_asserted_absent(self) -> None:
        # native/third_party/heif-dist-android-arm64-v8a/ is a whole
        # committed, negated exception (.gitignore's own comment: "reviewed,
        # pinned INPUT"). `git check-ignore` must say "not ignored" for it,
        # and discover_vendored_roots() must therefore NOT include it as a
        # root to assert absent -- asserting absence of tracked content
        # would fail-closed forever (the guard plan's DESIGN HAZARD).
        roots = fresh_runner_gate.discover_vendored_roots(_REPO_ROOT)
        names = {p.as_posix() for p in roots}
        self.assertNotIn(
            "native/third_party/heif-dist-android-arm64-v8a",
            names,
            "a fully-negated, tracked vendored exception must never be "
            "asserted absent -- it would fail closed on every run",
        )

    def test_dng_sdk_subtree_is_asserted_absent_but_root_is_not(self) -> None:
        # native/third_party/dng_sdk/ itself is tracked (source lives there);
        # only its targets/ (and documents/, projects/) subtrees are
        # ignored. discover_vendored_roots() must find the ignored CHILD
        # without asserting absence of the tracked PARENT.
        roots = fresh_runner_gate.discover_vendored_roots(_REPO_ROOT)
        names = {p.as_posix() for p in roots}
        self.assertNotIn(
            "native/third_party/dng_sdk",
            names,
            "dng_sdk/ itself is tracked source, not a vendored root",
        )
        # The specific ignored child present in .gitignore today:
        # native/third_party/dng_sdk/targets/. Only assert it if the parent
        # actually exists on this machine (tree-scan discovers it either way
        # via `git ls-tree`, since dng_sdk/ is tracked, so this should hold
        # on every machine, not just ones that have built it).
        self.assertIn("native/third_party/dng_sdk/targets", names)

    def test_returns_pathlib_paths_under_native_third_party(self) -> None:
        roots = fresh_runner_gate.discover_vendored_roots(_REPO_ROOT)
        self.assertTrue(roots, "must discover at least one vendored root in this repo")
        for root in roots:
            self.assertTrue(root.as_posix().startswith("native/third_party/"))


class TrackedFilesUnderTest(unittest.TestCase):
    def test_libraw_provenance_is_the_only_tracked_file(self) -> None:
        tracked = fresh_runner_gate._tracked_files_under(
            _REPO_ROOT, Path("native/third_party/libraw")
        )
        self.assertEqual(tracked, {"native/third_party/libraw/PROVENANCE.md"})

    def test_nonexistent_path_returns_empty_set(self) -> None:
        tracked = fresh_runner_gate._tracked_files_under(
            _REPO_ROOT, Path("native/third_party/this-does-not-exist-anywhere")
        )
        self.assertEqual(tracked, set())


if __name__ == "__main__":
    unittest.main()
