"""Tests for native/scripts/prepush.py (local-only merged pre-push gate)."""
from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[2]  # native/scripts/
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

import prepush  # noqa: E402

REPO = SCRIPTS.parents[1]

_WORKFLOW = """name: X
on:
  push:
jobs:
  first-job:
    runs-on: ubuntu-latest
    steps:
      - name: a
        run: echo hi
  second:  # trailing comment
    runs-on: ubuntu-latest
"""


class TestScopeDerivation(unittest.TestCase):
    def test_parses_top_level_job_ids_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "x.yml").write_text(_WORKFLOW, encoding="utf-8")
            self.assertEqual(prepush.workflow_jobs(Path(tmp)), {"x.yml:first-job", "x.yml:second"})

    def test_every_committed_workflow_job_is_classified(self):
        unclassified, stale = prepush.scope_drift(REPO / prepush.WORKFLOWS_DIR)
        self.assertEqual(unclassified, [])
        self.assertEqual(stale, [])

    def test_new_job_is_reported_as_unclassified(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "x.yml").write_text(_WORKFLOW, encoding="utf-8")
            unclassified, _ = prepush.scope_drift(Path(tmp))
            self.assertIn("x.yml:first-job", unclassified)

    def test_decisions_are_from_the_closed_set(self):
        for job, (decision, reason) in prepush.JOB_SCOPE.items():
            self.assertIn(decision, {"included", "host-specific", "excluded"}, job)
            self.assertTrue(reason.strip(), job)


class TestPolicyLint(unittest.TestCase):
    def test_flags_a_workflow_mentioning_prepush(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "x.yml").write_text(_WORKFLOW + "      - run: python3 native/scripts/ci.py prepush\n",
                                             encoding="utf-8")
            self.assertEqual(len(prepush.prepush_mentions(Path(tmp))), 1)

    def test_committed_workflows_are_clean(self):
        self.assertEqual(prepush.prepush_mentions(REPO / prepush.WORKFLOWS_DIR), [])


class TestSummary(unittest.TestCase):
    def _summarize(self, results, partial=False):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = prepush.summarize(results, "h", "abc", partial)
        lines = [ln for ln in buf.getvalue().splitlines() if ln.startswith("PREPUSH-SUMMARY")]
        self.assertEqual(len(lines), 1)
        return rc, lines[0]

    def test_skip_is_counted_and_not_a_pass(self):
        rc, line = self._summarize([("a", "PASS", 0), ("b", "SKIP", None)])
        self.assertEqual(rc, 0)
        self.assertIn("total=2 passed=1 failed=0 skipped=1 partial=0", line)

    def test_any_failure_is_nonzero(self):
        rc, line = self._summarize([("a", "PASS", 0), ("b", "FAIL", 2)], partial=True)
        self.assertEqual(rc, 1)
        self.assertIn("failed=1", line)
        self.assertIn("partial=1", line)
        self.assertIn("failed_steps=b", line)


class TestRoster(unittest.TestCase):
    def test_step_names_unique(self):
        with redirect_stdout(io.StringIO()):
            names = [s.name for s in prepush.build_steps(REPO)]
        self.assertEqual(len(names), len(set(names)))

    def test_runners_derived_from_gates_manifest_exclude_manual(self):
        runners, manual = prepush.gate_runners(REPO)
        self.assertTrue(runners)
        self.assertFalse(set(runners) & set(manual))


if __name__ == "__main__":
    unittest.main()
