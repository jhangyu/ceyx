"""Unit tests for native/scripts/ci/check_step_order.py (guard (i))."""

from __future__ import annotations

import io
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from .. import check_step_order as cso
from .. import run as run_module

_REPO_ROOT = Path(__file__).resolve().parents[4]
_WORKFLOWS_DIR = _REPO_ROOT / ".github" / "workflows"


def _run_captured(func, *args, **kwargs):
    out = io.StringIO()
    with redirect_stdout(out):
        rc = func(*args, **kwargs)
    return rc, out.getvalue()


_PAIR = cso.ProducerPair(
    label="p",
    workflow="fake.yml",
    producer_step_name="Fetch thing",
    consumer_step_names=("Guard thing",),
)

_GAP_PAIR = cso.ProducerPair(
    label="gap",
    workflow="fake.yml",
    producer_step_name="Fetch other thing",
    consumer_step_names=(),
)


def _yaml(steps):
    """Builds a minimal well-formed workflow body from (name, run_line) pairs."""
    lines = [
        "on: push",
        "jobs:",
        "  build:",
        "    runs-on: ubuntu-latest",
        "    steps:",
    ]
    for name, body in steps:
        lines.append(f"      - name: {name}")
        lines.append(f"        run: {body}")
    return "\n".join(lines) + "\n"


class CheckStepOrderPairTableTests(unittest.TestCase):
    """Direct unit coverage of check_text() against small synthetic YAML,
    independent of the real workflow file (which is covered separately
    below, including the historical red)."""

    def test_producer_before_consumer_passes(self):
        text = _yaml([
            ("Fetch thing", "python3 fetch.py"),
            ("Guard thing", "python3 guard.py"),
        ])
        findings, audit = cso.check_text(text, "fake.yml", pairs=(_PAIR,))
        self.assertEqual(findings, [])
        self.assertTrue(any("OK" in line for line in audit))

    def test_consumer_before_producer_fails(self):
        # This is the synthetic mirror of e7778cdb^'s real defect shape:
        # guard step physically precedes its fetch step.
        text = _yaml([
            ("Guard thing", "python3 guard.py"),
            ("Fetch thing", "python3 fetch.py"),
        ])
        findings, _audit = cso.check_text(text, "fake.yml", pairs=(_PAIR,))
        self.assertEqual(len(findings), 1)
        self.assertIn("not ordered after", findings[0][1])

    def test_missing_consumer_step_is_a_finding_not_a_silent_pass(self):
        text = _yaml([("Fetch thing", "python3 fetch.py")])
        findings, _audit = cso.check_text(text, "fake.yml", pairs=(_PAIR,))
        self.assertEqual(len(findings), 1)
        self.assertIn("not found in fake.yml", findings[0][1])

    def test_zero_consumer_producer_is_reported_not_skipped_silently(self):
        text = _yaml([("Fetch other thing", "python3 fetch.py")])
        findings, audit = cso.check_text(text, "fake.yml", pairs=(_GAP_PAIR,))
        self.assertEqual(findings, [])
        self.assertTrue(any("NO_CONSUMER_DECLARED" in line for line in audit))

    def test_stale_producer_reference_is_a_finding(self):
        text = _yaml([("Guard thing", "python3 guard.py")])
        findings, _audit = cso.check_text(text, "fake.yml", pairs=(_PAIR,))
        self.assertEqual(len(findings), 1)
        self.assertIn("not found in fake.yml", findings[0][1])


class CheckStepOrderRealWorkflowTests(unittest.TestCase):
    """Exercises PRODUCER_PAIRS against the real build.yml at the current
    tip (green), and against the historical pre-WI-41 red via `git show`.
    """

    def test_tip_build_yml_passes(self):
        path = _WORKFLOWS_DIR / "build.yml"
        text = path.read_text(encoding="utf-8")
        findings, audit = cso.check_text(text, "build.yml")
        self.assertEqual(findings, [], msg=f"unexpected findings: {findings}")
        self.assertTrue(
            any("NO_CONSUMER_DECLARED" in line and "halide" in line
                for line in audit),
            "expected the halide awaiting-consumer gap to stay visible "
            "at the tip",
        )

    def test_historical_pre_wi41_ordering_is_red(self):
        # e7778cdb is WI-41's fix commit; its PARENT is the red state this
        # guard is built against (USER RULING G (i)2). Routed through the
        # audited `run.run()` primitive rather than a bare `subprocess.run`
        # call -- native/scripts/ci/run.py is the ONE place under
        # native/scripts/ci/ allowed to import subprocess (WI-2 AC,
        # enforced by check_no_test_execution_in_ci.py's
        # [subprocess-outside-run] rule); a second direct import here
        # would be exactly the "second implementation" this repo's
        # standing rule forbids, not merely a lint dodge.
        result = run_module.run(
            ["git", "show", "e7778cdb^:.github/workflows/build.yml"],
            cwd=_REPO_ROOT,
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        text = result.stdout
        findings, _audit = cso.check_text(text, "build.yml")
        self.assertEqual(len(findings), 2, msg=f"findings: {findings}")
        labels = {label for label, _msg in findings}
        self.assertEqual(labels, {"libraw-provenance-and-alias-table"})

    def test_main_exit_code_at_tip_is_zero(self):
        rc, _out = _run_captured(cso.main, [])
        self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
