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
