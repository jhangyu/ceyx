"""Unit tests for native/scripts/ci/check_wiring_is_ledger.py (guard (f)②).

TWO LAYERS, and the distinction is deliberate:
  1. Decision-logic tests with the two git-reading functions patched out --
     fast, hermetic, and they prove the checker can still FAIL.
  2. REAL-HISTORY regression tests that replay WI-26's actual wiring commit.
     Those are the acceptance evidence: (f)② is a claim about COMMITS, so a
     unit test over a synthetic workflow does not discharge it (planner6
     §(f)3). They are kept here so the replay cannot silently stop working.

The real-history tests DECLARE A SKIP (never a silent one) when the commits
are unreachable -- e.g. a shallow CI checkout. A silent skip there would be
the exact false-green this guard exists to reject: the guard needs
`fetch-depth: 0`, and if it does not have it, the tests must SAY so rather
than quietly report green.
"""

from __future__ import annotations

import io
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

from .. import check_wiring_is_ledger as cwil

# The real defect this guard was built from (see the module docstring):
# WI-26 wired the alias-table step; WI-43 added the ledger entries later.
_WI26_WIRING = "b4e44b84"
_WI43_LEDGER = "bba2a752"
_ALIAS_KEYS = {"TABLE_COUNT", "ALIAS_TABLE_FIRST_ELEMENT_ALL_AT"}


