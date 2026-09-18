"""Unit tests for native/scripts/ci/markerdiff.py (WI-3)."""

from __future__ import annotations

import unittest
from pathlib import Path

from .. import markerdiff

_GOLDEN_DIR = Path(__file__).parent / "golden" / "baseline"
_LINUX_BASELINE = _GOLDEN_DIR / "r7-34707800234-d33cc607-linux.markers"


class ExtractTests(unittest.TestCase):
    def test_extract_ignores_non_marker_prose(self):
        text = "Linux / x86_64 Vulkan\tStep\t2026-09-12T17:28:22.0000000Z hello there\n"
        self.assertEqual(markerdiff.extract(text), [])

    def test_extract_finds_marker_after_stripping_prefix(self):
        text = "Linux / x86_64 Vulkan\tStep\t2026-09-12T17:28:22.0000000Z PROBE_CODECS_RC=0\n"
        self.assertEqual(markerdiff.extract(text), ["PROBE_CODECS_RC=0"])

    def test_extract_handles_bare_marker_file(self):
        text = "PROBE_CODECS_RC=0\nEXPORTS_CHECKED=35\n"
        self.assertEqual(
            markerdiff.extract(text), ["PROBE_CODECS_RC=0", "EXPORTS_CHECKED=35"]
        )


class NormalizeTests(unittest.TestCase):
    def test_path_normalization_is_not_a_delta(self):
        baseline = "PATH=/home/runner/work/ceyx/ceyx/build\n"
        candidate = "PATH=/Users/someone/checkout/ceyx/build\n"
        rc, deltas = markerdiff.diff(baseline, candidate)
        self.assertEqual(rc, 0)
        self.assertEqual(deltas, [])

    def test_value_digits_survive_normalization(self):
        line = "EXPORTS_CHECKED=35"
        self.assertEqual(markerdiff.normalize(line), "EXPORTS_CHECKED=35")

    def test_tempdir_path_normalized(self):
        line = "SOME_PATH=/tmp/abc123/foo.txt"
        self.assertEqual(markerdiff.normalize(line), "SOME_PATH=<TMP>")


class DiffTests(unittest.TestCase):
    def test_identical_inputs_pass_with_zero_deltas(self):
        text = "PROBE_CODECS_RC=0\nPROBE_CODECS_RC=0\nEXPORTS_RESULT=PASS\n"
        rc, deltas = markerdiff.diff(text, text)
        self.assertEqual(rc, 0)
        self.assertEqual(deltas, [])

    def test_dropped_duplicate_is_a_failure(self):
        baseline = "PROBE_CODECS_RC=0\nPROBE_CODECS_RC=0\n"
        candidate = "PROBE_CODECS_RC=0\n"
        rc, deltas = markerdiff.diff(baseline, candidate)
        self.assertNotEqual(rc, 0)
        self.assertIn("-1 PROBE_CODECS_RC=0", deltas)

    def test_added_marker_is_a_failure(self):
        baseline = "EXPORTS_RESULT=PASS\n"
        candidate = "EXPORTS_RESULT=PASS\nEXTRA_MARKER=1\n"
        rc, deltas = markerdiff.diff(baseline, candidate)
        self.assertNotEqual(rc, 0)
        self.assertIn("+1 EXTRA_MARKER=1", deltas)

    def test_baseline_preserves_double_probe_rc(self):
        text = _LINUX_BASELINE.read_text(encoding="utf-8")
        self.assertEqual(text.count("PROBE_CODECS_RC=0"), 2)


