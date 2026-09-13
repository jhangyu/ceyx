"""Unit tests for native/scripts/ci/configure_log.py (WI-19a)."""

from __future__ import annotations

import io
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

from .. import configure_log


def _run_captured(func, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = func(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


class ConfigureLogTests(unittest.TestCase):
    def setUp(self):
        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)
        self.tmp = Path(self._tmpdir.name)

    def _write_log(self, name: str, text: str) -> str:
        path = self.tmp / name
        path.write_text(text)
        return str(path)

    # ---- android HEIF check: genuine `.*` wildcard ------------------------

    def test_heif_wildcard_pattern_matches(self):
        log = self._write_log(
            "android_build.log",
            "-- HEIF: libheif 1.17.6 + libde265 1.0.15 (dynamic)\n",
        )
        rc, out, _ = _run_captured(
            configure_log.assert_configure_log,
            log,
            r"HEIF: libheif .* \+ libde265 .* \(dynamic\)",
            "HEIF enabled",
            "configure log does not show the expected HEIF line",
        )
        self.assertEqual(rc, 0)
        self.assertIn("ASSERT HEIF enabled RC=0", out)

    def test_heif_pattern_with_unescaped_plus_and_parens_does_not_match(self):
        """Regression pin for the correction in configure_log.py's docstring:
        an unescaped `+`/`(`/`)` (BRE-literal, but Python `re` metacharacters)
        must NOT be treated as literal here -- this module does `re.search`
        verbatim, no auto-escaping. If this ever starts passing, someone
        quietly added auto-escaping and the module docstring's correction
        needs to be revisited."""
        log = self._write_log(
            "android_build.log",
            "-- HEIF: libheif 1.17.6 + libde265 1.0.15 (dynamic)\n",
        )
        rc, _, _ = _run_captured(
            configure_log.assert_configure_log,
            log,
            r"HEIF: libheif .* + libde265 .* (dynamic)",
            "HEIF enabled",
            "unused",
        )
        self.assertEqual(rc, 1)

    def test_heif_wildcard_pattern_missing_fails_with_error(self):
        log = self._write_log("android_build.log", "-- HEIF: disabled\n")
        rc, out, err = _run_captured(
            configure_log.assert_configure_log,
            log,
            r"HEIF: libheif .* \+ libde265 .* \(dynamic\)",
            "HEIF enabled",
            "configure log does not show the expected HEIF line",
        )
        self.assertEqual(rc, 1)
        self.assertIn("ASSERT HEIF enabled RC=1", out)
        self.assertIn("::error::configure log does not show the expected HEIF line", err)

    # ---- android JXL check: literal parens, no wildcard -------------------

    def test_jxl_literal_parens_pattern_matches(self):
        log = self._write_log(
            "android_build.log",
            "-- JXL: disabled (CEYX_ENABLE_JXL=OFF)\n",
        )
        rc, out, _ = _run_captured(
            configure_log.assert_configure_log,
            log,
            r"JXL: disabled \(CEYX_ENABLE_JXL=OFF\)",
            "JXL explicit-OFF",
            "configure log does not show the expected JXL-disabled line",
        )
        self.assertEqual(rc, 0)
        self.assertIn("ASSERT JXL explicit-OFF RC=0", out)

    def test_jxl_unescaped_parens_do_not_match(self):
        """Regression pin, sibling of the HEIF one above: unescaped `(`/`)`
        are Python `re` group syntax, not literal characters -- they are
        consumed without requiring the input text to contain a literal
        paren at all. `(CEYX_ENABLE_JXL=OFF)` unescaped therefore searches
        for the bare substring `CEYX_ENABLE_JXL=OFF` immediately following
        `JXL: disabled `, which the real text (with a literal `(` in
        between) does not contain -- so this must NOT match."""
        log = self._write_log(
            "android_build.log",
            "-- JXL: disabled (CEYX_ENABLE_JXL=OFF)\n",
        )
        rc, _, _ = _run_captured(
            configure_log.assert_configure_log,
            log,
            r"JXL: disabled (CEYX_ENABLE_JXL=OFF)",
            "JXL explicit-OFF",
            "unused",
        )
        self.assertEqual(rc, 1)

    # ---- windows JXL check: plain literal substring ------------------------

    def test_windows_jxl_static_pattern_matches(self):
        log = self._write_log("configure.log", "-- JXL: static\n")
        rc, out, _ = _run_captured(
            configure_log.assert_configure_log,
            log,
            "JXL: static",
            "JXL static-link",
            "configure.log does not show the expected JXL static-link line",
        )
        self.assertEqual(rc, 0)
        self.assertIn("ASSERT JXL static-link RC=0", out)

    def test_windows_jxl_static_pattern_missing_fails(self):
        log = self._write_log("configure.log", "-- JXL: disabled\n")
        rc, out, err = _run_captured(
            configure_log.assert_configure_log,
            log,
            "JXL: static",
            "JXL static-link",
            "configure.log does not show the expected JXL static-link line",
        )
        self.assertEqual(rc, 1)
        self.assertIn("ASSERT JXL static-link RC=1", out)
        self.assertIn("::error::configure.log does not show the expected JXL static-link line", err)


if __name__ == "__main__":
    unittest.main()
