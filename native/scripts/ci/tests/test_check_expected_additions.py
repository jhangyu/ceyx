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
        self.assertIn(
            "OK (1 producible entry verified, 0 build-artifact skip(s), 0 producer-input-missing skip(s))",
            out,
        )

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

    def test_missing_producer_input_is_a_declared_skip_not_a_failure(self):
        """WI-44 (root cause: CI run 34746593820): a producer whose
        positional argv names a path absent on THIS machine (the exact
        shape of `check_alias_table_convention.py`'s vendored-LibRaw
        argument before build.yml's fetch step runs) must be a NAMED,
        PRINTED skip and RC=0 -- never a silent no-op, and never
        classified as stale (which would require actually running the
        producer and comparing output) or unclassified (a real producer IS
        known here, its input is just not present yet)."""
        entry = markerdiff._ExpectedAddition("nativetests", "NEEDS_INPUT=1", "test")

        def fail_if_called(argv, cwd=None, env=None):
            self.fail("producer must not be invoked when its input path is missing")

        with mock.patch.object(markerdiff, "EXPECTED_ADDITIONS", (entry,)), \
             mock.patch.object(
                 cea, "_KEY_TO_PRODUCER_SCRIPT",
                 {"NEEDS_INPUT": ("fake/producer.py", ("definitely/does/not/exist.cpp",))},
             ), \
             mock.patch.object(cea, "_BUILD_ARTIFACT_KEYS", frozenset()), \
             mock.patch.object(run_module, "run", side_effect=fail_if_called):
            rc, out, _ = _run_captured(cea.main)
        self.assertEqual(rc, 0)
        self.assertIn("SKIP (producer input not present locally: 'definitely/does/not/exist.cpp')", out)
        self.assertIn("NEEDS_INPUT=1", out)
        self.assertIn("0 build-artifact skip(s), 1 producer-input-missing skip(s)", out)

    def test_missing_producer_inputs_helper_is_argv_order_and_repo_relative(self):
        """Direct unit test of the precondition helper itself (WI-44):
        exactly the argv entries that do not exist under REPO_ROOT come
        back, in argv order -- a real existing path (this test file
        itself) is correctly excluded."""
        real = str(Path(__file__).resolve().relative_to(_REPO_ROOT))
        missing = cea._missing_producer_inputs((real, "totally/fake/path.cpp"))
        self.assertEqual(missing, ["totally/fake/path.cpp"])

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

    # -- WI-53 / guard (f)③ ------------------------------------------------
    # The literal below is deliberately NOT derived from
    # `_KEY_TO_PRODUCER_SCRIPT`. If it were, emptying the map would make the
    # binding test iterate nothing and pass vacuously -- the first false-green
    # shape pre-registered in tmp/verify/wi53/f3-PREREG.md §2 (V1).
    _EXPECTED_DISTINCT_PRODUCER_INVOCATIONS = 2

    def test_producer_map_argv_matches_workflows(self):
        """WI-43, generalized to the WHOLE producer map by WI-53 (guard (f)③).

        `_KEY_TO_PRODUCER_SCRIPT`'s (script, argv) values and the workflows'
        real `python3 <script> ...` invocations are TWO INDEPENDENT
        STATEMENTS of the same fact (the map does not read the workflow
        files at runtime, and the workflow files do not read the map) --
        this test is the mechanical binding between them. A validator that
        derived its input from the thing it validates could never catch
        that drift.

        WHAT WI-53 CHANGED AND WHY IT WAS NOT COSMETIC: the predecessor
        (`test_alias_table_argv_matches_build_yml`) hardcoded the key
        `"TABLE_COUNT"` and scanned only `build.yml`, so the map's OTHER
        producer -- the `check_shell_prohibition.py` pair -- had no argv
        binding at all. That blindness was demonstrated mechanically before
        it was fixed: perturbing the map side and perturbing `build.yml`'s
        real `run:` line each left the full 9-test suite GREEN
        (`tmp/verify/wi53/red-A-before.txt`, `red-B-before.txt`). The loop
        below is driven by the map itself, so a producer added in future is
        bound automatically rather than silently unbound -- which is the
        actual defect class, not the one missing entry."""
        by_invocation: dict[tuple[str, tuple[str, ...]], list[str]] = {}
        for key, invocation in cea._KEY_TO_PRODUCER_SCRIPT.items():
            by_invocation.setdefault(invocation, []).append(key)

        self.assertEqual(
            len(by_invocation),
            self._EXPECTED_DISTINCT_PRODUCER_INVOCATIONS,
            "the producer map's distinct (script, argv) invocation count changed -- update "
            "_EXPECTED_DISTINCT_PRODUCER_INVOCATIONS deliberately. This literal exists so an "
            "emptied/shrunken map fails here instead of passing a vacuous, zero-iteration loop.",
        )

        bound = 0
        for (script, argv), keys in sorted(by_invocation.items()):
            found = list(cea.iter_workflow_invocations(script, workflows_dir=_WORKFLOWS_DIR))
            self.assertEqual(
                len(found), 1,
                f"expected exactly ONE workflow invocation of {script} (producer of "
                f"{sorted(keys)}), found {[(f.workflow, f.line, f.argv) for f in found]} -- "
                "a producer map entry naming a script no workflow invokes (or invokes twice) "
                "is itself the drift this test exists to catch",
            )
            real = found[0]
            self.assertEqual(
                real.argv, argv,
                f"{real.workflow}:{real.line} ({real.step_name!r}) invokes {script} with "
                f"{real.argv!r}, but _KEY_TO_PRODUCER_SCRIPT says {argv!r} for "
                f"{sorted(keys)} -- these must be updated in the same commit",
            )
            bound += 1

        self.assertEqual(bound, self._EXPECTED_DISTINCT_PRODUCER_INVOCATIONS)

    def test_invocation_extractor_ignores_bare_mentions_and_joins_continuations(self):
        """The extractor's two non-obvious semantics, tested directly rather
        than assumed: (1) a script name appearing WITHOUT an interpreter
        token in front of it is a mention, not an invocation -- otherwise a
        comment-shaped or argument-shaped occurrence manufactures a phantom
        binding (guard (g)'s 13-phantom lesson, one layer down); (2) a
        backslash-continued invocation is joined before tokenizing, so its
        argv is read in full instead of as an empty tuple, which would be a
        false PASS rather than a crash."""
        import tempfile

        yaml = """jobs:
  j:
    steps:
      - name: Bare mention only
        run: echo "see native/scripts/demo.py for details"
      - name: Real multi-line invocation
        run: |
          python3 native/scripts/demo.py \\
            --alpha one \\
            --beta two
"""
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "synthetic.yml").write_text(yaml)
            found = list(
                cea.iter_workflow_invocations("native/scripts/demo.py", workflows_dir=Path(d))
            )

        self.assertEqual(len(found), 1, f"bare mention must not count as an invocation: {found}")
        self.assertEqual(found[0].argv, ("--alpha", "one", "--beta", "two"))
        self.assertEqual(found[0].step_name, "Real multi-line invocation")


if __name__ == "__main__":
    unittest.main()
