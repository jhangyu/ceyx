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


class TestBareBinaryRunner(unittest.TestCase):
    """gates.py kind `runner:native/scripts/prepush.py`: a bare test binary the
    gate builds and runs itself; every failure names what failed, none skips."""

    OK = "[X SUMMARY] executed=3 skipped=0 failed=0 skipped_cases=\n"

    def _verdict(self, build, corpus, run, names=("test_x",), inner=None):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = prepush.run_bare_binaries(Path(build), Path(build), list(names), corpus, run, inner)
        return rc, buf.getvalue()

    def _build_dir(self, tmp, exe="test_x"):
        build = Path(tmp)
        if exe:
            (build / prepush._exe(exe)).write_bytes(b"")
        return build

    def test_registered_in_gates_and_built_by_the_gate(self):
        runners, _ = prepush.gate_runners(REPO)
        self.assertIn("test_dng_slot_decommit", runners.get(prepush.BARE_RUNNER, []))

    def test_pass_runs_the_binary(self):
        ran = []
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = self._verdict(self._build_dir(tmp), {}, lambda argv: ran.append(argv) or (0, self.OK))
        self.assertEqual(rc, 0)
        self.assertEqual(len(ran), 1)
        self.assertIn("PREPUSH_BARE_CASE test_x PASS", out)

    def test_missing_binary_fails_naming_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = self._verdict(self._build_dir(tmp, exe=None), {}, lambda argv: (0, self.OK))
        self.assertEqual(rc, 1)
        self.assertIn("test_x", out)
        self.assertIn("binary not found", out)

    def test_missing_corpus_file_fails_naming_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = self._verdict(self._build_dir(tmp), {"test_x": ("image_samples/none.raf",)},
                                    lambda argv: (0, self.OK))
        self.assertEqual(rc, 1)
        self.assertIn("none.raf", out)
        self.assertIn("corpus file missing", out)

    def test_rc2_could_not_decode_is_a_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = self._verdict(self._build_dir(tmp), {}, lambda argv: (2, ""))
        self.assertEqual(rc, 1)
        self.assertIn("could not decode", out)

    def test_nonzero_rc_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, _ = self._verdict(self._build_dir(tmp), {}, lambda argv: (1, self.OK))
        self.assertEqual(rc, 1)

    def test_internal_skip_is_counted_and_its_reason_echoed(self):
        out_text = ("[X] D7 -> SKIP reason=no-resident-size-instrument\n"
                    "[X SUMMARY] executed=16 skipped=1 failed=0 skipped_cases=D7\n")
        inner = []
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = self._verdict(self._build_dir(tmp), {}, lambda argv: (0, out_text), inner=inner)
        self.assertEqual(rc, 0)
        self.assertEqual(len(inner), 1)
        self.assertIn("D7", inner[0][1])
        self.assertIn("no-resident-size-instrument", inner[0][1])
        self.assertIn("PREPUSH_BARE_INNER_SKIP test_x::D7 reason=no-resident-size-instrument", out)
        self.assertIn("skipped=1", out)

    def test_skip_without_named_case_is_still_counted(self):
        inner = []
        with tempfile.TemporaryDirectory() as tmp:
            rc, _ = self._verdict(self._build_dir(tmp), {},
                                  lambda argv: (0, "[X SUMMARY] executed=2 skipped=2\n"), inner=inner)
        self.assertEqual(rc, 0)
        self.assertEqual(len(inner), 2)

    def test_missing_summary_line_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = self._verdict(self._build_dir(tmp), {}, lambda argv: (0, "all good, trust me\n"))
        self.assertEqual(rc, 1)
        self.assertIn("no SUMMARY line", out)
        self.assertNotIn("PREPUSH_BARE_CASE test_x PASS", out)

    def test_executed_zero_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            rc, out = self._verdict(self._build_dir(tmp), {},
                                    lambda argv: (0, "[X SUMMARY] executed=0 skipped=4 failed=0 skipped_cases=a,b,c,d\n"))
        self.assertEqual(rc, 1)
        self.assertIn("executed=0", out)

    def test_step_is_on_the_roster_for_every_host(self):
        for host in prepush.ALL_HOSTS:
            with redirect_stdout(io.StringIO()):
                steps = {s.name: s for s in prepush.build_steps(REPO, host)}
            self.assertIn("test-bare-binaries", steps, host)
            self.assertIn(host, steps["test-bare-binaries"].hosts)


class BuildTestsGeneratorTests(unittest.TestCase):
    """t_build_tests enumeration/keep-going are generator properties (Ninja = Windows argv, Makefiles = macOS)."""

    def test_help_targets_both_generators_and_ninja_argv_locked(self):
        self.assertIn("test_decode", prepush._help_targets("test_decode: phony\nall: phony\n"))
        self.assertIn("test_decode", prepush._help_targets(
            "The following are some of the valid targets for this Makefile:\n"
            "... all (the default if no target is provided)\n... test_decode\n"))
        self.assertEqual(prepush._KEEP_GOING["Ninja"], ["-k", "0"])

    def test_generator_unknown_when_cache_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(prepush._generator(Path(tmp)), "")
            (Path(tmp) / "CMakeCache.txt").write_text("CMAKE_GENERATOR:INTERNAL=Ninja\n", encoding="utf-8")
            self.assertEqual(prepush._generator(Path(tmp)), "Ninja")


if __name__ == "__main__":
    unittest.main()
