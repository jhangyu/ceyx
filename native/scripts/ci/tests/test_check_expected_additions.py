"""Unit tests for native/scripts/ci/check_expected_additions.py."""

from __future__ import annotations

import io
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import check_expected_additions as cea
from .. import markerdiff
from .. import run as run_module
from .. import workflow_scan

_REPO_ROOT = Path(__file__).resolve().parents[4]
_WORKFLOWS_DIR = _REPO_ROOT / ".github" / "workflows"


def _fake_run_result(stdout="", stderr=""):
    return run_module.RunResult(argv=[], returncode=0, stdout=stdout, stderr=stderr)


def _run_captured(func, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = func(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


class CheckExpectedAdditionsTests(unittest.TestCase):
    def test_matching_ledger_passes(self):
        entry = markerdiff._ExpectedAddition("nativetests", "FAKE_KEY=7", "test")

        def fake_run(argv, cwd=None, env=None):
            return _fake_run_result(stdout="FAKE_KEY=7\nOTHER=1\n")

        with mock.patch.object(markerdiff, "EXPECTED_ADDITIONS", (entry,)), \
             mock.patch.object(cea, "_KEY_TO_PRODUCER_SCRIPT", {"FAKE_KEY": ("fake/producer.py", ())}), \
             mock.patch.object(cea, "_BUILD_ARTIFACT_KEYS", frozenset()), \
             mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, _ = _run_captured(cea.main)
        self.assertEqual(rc, 0)
        self.assertIn("OK (1 producible entry verified, 0 build-artifact skip(s))", out)

    def test_stale_ledger_fails_naming_entry_and_both_lines(self):
        """The exact regression class this script exists to catch: a
        ratchet changed the producer's output but nobody updated the
        ledger."""
        entry = markerdiff._ExpectedAddition("nativetests", "SHELL_ALLOWLIST_SIZE=109", "push 4")

        def fake_run(argv, cwd=None, env=None):
            return _fake_run_result(stdout="SHELL_ALLOWLIST_SIZE=105\n")

        with mock.patch.object(markerdiff, "EXPECTED_ADDITIONS", (entry,)), \
             mock.patch.object(
                 cea, "_KEY_TO_PRODUCER_SCRIPT", {"SHELL_ALLOWLIST_SIZE": ("fake/producer.py", ())}
             ), \
             mock.patch.object(cea, "_BUILD_ARTIFACT_KEYS", frozenset()), \
             mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, err = _run_captured(cea.main)
        self.assertEqual(rc, 1)
        self.assertIn("SHELL_ALLOWLIST_SIZE=109", err)
        self.assertIn("SHELL_ALLOWLIST_SIZE=105", err)

    def test_unclassified_key_is_a_hard_fail_not_a_skip(self):
        """A ledger entry with no known producer and no build-artifact
        classification must FAIL -- a silent skip for an unrecognised input
        is the false-green shape this campaign keeps finding."""
        entry = markerdiff._ExpectedAddition("nativetests", "MYSTERY_MARKER=1", "test")

        with mock.patch.object(markerdiff, "EXPECTED_ADDITIONS", (entry,)), \
             mock.patch.object(cea, "_KEY_TO_PRODUCER_SCRIPT", {}), \
             mock.patch.object(cea, "_BUILD_ARTIFACT_KEYS", frozenset()):
            rc, out, err = _run_captured(cea.main)
        self.assertEqual(rc, 1)
        self.assertIn("MYSTERY_MARKER=1", err)
        self.assertIn("no identifiable local producer", err)

    def test_build_artifact_key_is_an_explicit_printed_skip(self):
        entry = markerdiff._ExpectedAddition("some_leg", "BUILD_ONLY_MARKER=42", "test")

        with mock.patch.object(markerdiff, "EXPECTED_ADDITIONS", (entry,)), \
             mock.patch.object(cea, "_KEY_TO_PRODUCER_SCRIPT", {}), \
             mock.patch.object(cea, "_BUILD_ARTIFACT_KEYS", frozenset({"BUILD_ONLY_MARKER"})):
            rc, out, err = _run_captured(cea.main)
        self.assertEqual(rc, 0)
        self.assertIn("SKIP (build-artifact, not locally producible): BUILD_ONLY_MARKER=42", out)

    def test_shared_producer_invoked_once_for_two_ledger_entries(self):
        """SHELL_ALLOWLIST_SIZE and SHELL_PROHIBITION_RESULT both come from
        check_shell_prohibition.py -- it must not be run twice."""
        entries = (
            markerdiff._ExpectedAddition("nativetests", "SHELL_ALLOWLIST_SIZE=109", "push 4"),
            markerdiff._ExpectedAddition("nativetests", "SHELL_PROHIBITION_RESULT=PASS", "push 2"),
        )
        call_count = {"n": 0}

        def fake_run(argv, cwd=None, env=None):
            call_count["n"] += 1
            return _fake_run_result(stdout="SHELL_ALLOWLIST_SIZE=109\nSHELL_PROHIBITION_RESULT=PASS\n")

        with mock.patch.object(markerdiff, "EXPECTED_ADDITIONS", entries), \
             mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, _ = _run_captured(cea.main)
        self.assertEqual(rc, 0)
        self.assertEqual(call_count["n"], 1, "shared producer must be cached, not re-run per ledger entry")

    def test_current_real_ledger_matches_current_real_producer(self):
        """Integration-shaped unit test: runs the REAL check_shell_prohibition.py
        as a bare subprocess (via run.run, no mocking) and confirms today's
        actual ledger entries are not already stale."""
        rc, out, err = _run_captured(cea.main)
        self.assertEqual(rc, 0, f"real ledger vs real producer mismatch: stdout={out!r} stderr={err!r}")

    def test_alias_table_argv_matches_build_yml(self):
        """WI-43: `_KEY_TO_PRODUCER_SCRIPT["TABLE_COUNT"]`'s argv and
        `build.yml`'s real invocation of `check_alias_table_convention.py`
        are TWO INDEPENDENT STATEMENTS of the same fact (the map does not
        read the workflow file at runtime, and the workflow file does not
        read the map) -- this test is the mechanical binding between them.
        If either drifts, this fails; a validator that derived its input
        from the thing it validates could never catch that drift, which is
        the exact shape this campaign spent this WI finding three times
        over (allowlist.py/markerdiff.py, the WI-26 step wiring/markerdiff.py
        ledger, and now this producer map/build.yml itself)."""
        script, argv = cea._KEY_TO_PRODUCER_SCRIPT["TABLE_COUNT"]
        self.assertEqual(
            cea._KEY_TO_PRODUCER_SCRIPT["ALIAS_TABLE_FIRST_ELEMENT_ALL_AT"],
            (script, argv),
            "TABLE_COUNT and ALIAS_TABLE_FIRST_ELEMENT_ALL_AT share one producer invocation",
        )

        text = (_WORKFLOWS_DIR / "build.yml").read_text()
        matches = []
        for step in workflow_scan.iter_run_steps(text, "build.yml"):
            code = workflow_scan.code_lines(step)
            if code and "check_alias_table_convention.py" in code[0]:
                matches.append((step.step_name, code[0]))

        self.assertEqual(
            len(matches), 1,
            f"expected exactly one build.yml step invoking check_alias_table_convention.py, found {matches}",
        )
        _step_name, real_line = matches[0]
        real_argv = tuple(real_line.split()[2:])  # drop "python3 <script>"
        self.assertEqual(
            real_argv, argv,
            f"build.yml invokes check_alias_table_convention.py with {real_argv!r}, "
            f"but _KEY_TO_PRODUCER_SCRIPT says {argv!r} -- these must be updated together",
        )


if __name__ == "__main__":
    unittest.main()
