"""Unit tests for native/scripts/ci/check_test_marker_leak.py (guard (a)).

Red-then-green discipline: `test_matches_red_case_line_shape` asserts
against the LITERAL leaked line from the recorded defect (commit
`81ee2889`, `T=0`, fixed at `a7840ef6`) so a future edit to
`_VALUE_MARKER_RE` cannot silently stop catching the exact shape this
guard was built for. The live reproduction against the real commits (via
`git worktree`, both before and after this module switched from a
subprocess-based to an in-process `run_selftest()` mechanism) lives
outside unit tests (slow) -- see
`tmp/verify/impl-a-marker-leak/wi50-red-green-evidence.txt`.
"""

from __future__ import annotations

import io
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

from .. import check_test_marker_leak as ctml


class FindLeakedMarkersTests(unittest.TestCase):
    def test_matches_red_case_line_shape(self):
        # The literal leaked line from commit 81ee2889 (push-8b round 1,
        # AC-2 MARKER_DIFF_DELTAS=1 failure).
        text = "some unittest output\nT=0\nmore output\n"
        leaks = ctml.find_leaked_markers(text)
        self.assertEqual(leaks, [(2, "T=0")])

    def test_matches_bare_rc_line(self):
        text = "RC=1\n"
        leaks = ctml.find_leaked_markers(text)
        self.assertEqual(leaks, [(1, "RC=1")])

    def test_does_not_flag_error_annotation(self):
        # ::error:: lines are expected, pre-existing selftest output (see
        # a7840ef6's commit message: 15 pre-existing ::error:: lines from
        # unrelated parked tests) -- not this guard's concern.
        text = "::error::boom\n"
        self.assertEqual(ctml.find_leaked_markers(text), [])

    def test_does_not_flag_notice_annotation(self):
        text = "::notice::info\n"
        self.assertEqual(ctml.find_leaked_markers(text), [])

    def test_does_not_flag_section_banner(self):
        text = "== some section ==\n"
        self.assertEqual(ctml.find_leaked_markers(text), [])

    def test_does_not_flag_ci_selftest_summary_line(self):
        # `CI-SELFTEST: tests=... failures=... errors=...` has a hyphen and
        # a colon, not a bare `=` after an ALL_CAPS token -- must never
        # false-positive on the checker's own summary line.
        text = "CI-SELFTEST: tests=372 failures=0 errors=0\n"
        self.assertEqual(ctml.find_leaked_markers(text), [])

    def test_does_not_flag_lowercase_or_mixed_case_assignment(self):
        text = "some_var=1\nMixedCase=2\n"
        self.assertEqual(ctml.find_leaked_markers(text), [])

    def test_flags_only_at_line_start(self):
        # A NAME=value shape embedded mid-line (e.g. inside a sentence) is
        # not what MARKER_RE matches -- report.py's emitters always print
        # the marker as the entire line.
        text = "prefix T=0 suffix\n"
        self.assertEqual(ctml.find_leaked_markers(text), [])

    def test_empty_text_no_leaks(self):
        self.assertEqual(ctml.find_leaked_markers(""), [])

    def test_multiple_leaks_all_reported_with_correct_line_numbers(self):
        text = "\n".join(["ok", "T=0", "still ok", "WEBP_DIST_WINDOWS_RC=7", ""])
        leaks = ctml.find_leaked_markers(text)
        self.assertEqual(leaks, [(2, "T=0"), (4, "WEBP_DIST_WINDOWS_RC=7")])


class MainTests(unittest.TestCase):
    """`main()` behavior driven through a mocked `run_selftest` so these
    tests do not themselves shell out (fast, hermetic) -- the real
    subprocess path is exercised by the manual red/green transcripts."""

    def test_main_returns_0_and_prints_pass_when_no_leaks(self):
        with mock.patch.object(ctml, "run_selftest", return_value=(0, "OK\n")):
            out = io.StringIO()
            with redirect_stdout(out):
                rc = ctml.main([])
        self.assertEqual(rc, 0)
        self.assertIn("PASS", out.getvalue())

    def test_main_returns_1_and_prints_leak_lines_when_leak_present(self):
        with mock.patch.object(
            ctml, "run_selftest", return_value=(0, "before\nT=0\nafter\n")
        ):
            out = io.StringIO()
            with redirect_stdout(out):
                rc = ctml.main([])
        self.assertEqual(rc, 1)
        printed = out.getvalue()
        self.assertIn("FAIL", printed)
        self.assertIn("T=0", printed)

    def test_main_returns_2_when_tests_dir_missing(self):
        with mock.patch.object(ctml, "_TESTS_DIR") as fake_path:
            fake_path.is_dir.return_value = False
            out = io.StringIO()
            with redirect_stdout(out):
                rc = ctml.main([])
        self.assertEqual(rc, 2)


class RunSelftestMechanismTest(unittest.TestCase):
    """Proves `run_selftest()`'s in-process capture actually catches a
    print() that bypasses an INNER capture helper -- the exact leak shape
    of the historical `T=0` defect -- by discovering a throwaway tests
    directory instead of the real `native/scripts/ci/tests/` (running the
    REAL discovery from inside a test IT discovers would recursively
    re-run the whole suite inside every test run)."""

    def test_print_inside_discovered_test_lands_in_outer_capture(self):
        import sys
        import tempfile
        import textwrap
        import uuid

        # A tiny fake test package with one test that leaks a print(),
        # exactly like the real `test_android_ndk_appended_only_when_given`
        # defect: a print() call that isn't wrapped by an inner
        # redirect_stdout of its own.
        #
        # Package/module names are made unique per invocation (not just
        # per-tempdir) -- this repo's own `ci.py selftest` discovery is
        # observably re-entered more than once per process (see the
        # duplicate `CI-SELFTEST:` summary lines noted in
        # tmp/verify/wi50/selftest_tip.log), and unittest's loader (3.14)
        # hard-fails with an `ImportError` if a module name it already has
        # in `sys.modules` gets re-discovered from a DIFFERENT path on a
        # later invocation. A fixed name (`faketests`/`test_leaky`) would
        # collide with itself across those re-entries; a uuid-suffixed name
        # cannot collide with anything, this run or a prior one.
        suffix = uuid.uuid4().hex
        pkg_name = f"faketests_{suffix}"
        mod_name = f"test_leaky_{suffix}"
        with tempfile.TemporaryDirectory() as tmp:
            top = Path(tmp)
            pkg = top / pkg_name
            pkg.mkdir()
            (pkg / "__init__.py").write_text("", encoding="utf-8")
            (pkg / f"{mod_name}.py").write_text(
                textwrap.dedent(
                    """
                    import unittest

                    class LeakyTest(unittest.TestCase):
                        def test_leaks(self):
                            print("T=0")
                            self.assertTrue(True)
                    """
                ),
                encoding="utf-8",
            )
            try:
                with mock.patch.object(ctml, "_TESTS_DIR", pkg), \
                     mock.patch.object(ctml, "_NATIVE_SCRIPTS_DIR", top):
                    rc, output = ctml.run_selftest()
            finally:
                # Leave no trace in sys.modules for a later re-entry of
                # discovery (or a later test) to trip over.
                sys.modules.pop(pkg_name, None)
                sys.modules.pop(f"{pkg_name}.{mod_name}", None)
        self.assertEqual(rc, 0)  # the fake test itself passes
        self.assertIn("T=0", output)  # but its print() still leaked into capture


if __name__ == "__main__":
    unittest.main()
