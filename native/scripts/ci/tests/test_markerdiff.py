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
