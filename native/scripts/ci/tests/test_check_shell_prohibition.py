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
from .. import run as ci_run


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


class TestObsoleteAllowlistEntries(unittest.TestCase):
    """WI-4b, Rule 4 (`[obsolete-allowlist]`): the other half of the
    stale-allowlist safety catch. A migrated step keeps its exact
    `- name:` and only its BODY changes (shell -> one-line `python3`), so
    Rule 3's `[stale-allowlist]` (which fires only when the step no longer
    EXISTS) stays silent while the allowlist keeps a dead exemption."""

    def test_obsolete_entry_is_flagged(self):
        """A stubbed allowlist entry naming a step whose body is ALREADY a
        compliant one-line python3 call must fail with [obsolete-allowlist]."""
        fake_entry = allowlist.Entry("x.yml", "One-liner", "test fixture")
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
            self.assertIn("[obsolete-allowlist]", combined)
            self.assertIn("One-liner", combined)

    def test_genuine_shell_entry_is_not_flagged(self):
        """FALSE-POSITIVE PIN: the same allowlisted step name, but with a
        genuine multi-line shell body, must NOT be flagged -- without this,
        [obsolete-allowlist] could "pass" by flagging every entry and the
        next real ratchet would be unreadable."""
        fake_entry = allowlist.Entry("bad.yml", "Multi-line shell step", "test fixture")
        with _IsolatedRepo() as root:
            fx = _IsolatedRepo()
            fx.root = root
            fx.write_workflow("bad.yml", THREE_LINE_BASH_WORKFLOW)
            with mock.patch.object(allowlist, "MUST_STAY", (fake_entry,)), mock.patch.object(
                allowlist, "ALLOWLIST_SIZE_EXPECTED", 1
            ):
                out, err = io.StringIO(), io.StringIO()
                with redirect_stdout(out), redirect_stderr(err):
                    rc = guard.main(repo_root=root)
            self.assertEqual(rc, 0, out.getvalue() + err.getvalue())
            self.assertNotIn("[obsolete-allowlist]", out.getvalue() + err.getvalue())

    def test_stale_and_obsolete_are_distinct(self):
        """A step that no longer exists at all must yield [stale-allowlist],
        never [obsolete-allowlist] -- the two rules must not mask each
        other."""
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
            self.assertNotIn("[obsolete-allowlist]", combined)

    def test_must_stay_entries_are_never_obsolete(self):
        """The C-G14 permanent entries (and every other real MUST_STAY
        entry) must not trip Rule 4 against the real repo: they are
        genuinely multi-line shell, so this pins that Rule 4 agrees with
        Rule 1's own classification of the same bodies."""
        real_repo_root = guard.REPO_ROOT
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = guard.main(repo_root=real_repo_root)
        combined = out.getvalue() + err.getvalue()
        self.assertNotIn(
            "[obsolete-allowlist]", combined,
            "a real allowlist entry is stranded -- ratchet it, do not weaken this test",
        )
        self.assertEqual(rc, 0, combined)


DASH_C_ONE_LINER_WORKFLOW = """\
jobs:
  build:
    steps:
      - name: Inline dash-c
        run: python3 -c "print('x')"
"""

DASH_M_ONE_LINER_WORKFLOW = """\
jobs:
  build:
    steps:
      - name: Inline dash-m
        run: python3 -m json.tool --help
"""


