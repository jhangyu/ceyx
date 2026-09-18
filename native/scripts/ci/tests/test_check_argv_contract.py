"""Unit tests for native/scripts/ci/check_argv_contract.py (guard (g),
command-path-count slice).

Layer 1 proves the checker can FAIL: `enumerate_command_paths` is patched to
return a mutated set (one path added, one removed) and the guard must
report both by name and exit non-zero. Layer 2 is the real-tree regression:
it loads the ACTUAL `ci.py` parser (no patching) and asserts the count is
36 -- this is the acceptance evidence for the inherited ruling
(`tmp/verify/lead16/SIGNOFF-LEDGER.md`: "ci.py has 36 registered command
paths, not 35"), so a synthetic-only test suite would not discharge it.
"""

from __future__ import annotations

import io
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

from .. import check_argv_contract as cac


def _run_captured(func, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = func(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


class ExpectedSetShapeTests(unittest.TestCase):
    def test_expected_count_is_36(self):
        self.assertEqual(len(cac.EXPECTED_COMMAND_PATHS), 36)
        self.assertEqual(cac.EXPECTED_COMMAND_COUNT, 36)

    def test_expected_paths_are_unique_tuples(self):
        # frozenset already enforces uniqueness; this asserts the shape
        # (every element is a non-empty tuple of str) rather than trusting it.
        for path in cac.EXPECTED_COMMAND_PATHS:
            self.assertIsInstance(path, tuple)
            self.assertTrue(path)
            for part in path:
                self.assertIsInstance(part, str)


class DecisionLogicTests(unittest.TestCase):
    """Hermetic: `enumerate_command_paths` is patched, so these test the
    VERDICT, not the real parser tree."""

    def test_added_command_is_reported_and_fails(self):
        mutated = set(cac.EXPECTED_COMMAND_PATHS) | {("bogus-new-command",)}
        dummy = mock.Mock()
        with mock.patch.object(cac, "load_ci_entrypoint", return_value=dummy), mock.patch.object(
            cac, "enumerate_command_paths", return_value=mutated
        ):
            rc, out, err = _run_captured(cac.main, [])
        self.assertEqual(rc, 1)
        self.assertIn("ARGV_CONTRACT_RESULT=FAIL", out)
        self.assertIn("ARGV_CONTRACT_ACTUAL_COUNT=37", out)
        self.assertIn("'bogus-new-command'", err)

    def test_removed_command_is_reported_and_fails(self):
        mutated = set(cac.EXPECTED_COMMAND_PATHS)
        mutated.discard(("selftest",))
        dummy = mock.Mock()
        with mock.patch.object(cac, "load_ci_entrypoint", return_value=dummy), mock.patch.object(
            cac, "enumerate_command_paths", return_value=mutated
        ):
            rc, out, err = _run_captured(cac.main, [])
        self.assertEqual(rc, 1)
        self.assertIn("ARGV_CONTRACT_RESULT=FAIL", out)
        self.assertIn("ARGV_CONTRACT_ACTUAL_COUNT=35", out)
        self.assertIn("'selftest'", err)

    def test_matching_set_passes(self):
        dummy = mock.Mock()
        with mock.patch.object(cac, "load_ci_entrypoint", return_value=dummy), mock.patch.object(
            cac, "enumerate_command_paths", return_value=set(cac.EXPECTED_COMMAND_PATHS)
        ):
            rc, out, err = _run_captured(cac.main, [])
        self.assertEqual(rc, 0)
        self.assertIn("ARGV_CONTRACT_RESULT=PASS", out)
        self.assertEqual(err, "")


class RealParserTreeTests(unittest.TestCase):
    """Real-history-equivalent regression: NOT patched. Loads the actual
    ci.py parser and asserts the live tree still has exactly 36 registered
    command paths, matching the frozen surface exactly (no added, no
    removed). A synthetic-only suite would not discharge the acceptance
    criterion this guard exists for."""

    def test_real_ci_py_has_exactly_36_command_paths(self):
        ci_entrypoint = cac.load_ci_entrypoint()
        parser = ci_entrypoint.build_parser()
        paths = cac.enumerate_command_paths(parser)
        self.assertEqual(len(paths), 36)
        self.assertEqual(paths, cac.EXPECTED_COMMAND_PATHS)

    def test_main_against_the_real_tree_passes(self):
        rc, out, err = _run_captured(cac.main, [])
        self.assertEqual(rc, 0)
        self.assertIn("ARGV_CONTRACT_ACTUAL_COUNT=36", out)
        self.assertIn("ARGV_CONTRACT_RESULT=PASS", out)
        self.assertEqual(err, "")


if __name__ == "__main__":
    unittest.main()