class ObservabilityMarkerTests(unittest.TestCase):
    """WI-splits: DLL_SIZE_BYTES value tolerated, presence/count still asserted."""

    def test_observability_value_change_is_not_a_delta(self):
        baseline = "DLL_SIZE_BYTES=10035712\n"
        candidate = "DLL_SIZE_BYTES=10049536\n"
        rc, deltas = markerdiff.diff(baseline, candidate)
        self.assertEqual(rc, 0)
        self.assertEqual(deltas, [])

    def test_observability_marker_absent_is_a_failure(self):
        baseline = "DLL_SIZE_BYTES=10035712\nEXPORTS_RESULT=PASS\n"
        candidate = "EXPORTS_RESULT=PASS\n"
        rc, deltas = markerdiff.diff(baseline, candidate)
        self.assertNotEqual(rc, 0)
        self.assertTrue(any("DLL_SIZE_BYTES" in d for d in deltas))

    def test_observability_marker_double_count_is_a_failure(self):
        baseline = "DLL_SIZE_BYTES=10035712\n"
        candidate = "DLL_SIZE_BYTES=10049536\nDLL_SIZE_BYTES=99999999\n"
        rc, deltas = markerdiff.diff(baseline, candidate)
        self.assertNotEqual(rc, 0)
        self.assertTrue(any("DLL_SIZE_BYTES" in d for d in deltas))

    def test_assertion_marker_value_change_still_fails(self):
        # regression pin: the split must not silently swallow non-observability keys
        baseline = "EXPORTS_CHECKED=35\n"
        candidate = "EXPORTS_CHECKED=36\n"
        rc, deltas = markerdiff.diff(baseline, candidate)
        self.assertNotEqual(rc, 0)
        self.assertIn("-1 EXPORTS_CHECKED=35", deltas)
        self.assertIn("+1 EXPORTS_CHECKED=36", deltas)

    def test_main_prints_observability_block_naming_dll_size_bytes(self):
        import io
        import contextlib

        baseline = "DLL_SIZE_BYTES=10035712\n"
        candidate = "DLL_SIZE_BYTES=10049536\n"
        b_path = Path(__file__).parent / "_tmp_obs_baseline.markers"
        c_path = Path(__file__).parent / "_tmp_obs_candidate.markers"
        b_path.write_text(baseline, encoding="utf-8")
        c_path.write_text(candidate, encoding="utf-8")
        try:
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = markerdiff.main(
                    ["--baseline", str(b_path), "--candidate", str(c_path)]
                )
            self.assertEqual(rc, 0)
            output = buf.getvalue()
            self.assertIn("DLL_SIZE_BYTES", output)
            self.assertIn("MARKER_DIFF_RESULT=PASS", output)
        finally:
            b_path.unlink(missing_ok=True)
            c_path.unlink(missing_ok=True)