class TestGuardDInlineDashC(unittest.TestCase):
    """Guard (d): an inline `python3 -c "..."` one-liner matches
    PYTHON_BODY_RE's interpreter prefix but its code is on the command line,
    not in any file this repo's guards scan -- must be rejected by Rule 1
    even though `len(code) == 1`. Scope: `-c` ONLY (SIGNOFF-LEDGER inherited
    ruling "(d) flags -c only NOT -m") -- `-m` is a separate case pinned
    NOT-flagged below."""

    def test_inline_dash_c_one_liner_fails_shell_body(self):
        with _IsolatedRepo() as root:
            fx = _IsolatedRepo()
            fx.root = root
            fx.write_workflow("x.yml", DASH_C_ONE_LINER_WORKFLOW)
            with mock.patch.object(allowlist, "MUST_STAY", ()), mock.patch.object(
                allowlist, "ALLOWLIST_SIZE_EXPECTED", 0
            ):
                out, err = io.StringIO(), io.StringIO()
                with redirect_stdout(out), redirect_stderr(err):
                    rc = guard.main(repo_root=root)
            self.assertEqual(rc, 1)
            combined = out.getvalue() + err.getvalue()
            self.assertIn("[shell-body]", combined)
            self.assertIn("Inline dash-c", combined)

    def test_inline_dash_m_one_liner_still_passes(self):
        """Negative control for the ruling's scope: `-m` must NOT be
        flagged. If this ever fails, the predicate has drifted from the
        binding ruling and must be narrowed back to `-c` only."""
        with _IsolatedRepo() as root:
            fx = _IsolatedRepo()
            fx.root = root
            fx.write_workflow("x.yml", DASH_M_ONE_LINER_WORKFLOW)
            with mock.patch.object(allowlist, "MUST_STAY", ()), mock.patch.object(
                allowlist, "ALLOWLIST_SIZE_EXPECTED", 0
            ):
                buf = io.StringIO()
                with redirect_stdout(buf):
                    rc = guard.main(repo_root=root)
            self.assertEqual(rc, 0, buf.getvalue())
            self.assertIn("SHELL_PROHIBITION_RESULT=PASS", buf.getvalue())

    def test_real_file_invocation_one_liner_still_passes(self):
        """Negative control: a real one-line file invocation (the existing
        ONE_LINE_WORKFLOW shape) must remain compliant -- proves the new
        predicate discriminates rather than rejecting every python call."""
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
            self.assertEqual(rc, 0, buf.getvalue())
            self.assertIn("SHELL_PROHIBITION_RESULT=PASS", buf.getvalue())

    def test_shared_predicate_both_sites_agree_on_dash_c(self):
        """`:168`/`:240` coupling (the hazard this batch fixes): construct an
        allowlisted entry whose real body is `python3 -c "..."`. Under the
        unified predicate, Rule 1 (correctly) requires the exemption AND
        `check_obsolete_entries` must NOT call it dead -- both sites reading
        the SAME function is what keeps this from silently diverging again."""
        fake_entry = allowlist.Entry("x.yml", "Inline dash-c", "test fixture")
        with _IsolatedRepo() as root:
            fx = _IsolatedRepo()
            fx.root = root
            fx.write_workflow("x.yml", DASH_C_ONE_LINER_WORKFLOW)
            with mock.patch.object(allowlist, "MUST_STAY", (fake_entry,)), mock.patch.object(
                allowlist, "ALLOWLIST_SIZE_EXPECTED", 1
            ):
                out, err = io.StringIO(), io.StringIO()
                with redirect_stdout(out), redirect_stderr(err):
                    rc = guard.main(repo_root=root)
            combined = out.getvalue() + err.getvalue()
            self.assertNotIn("[obsolete-allowlist]", combined)
            self.assertNotIn("[shell-body]", combined)
            self.assertEqual(rc, 0, combined)

    def test_shared_predicate_real_file_invocation_still_flagged_obsolete(self):
        """Unrelated-to-(d) behaviour that must not regress: an allowlisted
        entry whose body IS a real compliant file invocation is still a
        genuinely dead exemption and must still be flagged."""
        fake_entry = allowlist.Entry("x.yml", "One-liner", "test fixture")
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
            self.assertIn("[obsolete-allowlist]", combined)


class TestBareScriptInvocation(unittest.TestCase):
    """Round-2 signoff blocker: every prior test invoked the guard by
    IMPORTING it, so a defect that only manifests when the module is run as
    a bare script (no parent package -- relative imports hard-fail) was
    invisible to all 73 of them. `.github/workflows/build.yml` invokes it
    exactly this way:

        run: python3 native/scripts/ci/check_shell_prohibition.py

    This test shells out to that EXACT command via `ci.run.run()` (the
    sanctioned subprocess path, list argv, shell=False) and would fail if
    the dual-mode import guard (`if not __package__: ... else: ...` at the
    top of check_shell_prohibition.py) were ever removed or broken again.
    """

    def test_bare_script_invocation_matches_ci_exactly(self):
        script = guard.REPO_ROOT / "native" / "scripts" / "ci" / "check_shell_prohibition.py"
        result = ci_run.run(["python3", str(script)], cwd=guard.REPO_ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SHELL_PROHIBITION_RESULT=PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
