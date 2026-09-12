"""Tests for native/scripts/ci/check_shell_prohibition.py (WI-4).

Every RED case below builds an ISOLATED temp directory tree (never touches
this repo's real `.github/workflows/`) and calls `check_shell_prohibition`
functions directly against it -- there is no `git checkout --` and no
mutation of any shared file, per the shared-tree red line.
"""
from __future__ import annotations

import io
import shutil
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path
from unittest import mock

# Relative imports -- see check_shell_prohibition.py's import comment /
# tmp/verify/pyci-push1-gate-VERDICT.md for why absolute `import ci.x` is
# identity-order-dependent and must not be used under ci/tests/.
from .. import check_shell_prohibition as guard
from .. import allowlist


ONE_LINE_WORKFLOW = """\
jobs:
  build:
    steps:
      - name: One-liner
        run: python3 native/scripts/ci.py foo
"""

THREE_LINE_BASH_WORKFLOW = """\
jobs:
  build:
    steps:
      - name: Multi-line shell step
        run: |
          echo "one"
          echo "two"
          echo "three"
"""


class _IsolatedRepo:
    """Builds a throwaway directory with just a `.github/workflows/` tree,
    entirely outside this real repo, restored (deleted) on exit.
    """

    def __enter__(self):
        self.root = Path(tempfile.mkdtemp(prefix="pyci-wi4-fixture-"))
        (self.root / ".github" / "workflows").mkdir(parents=True)
        return self.root

    def __exit__(self, *exc):
        shutil.rmtree(self.root, ignore_errors=True)
        return False

    def write_workflow(self, name: str, text: str):
        (self.root / ".github" / "workflows" / name).write_text(text)


class TestOneLinePython3BodyPasses(unittest.TestCase):
    def test_one_line_python3_body_passes_with_no_allowlist_entry(self):
        # Isolated fixture repo -- the REAL MUST_STAY (89+ entries) names
        # steps that don't exist in this tiny fixture tree, which would
        # otherwise trip Rule 3's [stale-allowlist] check spuriously. Empty
        # the allowlist for the duration of this specific green-path test;
        # Rule 3 itself is exercised against an intentionally-stale entry by
        # TestStaleAllowlistEntryFails below, and against the REAL allowlist
        # by TestGuardMatchesRealRepo.
        with _IsolatedRepo() as root:
            fx = _IsolatedRepo()
            fx.root = root
            fx.write_workflow("x.yml", ONE_LINE_WORKFLOW)
            with mock.patch.object(allowlist, "MUST_STAY", ()), mock.patch.object(
                allowlist, "ALLOWLIST_SIZE_EXPECTED", 0
            ):
                buf = io.StringIO()
                with redirect_stdout(buf):
                    rc = guard.main(repo_root=root)
            self.assertEqual(rc, 0)
            self.assertIn("SHELL_PROHIBITION_RESULT=PASS", buf.getvalue())


class TestPlantedBashBlockFails(unittest.TestCase):
    """RED CASE A."""

    def test_planted_multiline_bash_block_fails(self):
        with _IsolatedRepo() as root:
            fx = _IsolatedRepo()
            fx.root = root
            fx.write_workflow("bad.yml", THREE_LINE_BASH_WORKFLOW)
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                rc = guard.main(repo_root=root)
            self.assertEqual(rc, 1)
            combined = out.getvalue() + err.getvalue()
            self.assertIn("[shell-body]", combined)
            self.assertIn("Multi-line shell step", combined)


class TestPlantedShFileFails(unittest.TestCase):
    """RED CASE B."""

    def test_planted_sh_file_fails(self):
        with _IsolatedRepo() as root:
            fx = _IsolatedRepo()
            fx.root = root
            fx.write_workflow("x.yml", ONE_LINE_WORKFLOW)
            ci_dir = root / "native" / "scripts" / "ci"
            ci_dir.mkdir(parents=True)
            (ci_dir / "helper.sh").write_text("#!/bin/sh\necho hi\n")
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                rc = guard.main(repo_root=root)
            self.assertEqual(rc, 1)
            combined = out.getvalue() + err.getvalue()
            self.assertIn("[new-shell-file]", combined)
            self.assertIn("helper.sh", combined)


class TestStaleAllowlistEntryFails(unittest.TestCase):
    """RED CASE C: an allowlist entry naming a step that does not exist in
    any workflow is itself a failure -- the ratchet's own safety catch.
    """

    def test_allowlist_entry_for_missing_step_fails(self):
        fake_entry = allowlist.Entry(
            "nonexistent.yml", "A step that does not exist", "test fixture"
        )
        with _IsolatedRepo() as root:
            fx = _IsolatedRepo()
            fx.root = root
            fx.write_workflow("x.yml", ONE_LINE_WORKFLOW)
            with mock.patch.object(allowlist, "MUST_STAY", (fake_entry,)), mock.patch.object(
                allowlist, "ALLOWLIST_SIZE_EXPECTED", 1
            ):
                out, err = io.StringIO(), io.StringIO()
                with redirect_stdout(out), redirect_stderr(err):
                    rc = guard.main(repo_root=root)
            self.assertEqual(rc, 1)
            combined = out.getvalue() + err.getvalue()
            self.assertIn("[stale-allowlist]", combined)


class TestAllowlistSizeIsPinned(unittest.TestCase):
    def test_allowlist_size_is_pinned(self):
        self.assertEqual(len(allowlist.MUST_STAY), allowlist.ALLOWLIST_SIZE_EXPECTED)


class TestGuardMatchesRealRepo(unittest.TestCase):
    """Confirms the guard passes against the ACTUAL repo tree, and that its
    own source and the allowlist's own text do not trip the checks they
    define (the self-defeating-instrument trap from WI-1's notes)."""

    def test_guard_passes_against_the_real_repo(self):
        real_repo_root = guard.REPO_ROOT
        out = io.StringIO()
        with redirect_stdout(out):
            rc = guard.main(repo_root=real_repo_root)
        self.assertEqual(rc, 0, out.getvalue())
        self.assertIn("SHELL_PROHIBITION_RESULT=PASS", out.getvalue())

    def test_guard_source_itself_is_not_flagged_as_a_new_shell_file(self):
        # check_shell_prohibition.py is a .py file, not one of the four
        # prohibited extensions, so this is really asserting the glob
        # patterns are extension-exact, not "contains .sh".
        for pattern in guard._SHELL_GLOB_PATTERNS:
            self.assertFalse(str(guard.__file__).endswith(pattern.lstrip("*")))


if __name__ == "__main__":
    unittest.main()
