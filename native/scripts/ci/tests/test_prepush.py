"""Tests for native/scripts/prepush.py + prepush_workflow.py (local-only pre-push gate)."""
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
import prepush_workflow as wf  # noqa: E402

REPO = SCRIPTS.parents[1]
WORKFLOWS = REPO / prepush.WORKFLOWS_DIR

_WORKFLOW = """name: X
on:
  push:
jobs:
  first-job:
    runs-on: ubuntu-latest
    env:
      OUT: ${{ github.workspace }}/out-${{ matrix.arch_tag }}
    strategy:
      matrix:
        include:
          - arch_tag: x86_64
            flag: "true"
          - arch_tag: arm64
            flag: "false"
    steps:
      - name: one-liner
        run: python3 native/scripts/ci.py stage --dir "$OUT"
      - name: gated
        if: matrix.flag == 'true'
        run: |
          set +e
          export K="$RUNNER_TEMP/k"
          cmake --build x \\
            --target y 2>&1 | tee log.txt
          RC=$?
          echo "RC=$RC"
          exit $RC
      - name: shell-logic
        run: |
          if [ -f x ]; then echo y; fi
  second:  # trailing comment
    runs-on: ubuntu-latest
"""


def _jobs(text: str) -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "x.yml").write_text(text, encoding="utf-8")
        return {j.job: j for j in wf.parse_workflow(Path(tmp) / "x.yml")}


class TestWorkflowParser(unittest.TestCase):
    def test_jobs_rows_env_and_steps(self):
        jobs = _jobs(_WORKFLOW)
        self.assertEqual(set(jobs), {"first-job", "second"})
        job = jobs["first-job"]
        self.assertEqual([r["arch_tag"] for r in job.rows], ["x86_64", "arm64"])
        self.assertEqual([s.name for s in job.steps], ["one-liner", "gated", "shell-logic"])
        self.assertEqual(wf.job_env(job, job.rows[0], "W"), {"OUT": "W/out-x86_64"})

    def test_conditions_follow_the_matrix_row(self):
        job = _jobs(_WORKFLOW)["first-job"]
        self.assertEqual(wf.condition(job.steps[1], job.rows[0]), "run")
        self.assertEqual(wf.condition(job.steps[1], job.rows[1]), "skip-row")

    def test_derivation_of_one_liner_and_rc_discipline_body(self):
        job = _jobs(_WORKFLOW)["first-job"]
        env = {"OUT": "W/o", "RUNNER_TEMP": "T"}
        one = wf.derive_commands(job.steps[0], job.rows[0], "W", env)
        self.assertEqual(one[0].argv, ["python3", "native/scripts/ci.py", "stage", "--dir", "W/o"])
        multi = wf.derive_commands(job.steps[1], job.rows[0], "W", env)
        self.assertEqual(len(multi), 1)
        self.assertEqual(multi[0].argv, ["cmake", "--build", "x", "--target", "y"])
        self.assertEqual(multi[0].env, {"K": "T/k"})
        self.assertEqual(multi[0].tee, "log.txt")

    def test_shell_logic_is_not_derivable(self):
        job = _jobs(_WORKFLOW)["first-job"]
        self.assertIsNone(wf.derive_commands(job.steps[2], job.rows[0], "W", {}))

    def test_quoted_operators_are_not_shell_syntax(self):
        self.assertFalse(wf._shell_syntax('python3 x.py --error "a (b) | c > d; e"'))
        self.assertTrue(wf._shell_syntax("a | b"))


class TestScopeDerivation(unittest.TestCase):
    def test_committed_workflows_have_no_scope_problems(self):
        self.assertEqual(prepush.scope_problems(WORKFLOWS), [])

    def test_unclassified_step_is_a_problem(self):
        with tempfile.TemporaryDirectory() as tmp:
            for p in WORKFLOWS.glob("*.yml"):
                text = p.read_text(encoding="utf-8")
                if p.name == "windows_build.yml":
                    text = text.replace("      - name: Verify Windows artifact",
                                        "      - name: A brand new step\n        run: echo hi\n\n"
                                        "      - name: Verify Windows artifact")
                (Path(tmp) / p.name).write_text(text, encoding="utf-8")
            problems = prepush.scope_problems(Path(tmp))
        self.assertTrue(any("'A brand new step' is not classified" in p for p in problems), problems)

    def test_decisions_are_from_the_closed_set(self):
        for job, (decision, reason) in prepush.JOB_SCOPE.items():
            self.assertIn(decision, {"mirrored", "caller", "unported", "excluded"}, job)
            self.assertTrue(reason.strip(), job)


class TestPolicyLint(unittest.TestCase):
    def test_flags_a_workflow_mentioning_prepush(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "x.yml").write_text(_WORKFLOW + "      - run: python3 native/scripts/ci.py prepush\n",
                                             encoding="utf-8")
            self.assertEqual(len(prepush.prepush_mentions(Path(tmp))), 1)

    def test_committed_workflows_are_clean(self):
        self.assertEqual(prepush.prepush_mentions(WORKFLOWS), [])


