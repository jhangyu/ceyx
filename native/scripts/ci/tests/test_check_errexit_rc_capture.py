"""Tests for native/scripts/ci/check_errexit_rc_capture.py (WI-51, guard (b)).

RED-BEFORE-GREEN discipline (USER-RULING-G.md guard (b) §3): the probe must
GENUINELY exhibit the defect -- a real `bash -e` step whose trailing marker
echo is actually skipped, never a synthetic string match. `fixtures/
errexit_rc_probe_defect.yml` is the byte-verbatim D6 layer-1 step as captured
at `6229efb0` (matches `tmp/verify/pyci-parking-lot.md`'s "ORDERED-2's PROBE
FIXTURE"), so its defect is not invented by this test suite -- it is the same
bytes the leader ruling names. `TestLiveMacosBuildYmlStillHasTheDefect` below
additionally confirms the guard flags the REAL, CURRENT
`.github/workflows/macos_build.yml`, which is the stronger demonstration the
ruling calls "preferred but not required".
"""
from __future__ import annotations

import unittest
from pathlib import Path

from .. import check_errexit_rc_capture as guard

FIXTURES_DIR = Path(__file__).resolve().parent / "fixtures"
REPO_ROOT = Path(__file__).resolve().parents[4]


class TestDefectFixtureIsFlagged(unittest.TestCase):
    """RED: the byte-verbatim D6 layer-1 defect must be flagged."""

    def test_unreachable_rc_marker_is_a_violation(self):
        text = (FIXTURES_DIR / "errexit_rc_probe_defect.yml").read_text()
        violations, safe = guard.scan_text(text, "errexit_rc_probe_defect.yml")
        self.assertEqual([], safe)
        self.assertEqual(1, len(violations))
        wf, line_no, step_name, marker, shell_repr, _reason, line_text = violations[0]
        self.assertEqual("errexit_rc_probe_defect.yml", wf)
        self.assertEqual(10, line_no)
        self.assertIn("D6 layer 1", step_name)
        self.assertEqual("D6_LAYER1_RC", marker)
        self.assertEqual("(default bash -e)", shell_repr)
        self.assertIn('echo "D6_LAYER1_RC=$?"', line_text)

    def test_main_returns_1_and_prints_fail_for_the_defect_fixture(self):
        rc = guard.main([str(FIXTURES_DIR / "errexit_rc_probe_defect.yml")])
        self.assertEqual(1, rc)


class TestFixedByCustomShellIsSafe(unittest.TestCase):
    """GREEN case 1: an explicit `shell: bash ... {0}` without `-e`."""

    def test_custom_shell_without_dash_e_is_safe(self):
        text = (FIXTURES_DIR / "errexit_rc_probe_fixed_shell.yml").read_text()
        violations, safe = guard.scan_text(text, "errexit_rc_probe_fixed_shell.yml")
        self.assertEqual([], violations)
        self.assertEqual(1, len(safe))
        self.assertEqual("custom shell: without -e", safe[0][5])

    def test_main_returns_0_for_the_fixed_shell_fixture(self):
        rc = guard.main([str(FIXTURES_DIR / "errexit_rc_probe_fixed_shell.yml")])
        self.assertEqual(0, rc)


class TestFixedBySetPlusEIsSafe(unittest.TestCase):
    """GREEN case 2: an explicit `set +e` before the marker echo."""

    def test_set_plus_e_is_safe(self):
        text = (FIXTURES_DIR / "errexit_rc_probe_fixed_set_plus_e.yml").read_text()
        violations, safe = guard.scan_text(text, "errexit_rc_probe_fixed_set_plus_e.yml")
        self.assertEqual([], violations)
        self.assertEqual(1, len(safe))
        self.assertEqual("set +e precedes it", safe[0][5])

    def test_main_returns_0_for_the_set_plus_e_fixture(self):
        rc = guard.main([str(FIXTURES_DIR / "errexit_rc_probe_fixed_set_plus_e.yml")])
        self.assertEqual(0, rc)


class TestNamedBashShellIsStillUnsafe(unittest.TestCase):
    """A bare `shell: bash` (no `{0}` template) is NOT a custom invocation --
    GitHub still applies its own default `-eo pipefail` -- so it must be
    treated identically to no `shell:` key at all.
    """

    def test_bare_shell_bash_without_template_is_still_a_violation(self):
        text = (
            "jobs:\n"
            "  build:\n"
            "    steps:\n"
            "      - name: X\n"
            "        shell: bash\n"
            "        run: |\n"
            "          python3 foo.py\n"
            '          echo "X_RC=$?"\n'
        )
        violations, safe = guard.scan_text(text, "x.yml")
        self.assertEqual([], safe)
        self.assertEqual(1, len(violations))


