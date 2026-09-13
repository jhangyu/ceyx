"""Unit tests for native/scripts/ci/check_step_order.py (guard (i))."""

from __future__ import annotations

import io
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from .. import check_step_order as cso

_REPO_ROOT = Path(__file__).resolve().parents[4]
_WORKFLOWS_DIR = _REPO_ROOT / ".github" / "workflows"
_FIXTURES_DIR = Path(__file__).resolve().parent / "fixtures"


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
    tip (green), and against the historical pre-WI-41 red via a committed
    fixture (see class docstring on the red test for why this replaced a
    live `git show` call)."""

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
        # guard is built against (USER RULING G (i)2).
        #
        # R2 CORRECTION (lead16 recall, CI run `34762557520`, job "Build
        # native test targets"): this test PREVIOUSLY resolved the
        # historical blob live via `git show e7778cdb^:...` -- that works
        # on a full local clone but fails with `fatal: invalid object name
        # 'e7778cdb^'` (git exit 128) on a `fetch-depth: 1` (default,
        # unwidened) GitHub Actions checkout, which is what every runner in
        # this repo's workflows actually uses (verified: no `fetch-depth:`
        # override exists anywhere in `.github/workflows/build.yml`, so all
        # three of its `actions/checkout@v4` calls get the action's own
        # default of 1). A live-history-dependent assertion is exactly the
        # defect class lead15's ledger already named for guard (f)②'s
        # future `origin/main..HEAD` range -- it fired here first because
        # this guard shipped first, not because it is a different class.
        #
        # FIX CHOSEN, with the trade stated: a committed BYTE-VERBATIM
        # fixture (`fixtures/build_yml_pre_wi41_ordering.yml`), following
        # guard (b)'s already-signed-off precedent
        # (`check_errexit_rc_capture.py`'s `errexit_rc_probe_defect.yml` --
        # "the fixture is the guarantee... it is a state of a real commit
        # and does not expire when the live file is later fixed",
        # USER-RULING-G.md guard (b) §3). REJECTED alternative:
        # `fetch-depth: 0` on the checkout -- environment-independent and
        # arguably more "honest" (resolves a REAL object, not a copy), but
        # it requires editing `.github/workflows/build.yml`, which has
        # exactly one writer this round (the B3-1 batch-3 wiring owner) and
        # is not mine to touch; it also makes every CI run clone full
        # history, a real cost this fix does not pay. The fixture's own
        # trade, stated rather than hidden: it is a COPY of the historical
        # blob, not the blob itself, and could in principle drift from what
        # `e7778cdb^` actually contains -- mitigated by byte-verbatim
        # capture (see the fixture's own header) and by this test still
        # asserting the exact same finding count/labels as before, so a
        # drifted fixture that stopped representing the real defect would
        # itself go red here (either 0 findings, or a wrong label set).
        text = (_FIXTURES_DIR / "build_yml_pre_wi41_ordering.yml").read_text(
            encoding="utf-8"
        )
        findings, _audit = cso.check_text(text, "build.yml")
        self.assertEqual(len(findings), 2, msg=f"findings: {findings}")
        labels = {label for label, _msg in findings}
        self.assertEqual(labels, {"libraw-provenance-and-alias-table"})

    def test_main_exit_code_at_tip_is_zero(self):
        rc, _out = _run_captured(cso.main, [])
        self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
