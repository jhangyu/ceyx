"""Unit tests for native/scripts/ci/check_argv_contract.py (guard (g),
command-path-count slice).

Layer 1 proves the checker can FAIL: `enumerate_command_paths` is patched to
return a mutated set (one path added, one removed) and the guard must
report both by name and exit non-zero. Layer 2 is the real-tree regression:
it loads the ACTUAL `ci.py` parser (no patching) and asserts the count is
37 -- this is the acceptance evidence for the inherited ruling
(`tmp/verify/lead16/SIGNOFF-LEDGER.md`: "ci.py has 36 registered command
paths, not 35"), so a synthetic-only test suite would not discharge it.
The inherited ruling's 36 is the BASELINE; Phase 1 of the four-phase CI
migration added the `guards` command (one path -- its `--docker`/
`--in-container` are flags, not sub-parsers), making the live total 37.
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
    def test_expected_count_is_37(self):
        # Deliberately LITERALS, not `cac.EXPECTED_COMMAND_COUNT` -- this is
        # double-entry bookkeeping. The guard declares the number; this test
        # pins it independently, so changing the CLI surface costs two
        # conscious edits in two files instead of one a reviewer skims past.
        # 36 baseline + Phase 1's `guards` command = 37.
        self.assertEqual(len(cac.EXPECTED_COMMAND_PATHS), 37)
        self.assertEqual(cac.EXPECTED_COMMAND_COUNT, 37)

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
        # Derived, not a literal: this count is "the frozen surface plus the
        # one path this test just injected", so it is a CONSEQUENCE of the
        # mutation rather than an independent ledger of its own. Pinning it
        # as a literal only guaranteed it would need editing every time the
        # real surface grew -- which is exactly what it did.
        self.assertIn(f"ARGV_CONTRACT_ACTUAL_COUNT={cac.EXPECTED_COMMAND_COUNT + 1}", out)
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
        # Derived for the same reason as the addition case above: the frozen
        # surface minus the one path this test just removed.
        self.assertIn(f"ARGV_CONTRACT_ACTUAL_COUNT={cac.EXPECTED_COMMAND_COUNT - 1}", out)
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
    ci.py parser and asserts the live tree still has exactly 37 registered
    command paths, matching the frozen surface exactly (no added, no
    removed). A synthetic-only suite would not discharge the acceptance
    criterion this guard exists for."""

    def test_real_ci_py_has_exactly_37_command_paths(self):
        ci_entrypoint = cac.load_ci_entrypoint()
        parser = ci_entrypoint.build_parser()
        paths = cac.enumerate_command_paths(parser)
        self.assertEqual(len(paths), 37)
        self.assertEqual(paths, cac.EXPECTED_COMMAND_PATHS)

    def test_main_against_the_real_tree_passes(self):
        rc, out, err = _run_captured(cac.main, [])
        self.assertEqual(rc, 0)
        self.assertIn("ARGV_CONTRACT_ACTUAL_COUNT=37", out)
        self.assertIn("ARGV_CONTRACT_RESULT=PASS", out)
        self.assertEqual(err, "")


if __name__ == "__main__":
    unittest.main()