def _run_captured(func, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = func(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


def _history_available(*revs: str) -> bool:
    try:
        for rev in revs:
            cwil._git("cat-file", "-e", f"{rev}^{{commit}}")
        return True
    except cwil.GitReadError:
        return False


_ALIAS_STEP = ("build.yml", "Guard - alias table")
_ALIAS_COMMANDS = [
    "python3 native/scripts/check_alias_table_convention.py "
    "native/third_party/libraw/src/metadata/normalize_model.cpp"
]


class DecisionLogicTests(unittest.TestCase):
    """Hermetic: the git-reading functions are patched, so these test the
    VERDICT, not the plumbing."""

    def _run(self, base_steps, head_steps, ledger):
        with mock.patch.object(cwil, "_steps_at_rev", side_effect=[base_steps, head_steps]), \
             mock.patch.object(cwil, "ledger_keys_at_rev", return_value=ledger):
            return _run_captured(cwil.main, ["--base", "B", "--head", "H"])

    def test_new_marker_step_without_ledger_entry_is_rejected(self):
        """THE DEFECT: a step that prints is newly wired, the ledger did not
        move. This is the one that must be able to fail."""
        rc, out, err = self._run({}, {_ALIAS_STEP: _ALIAS_COMMANDS}, set())
        self.assertEqual(rc, 1)
        self.assertIn("WIRING_LEDGER_RESULT=FAIL", out)
        self.assertIn("TABLE_COUNT", err)
        self.assertIn("ALIAS_TABLE_FIRST_ELEMENT_ALL_AT", err)

    def test_new_marker_step_with_ledger_entry_is_accepted(self):
        """The discriminating half: SAME newly-wired step, ledger caught up.
        Without this, a checker that rejected every new step would pass the
        test above and be worthless."""
        rc, out, _ = self._run({}, {_ALIAS_STEP: _ALIAS_COMMANDS}, set(_ALIAS_KEYS))
        self.assertEqual(rc, 0)
        self.assertIn("WIRING_LEDGER_RESULT=PASS", out)
        self.assertIn("NEW_MARKER_EMITTING_STEPS=1", out)

    def test_partial_ledger_is_still_rejected_and_names_only_the_gap(self):
        rc, out, err = self._run({}, {_ALIAS_STEP: _ALIAS_COMMANDS}, {"TABLE_COUNT"})
        self.assertEqual(rc, 1)
        self.assertIn("ALIAS_TABLE_FIRST_ELEMENT_ALL_AT", err)

    def test_preexisting_step_is_not_flagged(self):
        """Only NEWLY wired steps are in scope -- an unchanged step with no
        ledger entry is P-24's territory, not a finding here."""
        steps = {_ALIAS_STEP: _ALIAS_COMMANDS}
        rc, out, _ = self._run(steps, steps, set())
        self.assertEqual(rc, 0)
        self.assertIn("NEW_STEPS_EXAMINED=0", out)

    def test_new_step_that_emits_nothing_is_not_flagged(self):
        rc, out, _ = self._run({}, {("build.yml", "Echo"): ["echo hello"]}, set())
        self.assertEqual(rc, 0)
        self.assertIn("NEW_STEPS_EXAMINED=1", out)
        self.assertIn("NEW_MARKER_EMITTING_STEPS=0", out)

    def test_counts_and_p24_notice_print_on_the_PASS_path_too(self):
        """A human reading a GREEN log must be able to see both that the
        guard examined something and that it does not cover P-24. A
        limitation visible only on failure is an unprinted limitation."""
        rc, out, _ = self._run({}, {_ALIAS_STEP: _ALIAS_COMMANDS}, set(_ALIAS_KEYS))
        self.assertEqual(rc, 0)
        self.assertIn("P_24_NOT_COVERED", out)
        self.assertIn("NEW_STEPS_EXAMINED=", out)
        self.assertIn("NEW_MARKER_EMITTING_STEPS=", out)


class GitPrimitiveTests(unittest.TestCase):
    """WI-53 follow-up: `_git` moved from `subprocess.run(check=True)` to the
    audited `run` primitive, which NEVER RAISES -- it returns a returncode.
    The `check=True` semantics callers depend on are now re-created by hand,
    so they are tested by hand. An untested failure path here would turn
    "this rev does not exist" into "this rev is empty", which reads as a
    clean PASS -- the precise false-green this guard exists to reject."""

    def test_failed_git_query_raises_rather_than_returning_empty(self):
        with self.assertRaises(cwil.GitReadError):
            cwil._git("cat-file", "-e", "definitely-not-a-rev^{commit}")

    def test_missing_path_at_rev_is_absence_not_an_error(self):
        """`_file_at_rev` must convert that raise into None -- absence."""
        self.assertIsNone(cwil._file_at_rev("HEAD", "no/such/file/here.py"))

    def test_missing_workflow_dir_at_rev_yields_no_steps(self):
        self.assertEqual(cwil._workflow_names_at_rev("definitely-not-a-rev"), [])

    def test_successful_git_query_returns_stdout(self):
        self.assertIn("check_wiring_is_ledger", cwil._git("ls-tree", "--name-only", "HEAD:native/scripts/ci"))


class RevisionResolutionTests(unittest.TestCase):
    """FAIL CLOSED when a revision does not resolve.

    Measured defect (lead17, `tmp/verify/lead17/b31-shallow-red-proof-
    ADJUDICATION.md`): with `refs/remotes/origin/main` deleted,
    `NEW_STEPS_EXAMINED` read 152 instead of 0 -- and RC was 0 with
    `WIRING_LEDGER_RESULT=PASS` in BOTH cases. The exit code could not
    distinguish a real diff from an evaporated baseline.

    The tests below pin BOTH halves of the distinction this fix rests on.
    An unresolvable REVISION is fatal; a missing PATH at a valid revision
    stays absence, because a guard that fired on every newly added file
    would be disabled by the first person to meet it -- the same end state
    as the vacuous pass, reached from the other side.
    """

    _NO_SUCH_REV = "definitely-not-a-rev"

    def test_unresolvable_base_exits_nonzero_instead_of_passing_vacuously(self):
        rc, out, err = _run_captured(
            cwil.main, ["--base", self._NO_SUCH_REV, "--head", "HEAD"]
        )
        self.assertEqual(rc, 2)
        self.assertIn("WIRING_LEDGER_RESULT=ERROR", out)
        self.assertNotIn("WIRING_LEDGER_RESULT=PASS", out)
        self.assertIn(self._NO_SUCH_REV, err)

    def test_unresolvable_base_examines_nothing_rather_than_everything(self):
        """The old failure did not merely pass -- it passed having silently
        treated all 152 wired steps as new. No count may be reported from a
        range whose baseline was never found."""
        _rc, out, _err = _run_captured(
            cwil.main, ["--base", self._NO_SUCH_REV, "--head", "HEAD"]
        )
        self.assertNotIn("NEW_STEPS_EXAMINED=", out)

    def test_unresolvable_head_exits_nonzero(self):
        rc, out, _err = _run_captured(
            cwil.main, ["--base", "HEAD", "--head", self._NO_SUCH_REV]
        )
        self.assertEqual(rc, 2)
        self.assertIn("WIRING_LEDGER_RESULT=ERROR", out)

    def test_unresolvable_revision_is_NOT_a_GitReadError_subclass(self):
        """STRUCTURAL, and the reason the fix holds. `_workflow_names_at_rev`
        still does `except GitReadError: return []`. If a later tidy-up made
        `UnresolvableRevision` inherit from `GitReadError`, that handler
        would swallow it and restore the exact vacuous PASS this task
        removed -- with every test above still green, because main() would
        never see the exception. This is the only test that notices."""
        self.assertFalse(issubclass(cwil.UnresolvableRevision, cwil.GitReadError))

    def test_steps_at_rev_raises_rather_than_returning_an_empty_dict(self):
        with self.assertRaises(cwil.UnresolvableRevision):
            cwil._steps_at_rev(self._NO_SUCH_REV)

    def test_resolve_rev_returns_a_commit_id_for_a_real_rev(self):
        resolved = cwil._resolve_rev("HEAD")
        self.assertRegex(resolved, r"^[0-9a-f]{40}$")

    def test_a_path_absent_at_a_VALID_rev_is_still_absence_not_fatal(self):
        """The other half of the distinction: a genuinely new file must not
        make this guard fatal, or it fires on every workflow addition."""
        self.assertIsNone(cwil._file_at_rev("HEAD", "no/such/file/here.py"))
        # ...and the surrounding read still succeeds at that same rev, so the
        # absence was handled rather than merely not raising.
        self.assertTrue(cwil._workflow_names_at_rev("HEAD"))

    def test_both_revs_resolvable_still_produces_an_ordinary_verdict(self):
        """Guards against the fix converting a working guard into a noisy
        one: a valid range must still reach a normal PASS/FAIL path."""
        rc, out, _err = _run_captured(cwil.main, ["--base", "HEAD", "--head", "HEAD"])
        self.assertEqual(rc, 0)
        self.assertIn("WIRING_LEDGER_RESULT=PASS", out)
        self.assertIn("NEW_STEPS_EXAMINED=0", out)


class RealHistoryReplayTests(unittest.TestCase):
    """The acceptance evidence: (f)② is a claim about commits."""

    def setUp(self):
        if not _history_available(_WI26_WIRING, _WI43_LEDGER):
            self.skipTest(
                f"DECLARED SKIP: commits {_WI26_WIRING}/{_WI43_LEDGER} are unreachable "
                "(shallow checkout?). Guard (f)② requires fetch-depth: 0; this replay "
                "cannot run without real history and is NOT silently passing."
            )

    def test_wi26_wiring_commit_is_rejected(self):
        rc, out, err = _run_captured(
            cwil.main, ["--base", f"{_WI26_WIRING}^", "--head", _WI26_WIRING]
        )
        self.assertEqual(rc, 1, "WI-26's real un-ledgered wiring commit must be rejected")
        self.assertIn("WIRING_LEDGER_RESULT=FAIL", out)
        for key in _ALIAS_KEYS:
            self.assertIn(key, err)

    def test_same_wiring_with_wi43_ledger_is_accepted(self):
        """Same range start, same new step -- only the ledger differs. This
        is what makes the rejection above discriminating rather than
        indiscriminate."""
        rc, out, _ = _run_captured(
            cwil.main, ["--base", f"{_WI26_WIRING}^", "--head", _WI43_LEDGER]
        )
        self.assertEqual(rc, 0)
        self.assertIn("WIRING_LEDGER_RESULT=PASS", out)
        self.assertIn("NEW_MARKER_EMITTING_STEPS=1", out)

    def test_ledger_is_read_at_the_range_head_not_the_working_tree(self):
        """If this read the working tree's ledger, today's 4 entries would
        vouch for WI-26 and the replay above would pass spuriously."""
        self.assertEqual(cwil.ledger_keys_at_rev(_WI26_WIRING) & _ALIAS_KEYS, set())
        self.assertEqual(cwil.ledger_keys_at_rev(_WI43_LEDGER) & _ALIAS_KEYS, _ALIAS_KEYS)


if __name__ == "__main__":
    unittest.main()
