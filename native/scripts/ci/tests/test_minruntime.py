"""Unit tests for native/scripts/ci/minruntime.py (WI-16b).

Fake tool outputs only (no real readelf/read_min_runtime.py/assert_min_
runtime_matches_declared.py execution). Source-kind dispatch is exercised
through `targets.spec`, never through a platform-name branch in this
module.
"""

from __future__ import annotations

import io
import os
import re
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import minruntime
from .. import run as run_module

_HERE = Path(__file__).resolve().parent
_GOLDEN_DIR = _HERE / "golden" / "expected"


def _fake_run_result(returncode=0, stdout="", stderr=""):
    return run_module.RunResult(argv=[], returncode=returncode, stdout=stdout, stderr=stderr)


class _Cwd:
    def __init__(self, path):
        self._path = str(path)
        self._old = os.getcwd()

    def __enter__(self):
        self._old = os.getcwd()
        os.chdir(self._path)
        return self._path

    def __exit__(self, *_exc):
        os.chdir(self._old)


def _run_captured(func, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = func(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


class MinRuntimeTests(unittest.TestCase):
    def setUp(self):
        import tempfile

        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)

    def _tmp(self):
        return self._tmpdir.name

    def _stage_dylibs(self, root, names):
        staged = Path(root) / "artifacts" / "native"
        staged.mkdir(parents=True, exist_ok=True)
        for n in names:
            (staged / n).write_bytes(b"")
        return staged

    # ---- C-G9 arch enforcement -----------------------------------------

    def test_macos_requires_arch(self):
        with self.assertRaises(ValueError):
            minruntime.min_runtime("macos", arch=None)

    def test_windows_rejects_arch(self):
        with self.assertRaises(ValueError):
            minruntime.min_runtime("windows", arch="x86_64")

    def test_android_rejects_arch(self):
        with self.assertRaises(ValueError):
            minruntime.min_runtime("android", arch="arm64")

    def test_linux_rejects_arch(self):
        with self.assertRaises(ValueError):
            minruntime.min_runtime("linux", arch="x86_64")

    # ---- data-driven dispatch, not platform-name branches ---------------

    def test_min_runtime_source_kinds_are_data_not_branches(self):
        source = re.sub(r'"""(?:.|\n)*?"""', "", Path(minruntime.__file__).read_text(), count=1)
        self.assertNotIn('platform == "android"', source)
        self.assertNotIn("platform == 'android'", source)
        # the only real per-platform branch left is binary-vs-binary
        # (windows vs macos WITHIN the same "binary" source kind), never a
        # branch that singles out linux, windows or android by comparing
        # against the source-kind dispatch itself.
        from .. import targets

        for platform in targets.platform_names():
            self.assertIn("min_runtime_source", targets.spec(platform))

    def test_android_min_runtime_reads_declaration_not_artifact(self):
        captured = {}

        def fake(argv, cwd=None, env=None):
            joined = " ".join(map(str, argv))
            if "read_min_runtime.py" in joined:
                captured["argv"] = argv
                return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_android=21\nSOURCE=gradle-declaration\n")
            return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_DRIFT_RESULT=PASS (measured=21, declared=21 from [android])\n")

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(minruntime.min_runtime, "android")
        self.assertEqual(rc, 0)
        self.assertIn("--gradle", captured["argv"])
        self.assertIn("plugin/android/build.gradle", captured["argv"])
        self.assertNotIn("--artifact", captured["argv"])
        self.assertIn("MIN_RUNTIME_android=21", out)
        self.assertIn("SOURCE=gradle-declaration", out)

    # ---- C-G4 item 2: read RC is echoed but never gated ------------------

    def test_read_min_runtime_rc_ungated(self):
        def fake(argv, cwd=None, env=None):
            joined = " ".join(map(str, argv))
            # NOTE: check the more-specific script names FIRST -- the
            # readelf DUMP FILENAME itself ("readelf_dynsyms.txt") contains
            # the substring "readelf", so a naive `if "readelf" in joined`
            # checked first would misfire on the read_min_runtime.py call
            # too (it passes that filename as --artifact).
            if "read_min_runtime.py" in joined:
                return _fake_run_result(returncode=1, stdout="READ_MIN_RUNTIME_RC=1\n")
            if "assert_min_runtime_matches_declared.py" in joined:
                return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_DRIFT_RESULT=PASS (measured=2.35, declared=2.35 from [linux])\n")
            if "readelf" in joined:
                return _fake_run_result(returncode=0, stdout="Symbol table ...\n")
            raise AssertionError(argv)

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(minruntime.min_runtime, "linux")
        self.assertEqual(rc, 0, "a nonzero READ_MIN_RUNTIME_RC alone must not fail min_runtime()")
        self.assertIn("READ_MIN_RUNTIME_RC=1", out)

    def test_drift_failure_returns_nonzero_no_error_line(self):
        def fake(argv, cwd=None, env=None):
            joined = " ".join(map(str, argv))
            if "read_min_runtime.py" in joined:
                return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_linux=2.36\n")
            if "assert_min_runtime_matches_declared.py" in joined:
                return _fake_run_result(returncode=1, stdout="", stderr="error: measured != declared\n")
            if "readelf" in joined:
                return _fake_run_result(returncode=0, stdout="Symbol table ...\n")
            raise AssertionError(argv)

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, err = _run_captured(minruntime.min_runtime, "linux")
        self.assertEqual(rc, 1)
        self.assertNotIn("::error::", out)
        self.assertNotIn("::error::", err)

    # ---- windows/macos "binary" source kind: staged path, not build dir --

    def test_windows_reads_staged_dll_not_build_dir(self):
        captured = {}

        def fake(argv, cwd=None, env=None):
            joined = " ".join(map(str, argv))
            if "read_min_runtime.py" in joined:
                captured["argv"] = argv
                return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_windows=10.0\n")
            return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_DRIFT_RESULT=PASS (measured=10.0, declared=10.0 from [windows])\n")

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(minruntime.min_runtime, "windows")
        self.assertEqual(rc, 0)
        idx = captured["argv"].index("--artifact")
        self.assertEqual(captured["argv"][idx + 1], "artifacts/native/dng_decoder_native.dll")

    def test_macos_reads_every_staged_dylib_sorted(self):
        captured = {}

        def fake(argv, cwd=None, env=None):
            joined = " ".join(map(str, argv))
            if "read_min_runtime.py" in joined:
                captured["read_argv"] = argv
                return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_macos=15.0\n")
            captured["drift_argv"] = argv
            return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_DRIFT_RESULT=PASS (measured=15.0, declared=15.0 from [macos.arm64])\n")

        tmp = self._tmp()
        self._stage_dylibs(tmp, ["libdng_decoder_native.dylib", "libomp.dylib", "libheif.1.dylib"])
        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(tmp):
            rc, out, _ = _run_captured(minruntime.min_runtime, "macos", arch="arm64")
        self.assertEqual(rc, 0)
        artifact_values = [
            captured["read_argv"][i + 1] for i, a in enumerate(captured["read_argv"]) if a == "--artifact"
        ]
        self.assertEqual(artifact_values, sorted(artifact_values))
        self.assertEqual(len(artifact_values), 3)
        # --arch belongs to the DRIFT-assert call only, never to
        # read_min_runtime.py's own argv.
        self.assertNotIn("--arch", captured["read_argv"])
        self.assertIn("--arch", captured["drift_argv"])
        self.assertEqual(captured["drift_argv"][captured["drift_argv"].index("--arch") + 1], "arm64")

    # ---- golden emission -------------------------------------------------

    def test_emission_matches_golden_windows(self):
        def fake(argv, cwd=None, env=None):
            joined = " ".join(map(str, argv))
            if "read_min_runtime.py" in joined:
                Path("artifacts").mkdir(exist_ok=True)
                Path("artifacts/min_runtime.txt").write_text("MIN_RUNTIME_windows=10.0\n")
                return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_windows=10.0\n")
            return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_DRIFT_RESULT=PASS (measured=10.0, declared=10.0 from [windows])\n")

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(minruntime.min_runtime, "windows")
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "min-runtime-windows.markers").read_text()
        self.assertEqual(out, expected)

    def test_emission_matches_golden_macos(self):
        def fake(argv, cwd=None, env=None):
            joined = " ".join(map(str, argv))
            if "read_min_runtime.py" in joined:
                Path("artifacts").mkdir(exist_ok=True)
                Path("artifacts/min_runtime.txt").write_text("MIN_RUNTIME_macos=15.0\n")
                return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_macos=15.0\n")
            return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_DRIFT_RESULT=PASS (measured=15.0, declared=15.0 from [macos.arm64])\n")

        tmp = self._tmp()
        self._stage_dylibs(tmp, ["libdng_decoder_native.dylib"])
        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(tmp):
            rc, out, _ = _run_captured(minruntime.min_runtime, "macos", arch="arm64")
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "min-runtime-macos.markers").read_text()
        self.assertEqual(out, expected)

    def test_emission_matches_golden_android(self):
        def fake(argv, cwd=None, env=None):
            joined = " ".join(map(str, argv))
            if "read_min_runtime.py" in joined:
                Path("min_runtime.txt").write_text("MIN_RUNTIME_android=21\nSOURCE=gradle-declaration\n")
                return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_android=21\nSOURCE=gradle-declaration\n")
            return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_DRIFT_RESULT=PASS (measured=21, declared=21 from [android])\n")

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(minruntime.min_runtime, "android")
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "min-runtime-android.markers").read_text()
        self.assertEqual(out, expected)


if __name__ == "__main__":
    unittest.main()
