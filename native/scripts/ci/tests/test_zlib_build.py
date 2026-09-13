"""Unit tests for native/scripts/ci/zlib_build.py (WI-24).

`_PINS` is monkeypatched to a test-only version/sha256 pair for every test
except the unknown-version case, so the tests control the exact bytes
checked against `hashlib.sha256` without needing the real zlib tarball --
same fake-tool-output convention as the rest of this package's tests.
`curl`/`tar`/`cmake` are never really invoked; `run.run` is patched with a
side_effect keyed on argv[0]."""

from __future__ import annotations

import hashlib
import io
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import run as run_module
from .. import zlib_build

_TEST_VERSION = "9.9.9-test"
_TEST_CONTENT = b"not a real zlib tarball, just fixture bytes"
_TEST_SHA256 = hashlib.sha256(_TEST_CONTENT).hexdigest()


def _fake_run_result(returncode=0, stdout="", stderr=""):
    return run_module.RunResult(argv=[], returncode=returncode, stdout=stdout, stderr=stderr)


def _run_captured(func, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = func(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


class _TestPins:
    """Context manager: monkeypatch zlib_build._PINS with a controlled
    version/sha256 pair, restoring the real dict on exit."""

    def __enter__(self):
        self._patcher = mock.patch.dict(
            zlib_build._PINS,
            {_TEST_VERSION: {"url": "https://example.invalid/zlib-test.tar.gz", "sha256": _TEST_SHA256}},
        )
        self._patcher.start()
        return self

    def __exit__(self, *exc):
        self._patcher.stop()


class BuildZlibTests(unittest.TestCase):
    def _workspace(self):
        return Path(tempfile.mkdtemp())

    def test_unknown_version_raises_valueerror(self):
        with self.assertRaises(ValueError):
            zlib_build.build_zlib("0.0.0-nonexistent", str(self._workspace()))

    def test_curl_failure_returns_curl_rc_and_no_marker(self):
        ws = self._workspace()

        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "curl":
                return _fake_run_result(returncode=6, stderr="curl: (6) Could not resolve host")
            raise AssertionError(f"unexpected argv: {argv}")

        with _TestPins(), mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, err = _run_captured(zlib_build.build_zlib, _TEST_VERSION, str(ws))
        self.assertEqual(rc, 6)
        self.assertNotIn("SHA_RC", out)

    def test_sha_mismatch_returns_1_and_no_marker(self):
        ws = self._workspace()

        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "curl":
                # -o <tarball> is argv[3]; write WRONG bytes so the digest
                # will not match the pinned sha256.
                Path(argv[3]).write_bytes(b"wrong bytes entirely")
                return _fake_run_result(returncode=0)
            raise AssertionError(f"unexpected argv beyond curl in this test: {argv}")

        with _TestPins(), mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, err = _run_captured(zlib_build.build_zlib, _TEST_VERSION, str(ws))
        self.assertEqual(rc, 1)
        self.assertIn("FAILED", out)
        # PORTED AS-IS: `set -e` aborts before the shell's "SHA_RC=$?" echo
        # ever runs on a mismatch -- no marker line here.
        self.assertNotIn("SHA_RC", out)

    def test_sha_match_prints_marker_then_tar_failure_returns_tar_rc(self):
        ws = self._workspace()

        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "curl":
                Path(argv[3]).write_bytes(_TEST_CONTENT)
                return _fake_run_result(returncode=0)
            if argv[0] == "tar":
                return _fake_run_result(returncode=2, stderr="tar: unexpected EOF")
            raise AssertionError(f"unexpected argv: {argv}")

        with _TestPins(), mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, err = _run_captured(zlib_build.build_zlib, _TEST_VERSION, str(ws))
        self.assertEqual(rc, 2)
        self.assertIn("SHA_RC=0", out)

    def test_configure_failure_returns_configure_rc(self):
        ws = self._workspace()

        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "curl":
                Path(argv[3]).write_bytes(_TEST_CONTENT)
                return _fake_run_result(returncode=0)
            if argv[0] == "tar":
                return _fake_run_result(returncode=0)
            if argv[0] == "cmake" and "-S" in argv:
                return _fake_run_result(returncode=1, stderr="CMake Error")
            raise AssertionError(f"unexpected argv: {argv}")

        with _TestPins(), mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, err = _run_captured(zlib_build.build_zlib, _TEST_VERSION, str(ws))
        self.assertEqual(rc, 1)
        self.assertIn("CMake Error", out)

    def test_build_failure_returns_build_rc(self):
        ws = self._workspace()

        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "curl":
                Path(argv[3]).write_bytes(_TEST_CONTENT)
                return _fake_run_result(returncode=0)
            if argv[0] == "tar":
                return _fake_run_result(returncode=0)
            if argv[0] == "cmake" and "-S" in argv:
                return _fake_run_result(returncode=0)
            if argv[0] == "cmake" and "--build" in argv:
                return _fake_run_result(returncode=5, stderr="ninja: build stopped")
            raise AssertionError(f"unexpected argv: {argv}")

        with _TestPins(), mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, err = _run_captured(zlib_build.build_zlib, _TEST_VERSION, str(ws))
        self.assertEqual(rc, 5)

    def _fake_run_through_build(self, ws):
        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "curl":
                Path(argv[3]).write_bytes(_TEST_CONTENT)
                return _fake_run_result(returncode=0)
            if argv[0] == "tar":
                return _fake_run_result(returncode=0)
            if argv[0] == "cmake" and "-S" in argv:
                return _fake_run_result(returncode=0)
            if argv[0] == "cmake" and "--build" in argv:
                return _fake_run_result(returncode=0)
            raise AssertionError(f"unexpected argv: {argv}")

        return fake_run

    def test_missing_required_file_returns_1(self):
        ws = self._workspace()
        # Simulate a partial install: only zlib.h present, zconf.h/lib
        # missing -- the exact incomplete-install shape the round-4
        # incident produced.
        install_dir = ws / "zlib-install"
        (install_dir / "include").mkdir(parents=True)
        (install_dir / "include" / "zlib.h").write_text("// zlib.h")

        with _TestPins(), mock.patch.object(
            run_module, "run", side_effect=self._fake_run_through_build(ws)
        ):
            rc, out, err = _run_captured(zlib_build.build_zlib, _TEST_VERSION, str(ws))
        self.assertEqual(rc, 1)
        self.assertIn("zlib install is missing", err)

    def test_happy_path_returns_0_and_lists_full_install(self):
        ws = self._workspace()
        install_dir = ws / "zlib-install"
        (install_dir / "include").mkdir(parents=True)
        (install_dir / "include" / "zlib.h").write_text("// zlib.h")
        (install_dir / "include" / "zconf.h").write_text("// zconf.h")
        (install_dir / "lib").mkdir(parents=True)
        (install_dir / "lib" / "zlibstatic.lib").write_bytes(b"\x00")

        with _TestPins(), mock.patch.object(
            run_module, "run", side_effect=self._fake_run_through_build(ws)
        ):
            rc, out, err = _run_captured(zlib_build.build_zlib, _TEST_VERSION, str(ws))
        self.assertEqual(rc, 0)
        self.assertIn("== installed zlib (complete) ==", out)
        self.assertIn("zlib.h", out)
        self.assertIn("zconf.h", out)
        self.assertIn("zlibstatic.lib", out)
        self.assertIn(
            "zlib install verified: zlib.h, zconf.h, zlibstatic.lib all present", out
        )
        self.assertEqual(err, "")


if __name__ == "__main__":
    unittest.main()
