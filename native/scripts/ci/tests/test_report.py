"""Tests for native/scripts/ci/report.py — WI-2.

Every observable line format is asserted as an EXACT byte string, not a
regex, because a near-miss (extra space, different case, missing
underscore) is exactly the kind of drift a regex would silently accept and
a downstream marker-diff (WI-3) would then also miss.
"""

from __future__ import annotations

import io
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))  # native/scripts/

from ci import report  # noqa: E402


def _captured_stdout(fn, *args, **kwargs) -> str:
    buf = io.StringIO()
    with redirect_stdout(buf):
        fn(*args, **kwargs)
    return buf.getvalue()


def _captured_stderr(fn, *args, **kwargs) -> str:
    buf = io.StringIO()
    with redirect_stderr(buf):
        fn(*args, **kwargs)
    return buf.getvalue()


class TestExactEmissionBytes(unittest.TestCase):
    def test_marker(self) -> None:
        self.assertEqual(_captured_stdout(report.marker, "EXPORTS_CHECKED", 35), "EXPORTS_CHECKED=35\n")

    def test_marker_string_value(self) -> None:
        self.assertEqual(_captured_stdout(report.marker, "SOURCE", "gradle-declaration"), "SOURCE=gradle-declaration\n")

    def test_rc(self) -> None:
        self.assertEqual(_captured_stdout(report.rc, "MIN_RUNTIME_DRIFT_RESULT", 0), "MIN_RUNTIME_DRIFT_RESULT_RC=0\n")

    def test_bare_rc_no_label(self) -> None:
        self.assertEqual(_captured_stdout(report.bare_rc, 0), "RC=0\n")

    def test_bare_rc_with_label(self) -> None:
        self.assertEqual(
            _captured_stdout(report.bare_rc, 0, "DT_NEEDED libheif.so.1 in /x/y.so"),
            "RC=0 (DT_NEEDED libheif.so.1 in /x/y.so)\n",
        )

    def test_section(self) -> None:
        self.assertEqual(_captured_stdout(report.section, "orientation probe"), "== orientation probe ==\n")

    def test_notice(self) -> None:
        self.assertEqual(_captured_stdout(report.notice, "hello"), "::notice::hello\n")

    def test_plain(self) -> None:
        self.assertEqual(_captured_stdout(report.plain, "verbatim echo text"), "verbatim echo text\n")


class TestErrorStream(unittest.TestCase):
    def test_error_goes_to_stderr_by_default(self) -> None:
        err = _captured_stderr(report.error, "boom")
        self.assertEqual(err, "::error::boom\n")

    def test_error_stream_override_goes_to_stdout(self) -> None:
        buf = io.StringIO()
        with redirect_stdout(buf):
            report.error("boom", stream=sys.stdout)
        self.assertEqual(buf.getvalue(), "::error::boom\n")

    def test_error_default_writes_nothing_to_stdout(self) -> None:
        out = _captured_stdout(report.error, "boom")
        self.assertEqual(out, "")


class TestGithubEnvAppend(unittest.TestCase):
    def test_appends_one_line(self) -> None:
        import tempfile

        with tempfile.NamedTemporaryFile(mode="w", delete=False, suffix=".env") as fh:
            path = fh.name
        try:
            report.github_env_append(path, "MIN_RUNTIME_android", "21")
            self.assertEqual(Path(path).read_text(encoding="utf-8"), "MIN_RUNTIME_android=21\n")
        finally:
            Path(path).unlink(missing_ok=True)

    def test_github_env_append_refuses_missing_path(self) -> None:
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                report.github_env_append("", "KEY", "value")
        self.assertEqual(ctx.exception.code, 2)

    def test_github_env_append_refuses_nonexistent_path(self) -> None:
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                report.github_env_append("/nonexistent/path/does/not/exist.env", "KEY", "value")
        self.assertEqual(ctx.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