class TestNonBashShellIsSkipped(unittest.TestCase):
    """A pwsh step is out of scope -- `$?` does not carry the same meaning
    there, and this guard does not evaluate it either way.
    """

    def test_pwsh_step_is_not_evaluated(self):
        text = (
            "jobs:\n"
            "  build:\n"
            "    steps:\n"
            "      - name: X\n"
            "        shell: pwsh\n"
            "        run: |\n"
            '          echo "X_RC=$?"\n'
        )
        violations, safe = guard.scan_text(text, "x.yml")
        self.assertEqual([], violations)
        self.assertEqual([], safe)


class TestNoRcMarkerIsIgnored(unittest.TestCase):
    def test_step_with_no_rc_echo_produces_no_findings(self):
        text = (
            "jobs:\n"
            "  build:\n"
            "    steps:\n"
            "      - name: X\n"
            "        run: |\n"
            "          echo hello\n"
        )
        violations, safe = guard.scan_text(text, "x.yml")
        self.assertEqual([], violations)
        self.assertEqual([], safe)


class TestAssignmentShapeIsRecognized(unittest.TestCase):
    """The live D6 step's ACTUAL shape (as of lead14's R2 review): a bare
    `VAR_RC=$?` assignment, echoed via `${VAR_RC}` on a LATER line, rather
    than an inline `echo "VAR_RC=$?"`. The assignment line is the true
    capture point and must be recognized on its own -- a checker that only
    ever looked at echo lines would silently miss this shape entirely (the
    R1 defect this class exists to prevent a regression of).
    """

    def test_bare_assignment_without_set_plus_e_is_a_violation(self):
        text = (
            "jobs:\n"
            "  build:\n"
            "    steps:\n"
            "      - name: X\n"
            "        run: |\n"
            "          python3 foo.py\n"
            "          X_RC=$?\n"
            '          echo "X_RC=${X_RC}"\n'
            '          exit "${X_RC}"\n'
        )
        violations, safe = guard.scan_text(text, "x.yml")
        self.assertEqual([], safe)
        self.assertEqual(1, len(violations))
        self.assertEqual("X_RC", violations[0][3])

    def test_bare_assignment_with_set_plus_e_is_safe(self):
        text = (
            "jobs:\n"
            "  build:\n"
            "    steps:\n"
            "      - name: X\n"
            "        run: |\n"
            "          set -u\n"
            "          set +e\n"
            "          python3 foo.py\n"
            "          X_RC=$?\n"
            '          echo "X_RC=${X_RC}"\n'
            '          exit "${X_RC}"\n'
        )
        violations, safe = guard.scan_text(text, "x.yml")
        self.assertEqual([], violations)
        self.assertEqual(1, len(safe))
        self.assertEqual("set +e precedes it", safe[0][5])


class TestLiveMacosBuildYmlD6StepIsCorrectlyRecognizedSafe(unittest.TestCase):
    """The REAL, CURRENT macos_build.yml D6 layer-1 step, as it stands at
    authorship of this revision: it carries `set +e` and uses the
    `VAR_RC=$?` assignment shape (not the echo shape the guard's first cut
    only recognized). Both facts must hold for this test to mean what it
    says -- if the assignment shape were still invisible to the guard, the
    marker would never be counted and this test would pass VACUOUSLY (0
    examined, 0 violations looks identical to N examined, 0 violations from
    the return values alone), so this test asserts MARKERS_EXAMINED >= 1 in
    addition to zero violations, specifically to rule that out.
    """

    def test_real_macos_build_yml_d6_step_is_examined_and_found_safe(self):
        path = REPO_ROOT / ".github" / "workflows" / "macos_build.yml"
        text = path.read_text()
        violations, safe = guard.scan_text(text, ".github/workflows/macos_build.yml")
        d6_safe = [s for s in safe if s[3] == "D6_LAYER1_RC"]
        d6_violations = [v for v in violations if v[3] == "D6_LAYER1_RC"]
        self.assertEqual(
            [], d6_violations,
            "if this fails, the live D6 step regressed back to unsafe -- "
            "not a guard bug.",
        )
        self.assertEqual(
            1, len(d6_safe),
            "expected the live D6_LAYER1_RC marker to be EXAMINED (not "
            "silently invisible) and found safe via its `set +e`. If this "
            "is 0, the guard has stopped recognizing the live step's "
            "shape again -- that is the R1 regression class, not a "
            "'nothing to check here' state.",
        )


if __name__ == "__main__":
    unittest.main()