class ExpectedAdditionsLedgerTests(unittest.TestCase):
    """WI-follow-up: a push that ADDS a guard/marker must not fail AC-2 for
    succeeding at its own job, while an unlisted addition or any removal
    still must."""

    def test_ledger_addition_is_not_a_delta_for_matching_leg(self):
        baseline = "EXPORTS_RESULT=PASS\n"
        candidate = (
            "EXPORTS_RESULT=PASS\n"
            "SHELL_ALLOWLIST_SIZE=42\n"
            "SHELL_PROHIBITION_RESULT=PASS\n"
        )
        rc, deltas = markerdiff.diff(baseline, candidate, leg="nativetests")
        self.assertEqual(rc, 0)
        self.assertEqual(deltas, [])

    def test_ledger_addition_still_a_delta_for_non_matching_leg(self):
        # Same lines, but the leg does not match any ledger entry -- the
        # ledger is scoped, not global.
        baseline = "EXPORTS_RESULT=PASS\n"
        candidate = "EXPORTS_RESULT=PASS\nSHELL_ALLOWLIST_SIZE=95\n"
        rc, deltas = markerdiff.diff(baseline, candidate, leg="linux")
        self.assertNotEqual(rc, 0)
        self.assertIn("+1 SHELL_ALLOWLIST_SIZE=95", deltas)

    def test_unlisted_marker_addition_still_fails(self):
        baseline = "EXPORTS_RESULT=PASS\n"
        candidate = "EXPORTS_RESULT=PASS\nSOME_NEW_MARKER=1\n"
        rc, deltas = markerdiff.diff(baseline, candidate, leg="nativetests")
        self.assertNotEqual(rc, 0)
        self.assertIn("+1 SOME_NEW_MARKER=1", deltas)

    def test_listed_marker_disappearing_is_a_hard_failure(self):
        # If a ledger line was already present in the baseline (i.e. this
        # simulates a later state where it has "graduated" into the
        # anchor) and vanishes from the candidate, that is a `-N` -- never
        # suppressed, regardless of the ledger.
        baseline = "SHELL_ALLOWLIST_SIZE=95\n"
        candidate = ""
        rc, deltas = markerdiff.diff(baseline, candidate, leg="nativetests")
        self.assertNotEqual(rc, 0)
        self.assertIn("-1 SHELL_ALLOWLIST_SIZE=95", deltas)

    def test_ledger_value_changed_is_a_hard_failure_not_forgiven(self):
        # This is the assertion-class proof: only the EXACT ledger line is
        # forgiven. A different (stale/unratcheted) value for the same key
        # is an ordinary unlisted addition and still fails -- 106 is a
        # stale pre-ratchet value, no longer on the ledger now that it
        # reads 44 (P-23 ratchet; was 45 under push 9 / WI-25).
        #
        # NOTE (WI-53): this file hardcodes the ratcheted value in two more
        # places (the candidate line above and the report-lines assertion
        # below). That makes THREE hand-maintained copies of a number whose
        # single source is allowlist.MUST_STAY's length -- the drift shape
        # this repo's "never write a second counter, call this one" rule
        # exists to reject. P-21/P-22's ratchets (44->43->42) will break
        # these same lines again. Flagged to the lead rather than
        # refactored mid-round.
        baseline = "EXPORTS_RESULT=PASS\n"
        candidate = "EXPORTS_RESULT=PASS\nSHELL_ALLOWLIST_SIZE=106\n"
        rc, deltas = markerdiff.diff(baseline, candidate, leg="nativetests")
        self.assertNotEqual(rc, 0)
        self.assertIn("+1 SHELL_ALLOWLIST_SIZE=106", deltas)

    def test_expected_addition_report_lines_names_both_entries(self):
        lines = markerdiff.expected_addition_report_lines("nativetests")
        joined = "\n".join(lines)
        self.assertIn("SHELL_ALLOWLIST_SIZE=42", joined)
        self.assertIn("SHELL_PROHIBITION_RESULT=PASS", joined)

    def test_expected_addition_report_lines_empty_for_other_leg(self):
        self.assertEqual(markerdiff.expected_addition_report_lines("linux"), [])

    def test_wi43_alias_table_markers_are_not_deltas_for_matching_leg(self):
        # WI-43: WI-26 wired check_alias_table_convention.py into
        # build.yml's nativetests leg -- a step that PRINTS is exactly as
        # much an emission change as a pinned literal, so its two markers
        # (check_alias_table_convention.py:92, :98) get the same ledger
        # treatment as SHELL_ALLOWLIST_SIZE/SHELL_PROHIBITION_RESULT above.
        baseline = "EXPORTS_RESULT=PASS\n"
        candidate = (
            "EXPORTS_RESULT=PASS\n"
            "TABLE_COUNT=10\n"
            "ALIAS_TABLE_FIRST_ELEMENT_ALL_AT=YES\n"
        )
        rc, deltas = markerdiff.diff(baseline, candidate, leg="nativetests")
        self.assertEqual(rc, 0)
        self.assertEqual(deltas, [])

    def test_wi43_alias_table_markers_still_a_delta_for_non_matching_leg(self):
        baseline = "EXPORTS_RESULT=PASS\n"
        candidate = "EXPORTS_RESULT=PASS\nTABLE_COUNT=10\n"
        rc, deltas = markerdiff.diff(baseline, candidate, leg="linux")
        self.assertNotEqual(rc, 0)
        self.assertIn("+1 TABLE_COUNT=10", deltas)

    def test_wi43_alias_table_value_changed_is_a_hard_failure_not_forgiven(self):
        # Same assertion-class proof as SHELL_ALLOWLIST_SIZE's 106 case:
        # only the EXACT ledger value is forgiven. TABLE_COUNT=11 is not
        # the ledgered 10, so it is an ordinary unlisted addition.
        baseline = "EXPORTS_RESULT=PASS\n"
        candidate = "EXPORTS_RESULT=PASS\nTABLE_COUNT=11\n"
        rc, deltas = markerdiff.diff(baseline, candidate, leg="nativetests")
        self.assertNotEqual(rc, 0)
        self.assertIn("+1 TABLE_COUNT=11", deltas)

    def test_expected_addition_report_lines_names_all_four_entries(self):
        lines = markerdiff.expected_addition_report_lines("nativetests")
        joined = "\n".join(lines)
        self.assertIn("TABLE_COUNT=10", joined)
        self.assertIn("ALIAS_TABLE_FIRST_ELEMENT_ALL_AT=YES", joined)

    def test_observability_split_unaffected_by_ledger(self):
        # Regression pin: the ledger addition must not interact with the
        # observability split built earlier.
        baseline = "DLL_SIZE_BYTES=10035712\n"
        candidate = "DLL_SIZE_BYTES=10049536\n"
        rc, deltas = markerdiff.diff(baseline, candidate, leg="windows")
        self.assertEqual(rc, 0)
        self.assertEqual(deltas, [])


class MainCliTests(unittest.TestCase):
    def test_main_prints_pass_for_identical_files(self, ):
        import io
        import contextlib

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = markerdiff.main(
                ["--baseline", str(_LINUX_BASELINE), "--candidate", str(_LINUX_BASELINE)]
            )
        self.assertEqual(rc, 0)
        output = buf.getvalue()
        self.assertIn("MARKER_DIFF_RESULT=PASS", output)
        self.assertIn("MARKER_DIFF_DELTAS=0", output)


if __name__ == "__main__":
    unittest.main()
