"""Unit tests for the golden-fixture extraction helper (WI-3, CARRY-4).

A leg whose FULL job log contains no marker lines must emit ONE documented
placeholder comment, as a first-class generator output -- not print nothing
(indistinguishable from a failed capture) and not rely on a hand-authored
exception file (WI-28's regeneration AC could never prove that byte-for-
byte, since "generate nothing" can never equal "a hand-written comment").
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

_MODULE_PATH = Path(__file__).parent / "golden" / "extract_markers.py"


def _load_extract_markers():
    spec = importlib.util.spec_from_file_location("extract_markers", _MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


extract_markers = _load_extract_markers()


_BASELINE_DIR = Path(__file__).parent / "golden" / "baseline"


def _baseline_legs():
    """Leg name encoded in each baseline fixture filename.

    Fixture names are `r<round>-<run-id>-<sha>-<leg>.markers`, so the leg is
    everything after the third hyphen -- a bounded split, because leg names
    themselves contain hyphens ("macos-arm64", "macos-x86_64").
    """
    return {p.stem.split("-", 3)[3] for p in _BASELINE_DIR.glob("*.markers")}


class RosterCoverageTests(unittest.TestCase):
    """AC-2 roster and the committed baseline set must be the same set.

    Without this, a leg could be added to LEG_TO_JOB_NAME with no fixture (or
    a fixture could be orphaned by a rename) and every existing test would
    still pass -- the roster size was not observable by any assertion, which
    is the failure mode this repo keeps re-learning. Deliberately a set
    equality and not a hardcoded `assertEqual(len(...), 10)`: pinning the
    count here would be a second counter for a number whose single source is
    LEG_TO_JOB_NAME itself.
    """

    def test_every_roster_leg_has_exactly_one_baseline_fixture(self):
        roster = set(extract_markers.LEG_TO_JOB_NAME)
        self.assertEqual(
            roster,
            _baseline_legs(),
            "AC-2 roster and golden/baseline/*.markers disagree; every leg needs "
            "exactly one committed fixture and every fixture needs a roster entry",
        )

    def test_fixture_filenames_are_unique_per_leg(self):
        # Set equality above would silently tolerate two fixtures for one leg.
        names = [p.stem.split("-", 3)[3] for p in _BASELINE_DIR.glob("*.markers")]
        self.assertEqual(sorted(names), sorted(set(names)))

    def test_guardscontainer_leg_is_covered(self):
        # The roster extension this fixture set was grown for (9 -> 10).
        self.assertIn("guardscontainer", extract_markers.LEG_TO_JOB_NAME)
        self.assertIn("guardscontainer", _baseline_legs())


class ZeroMarkerPlaceholderTests(unittest.TestCase):
    def test_placeholder_text_for_dartanalyze(self):
        self.assertEqual(
            extract_markers.zero_marker_placeholder("dartanalyze"),
            "# intentionally empty: verify-dart emits no markers; "
            "contract is the job exit code",
        )

    def test_placeholder_defaults_to_leg_name_when_unmapped(self):
        self.assertEqual(
            extract_markers.zero_marker_placeholder("someleg"),
            "# intentionally empty: someleg emits no markers; "
            "contract is the job exit code",
        )

    def test_main_emits_placeholder_when_leg_has_no_markers(self, tmp_path=None):
        import io
        import contextlib
        import tempfile

        # A single-job log (no job-name column) with zero marker-shaped
        # lines: this was the RED case before CARRY-4 -- main() printed
        # nothing at all, so a regenerated fixture would be 0 bytes and
        # indistinguishable from a failed capture.
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as fh:
            fh.write("Step\t2026-09-12T17:17:56.0000000Z some prose, no markers here\n")
            log_path = fh.name

        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                rc = extract_markers.main(["--log", log_path, "--leg", "dartanalyze"])
        finally:
            Path(log_path).unlink()

        self.assertEqual(rc, 0)
        self.assertEqual(
            buf.getvalue().strip(),
            "# intentionally empty: verify-dart emits no markers; "
            "contract is the job exit code",
        )

    def test_main_still_prints_real_markers_when_present(self):
        import io
        import contextlib
        import tempfile

        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as fh:
            fh.write(
                "Linux / x86_64 Vulkan\tStep\t2026-09-12T17:28:22.0000000Z "
                "PROBE_CODECS_RC=0\n"
            )
            log_path = fh.name

        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                rc = extract_markers.main(["--log", log_path, "--leg", "linux"])
        finally:
            Path(log_path).unlink()

        self.assertEqual(rc, 0)
        self.assertEqual(buf.getvalue().strip(), "PROBE_CODECS_RC=0")


if __name__ == "__main__":
    unittest.main()