class TestSignature(unittest.TestCase):
    ENTRY = prepush.HOST_UNSUPPORTED[prepush.WINDOWS_X64]["target:test_raw_hardening"]

    def test_declared_failure_matches(self):
        text = ("tests\\test_raw_hardening.cpp(327,9): error: use of undeclared identifier 'setenv'\n"
                "1 error generated.\n"
                "FAILED: [code=1] CMakeFiles/test_raw_hardening.dir/tests/test_raw_hardening.cpp.obj\n")
        self.assertEqual(prepush.match_signature(self.ENTRY, text), [])

    def test_any_other_failure_is_reported(self):
        text = ("tests\\test_raw_hardening.cpp(327,9): error: use of undeclared identifier 'setenv'\n"
                "tests\\test_raw_hardening.cpp(12,1): error: expected ';' after expression\n")
        problems = prepush.match_signature(self.ENTRY, text)
        self.assertTrue(any("undeclared failure" in p for p in problems), problems)

    def test_missing_signature_is_reported(self):
        problems = prepush.match_signature(self.ENTRY, "FAILED: [code=1] something else\n")
        self.assertTrue(any("not observed" in p for p in problems), problems)

    def test_every_entry_names_class_evidence_and_signature(self):
        for host, items in prepush.HOST_UNSUPPORTED.items():
            self.assertIn(host, prepush.ALL_HOSTS)
            for item, entry in items.items():
                self.assertTrue(item.split(":", 1)[0] in {"target", "decode-main", "decode-case", "plugin-test"}, item)
                self.assertTrue(entry.cls and entry.evidence and entry.must and entry.allowed, item)

    def test_entries_name_real_things(self):
        runners, _ = prepush.gate_runners(REPO)
        exes = {e for v in runners.values() for e in v}
        cases = {c for c, _ in prepush.DECODE_CASES}
        for items in prepush.HOST_UNSUPPORTED.values():
            for item in items:
                kind, name = item.split(":", 1)
                if kind == "target":
                    self.assertIn(name, exes)
                elif kind == "decode-case":
                    self.assertIn(name, cases)
                elif kind == "decode-main":
                    self.assertIn(name, runners)
                else:
                    self.assertTrue((REPO / "plugin" / "test" / name).is_file(), item)


class TestSummary(unittest.TestCase):
    def _summarize(self, results, partial=False, inner=0):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = prepush.summarize(results, "h", "abc", partial, inner)
        lines = [ln for ln in buf.getvalue().splitlines() if ln.startswith("PREPUSH-SUMMARY")]
        self.assertEqual(len(lines), 1)
        return rc, lines[0]

    def test_counters_are_distinct(self):
        rc, line = self._summarize([("a", "PASS", 0), ("b", prepush.SKIP, None), ("c", prepush.HOSTSKIP, None),
                                    ("d", prepush.CIONLY, None), ("e", prepush.COVERED, None),
                                    ("f", prepush.KNOWNDEFECT, None), ("g", prepush.FLAKY, None)], inner=3)
        self.assertEqual(rc, 0)
        self.assertIn("passed=1 failed=0 skipped=1 skipped_host=1 known_defect=1 flaky_known=1 skipped_inner=3 "
                      "ci_only=1 covered=1 partial=0", line)

    def test_teardown_fastfail_signature_rejects_a_failing_check(self):
        entry = prepush.HOST_UNSUPPORTED[prepush.WINDOWS_X64]["decode-case:cfa-color-bggr"]
        good = "[CFA COLOR] file=x B-R=86.90 min=50.00 [PASS]\nPREPUSH_CHILD_EXIT=3221226505\n" \
               "PREPUSH_CASE_RESULT cfa-color-bggr FAIL -- file=x\n"
        self.assertEqual(prepush.match_signature(entry, good), [])
        wrong_exit = good.replace("3221226505", "1")
        self.assertTrue(prepush.match_signature(entry, wrong_exit))
        check_failed = good.replace("[PASS]", "[FAIL]")
        self.assertTrue(prepush.match_signature(entry, check_failed))

    def test_any_failure_is_nonzero(self):
        rc, line = self._summarize([("a", "PASS", 0), ("b", "FAIL", 2)], partial=True)
        self.assertEqual(rc, 1)
        self.assertIn("failed=1", line)
        self.assertIn("failed_steps=b", line)


class TestRoster(unittest.TestCase):
    def test_step_names_unique_on_every_host(self):
        for host in prepush.ALL_HOSTS:
            with redirect_stdout(io.StringIO()):
                names = [s.name for s in prepush.build_steps(REPO, host)]
            self.assertEqual(len(names), len(set(names)), host)

    def test_own_leg_without_implementation_is_red_not_skipped(self):
        with redirect_stdout(io.StringIO()):
            steps = {s.name: s for s in prepush.build_steps(REPO, prepush.LINUX_X64)}
        leg = steps["linux-leg"]
        self.assertIn(prepush.LINUX_X64, leg.hosts)
        self.assertIsNone(leg.action)  # run_inner reports PREPUSH_UNIMPLEMENTED -> FAIL

    def test_runners_derived_from_gates_manifest_exclude_manual(self):
        runners, manual = prepush.gate_runners(REPO)
        self.assertTrue(runners)
        self.assertFalse(set(runners) & set(manual))


if __name__ == "__main__":
    unittest.main()
