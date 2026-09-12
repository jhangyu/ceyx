"""Unit tests for native/scripts/ci/orientation.py (WI-10).

Fake tool outputs only (no real strings/nm/cmake/probe execution). Android's
scan is a genuinely different algorithm from linux/windows (see module
docstring) and is tested separately rather than assumed identical.
"""

from __future__ import annotations

import io
import os
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import orientation
from .. import run as run_module

_LINUX_SO = "native/build-linux/libdng_decoder_native.so"
_WIN_DLL = "native/build-windows/dng_decoder_native.dll"


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


_HERE = Path(__file__).resolve().parent
_GOLDEN_DIR = _HERE / "golden" / "expected"

_HAPPY_STRINGS_TEXT = (
    "dng_render_stage4_split\n"
    "orient_a_x\norient_b_x\norient_c_x\norient_a_y\norient_b_y\norient_c_y\n"
)


class OrientationTests(unittest.TestCase):
    def setUp(self):
        import tempfile

        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)

    def _tmp(self):
        return self._tmpdir.name

    # ---- linux/windows shared shape ------------------------------------

    def test_missing_symbol_names_all_missing(self):
        """Red: missing suffix order is symbols first, then coefficients."""
        text = "orient_a_x\norient_b_x\n"  # symbol + 4 coeffs missing

        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout=text)

        with mock.patch.object(orientation, "shutil") as fake_shutil, \
             mock.patch.object(run_module, "run", side_effect=fake), \
             _Cwd(self._tmp()):
            fake_shutil.which.side_effect = lambda name: f"/usr/bin/{name}" if name == "llvm-strings" else None
            rc, out, err = _run_captured(orientation._strings_scan, "linux", _LINUX_SO, "so_strings.txt")
        self.assertEqual(rc, 1)
        self.assertIn(
            "missing fused-orientation signal(s) in "
            f"{_LINUX_SO}: symbol:dng_render_stage4_split string:orient_c_x string:orient_a_y "
            "string:orient_b_y string:orient_c_y —",
            err,
        )

    def test_whole_line_match_not_substring(self):
        """A strings file containing `xxorient_a_xyy` (substring, not an
        exact line) must FAIL -- a substring match would weaken the check."""
        text = "dng_render_stage4_split\nxxorient_a_xyy\norient_b_x\norient_c_x\norient_a_y\norient_b_y\norient_c_y\n"

        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout=text)

        with mock.patch.object(orientation, "shutil") as fake_shutil, \
             mock.patch.object(run_module, "run", side_effect=fake), \
             _Cwd(self._tmp()):
            fake_shutil.which.side_effect = lambda name: f"/usr/bin/{name}" if name == "llvm-strings" else None
            rc, out, err = _run_captured(orientation._strings_scan, "linux", _LINUX_SO, "so_strings.txt")
        self.assertEqual(rc, 1, "xxorient_a_xyy must not satisfy orient_a_x")
        self.assertIn("string:orient_a_x", err)

    def test_no_strings_tool_error_text_is_verbatim(self):
        """Red: neither candidate tool resolves."""
        with mock.patch.object(orientation, "shutil") as fake_shutil, _Cwd(self._tmp()):
            fake_shutil.which.return_value = None
            rc, out, err = _run_captured(orientation._strings_scan, "linux", _LINUX_SO, "so_strings.txt")
        self.assertEqual(rc, 1)
        self.assertIn(
            "::error::neither llvm-strings nor strings is on PATH; capability is UNVERIFIED, "
            "refusing to publish.",
            err,
        )

    def test_happy_path_linux(self):
        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout=_HAPPY_STRINGS_TEXT)

        with mock.patch.object(orientation, "shutil") as fake_shutil, \
             mock.patch.object(run_module, "run", side_effect=fake), \
             _Cwd(self._tmp()):
            fake_shutil.which.side_effect = lambda name: f"/usr/bin/{name}" if name == "llvm-strings" else None
            rc, out, _ = _run_captured(orientation._strings_scan, "linux", _LINUX_SO, "so_strings.txt")
        self.assertEqual(rc, 0)
        self.assertIn("STRINGS_TOOL=llvm-strings STRINGS_RC=0", out)

    def test_windows_uses_strings_fallback_when_llvm_strings_absent(self):
        def fake(argv, cwd=None, env=None):
            self.assertEqual(argv[0], "strings")
            return _fake_run_result(returncode=0, stdout=_HAPPY_STRINGS_TEXT)

        with mock.patch.object(orientation, "shutil") as fake_shutil, \
             mock.patch.object(run_module, "run", side_effect=fake), \
             _Cwd(self._tmp()):
            fake_shutil.which.side_effect = lambda name: "/usr/bin/strings" if name == "strings" else None
            rc, out, _ = _run_captured(orientation._strings_scan, "windows", _WIN_DLL, "dll_strings.txt")
        self.assertEqual(rc, 0)
        self.assertIn("STRINGS_TOOL=strings STRINGS_RC=0", out)

    def test_emission_matches_golden_linux(self):
        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout=_HAPPY_STRINGS_TEXT)

        with mock.patch.object(orientation, "shutil") as fake_shutil, \
             mock.patch.object(run_module, "run", side_effect=fake), \
             _Cwd(self._tmp()):
            fake_shutil.which.side_effect = lambda name: f"/usr/bin/{name}" if name == "llvm-strings" else None
            rc, out, _ = _run_captured(orientation._strings_scan, "linux", _LINUX_SO, "so_strings.txt")
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "assert-orientation-linux.markers").read_text()
        self.assertEqual(out, expected)

    def test_emission_matches_golden_windows(self):
        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout=_HAPPY_STRINGS_TEXT)

        with mock.patch.object(orientation, "shutil") as fake_shutil, \
             mock.patch.object(run_module, "run", side_effect=fake), \
             _Cwd(self._tmp()):
            fake_shutil.which.side_effect = lambda name: f"/usr/bin/{name}" if name == "llvm-strings" else None
            rc, out, _ = _run_captured(orientation._strings_scan, "windows", _WIN_DLL, "dll_strings.txt")
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "assert-orientation-windows.markers").read_text()
        self.assertEqual(out, expected)

    # ---- android (genuinely different shape) ---------------------------

    def test_android_symbol_check_is_substring_not_wholeline(self):
        """PORTED AS-IS: Android's real shell uses `grep -q` (substring),
        not `grep -qx`, for the symbol check only -- a symbol name embedded
        inside a longer nm line must still PASS on Android, unlike
        linux/windows."""
        nm_text = "0000000000001234 T __android_dng_render_stage4_split_wrapper\n"
        strings_text = "orient_a_x\norient_b_x\norient_c_x\norient_a_y\norient_b_y\norient_c_y\n"

        def fake(argv, cwd=None, env=None):
            joined = " ".join(map(str, argv))
            if "llvm-nm" in joined:
                return _fake_run_result(returncode=0, stdout=nm_text)
            if argv[0] == "strings":
                return _fake_run_result(returncode=0, stdout=strings_text)
            raise AssertionError(f"unexpected argv: {argv}")

        with mock.patch.dict(os.environ, {"ANDROID_NDK_HOME": "/opt/ndk"}), \
             mock.patch.object(run_module, "run", side_effect=fake), \
             _Cwd(self._tmp()):
            rc, out, _ = _run_captured(orientation._android_scan, "native/build-android/libdng_decoder_native.so", "android_so_strings.txt")
        # dng_render_stage4_split is present only as a SUBSTRING of the nm
        # line; dng_render_stage4 also matches as a substring of the same
        # line. Both must PASS under Android's real (substring) semantics.
        self.assertEqual(rc, 0, "substring match must pass on Android's real symbol check")

    def test_android_missing_signals_use_android_wording(self):
        nm_text = "0000000000001234 T some_unrelated_symbol\n"
        strings_text = "orient_a_x\n"  # 5 coeffs missing

        def fake(argv, cwd=None, env=None):
            joined = " ".join(map(str, argv))
            if "llvm-nm" in joined:
                return _fake_run_result(returncode=0, stdout=nm_text)
            if argv[0] == "strings":
                return _fake_run_result(returncode=0, stdout=strings_text)
            raise AssertionError(f"unexpected argv: {argv}")

        with mock.patch.dict(os.environ, {"ANDROID_NDK_HOME": "/opt/ndk"}), \
             mock.patch.object(run_module, "run", side_effect=fake), \
             _Cwd(self._tmp()):
            rc, out, err = _run_captured(orientation._android_scan, "native/build-android/libdng_decoder_native.so", "android_so_strings.txt")
        self.assertEqual(rc, 1)
        self.assertIn("NM_RC=0 STRINGS_RC=0", out)
        self.assertIn("symbol:dng_render_stage4", err)
        self.assertIn("symbol:dng_render_stage4_split", err)

    def test_android_tool_failure_error_text_is_verbatim(self):
        def fake(argv, cwd=None, env=None):
            joined = " ".join(map(str, argv))
            if "llvm-nm" in joined:
                return _fake_run_result(returncode=1, stdout="", stderr="not found\n")
            return _fake_run_result(returncode=0, stdout="orient_a_x\n")

        with mock.patch.dict(os.environ, {"ANDROID_NDK_HOME": "/opt/ndk"}), \
             mock.patch.object(run_module, "run", side_effect=fake), \
             _Cwd(self._tmp()):
            rc, out, err = _run_captured(orientation._android_scan, "native/build-android/libdng_decoder_native.so", "android_so_strings.txt")
        self.assertEqual(rc, 1)
        self.assertIn(
            "::error::llvm-nm or strings failed reading "
            "native/build-android/libdng_decoder_native.so; capability is UNVERIFIED, "
            "refusing to publish.",
            err,
        )

    def test_emission_matches_golden_android(self):
        nm_text = "0000000000001234 T dng_render_stage4\n0000000000001240 T dng_render_stage4_split\n"
        strings_text = "orient_a_x\norient_b_x\norient_c_x\norient_a_y\norient_b_y\norient_c_y\n"

        def fake(argv, cwd=None, env=None):
            joined = " ".join(map(str, argv))
            if "llvm-nm" in joined:
                return _fake_run_result(returncode=0, stdout=nm_text)
            if argv[0] == "strings":
                return _fake_run_result(returncode=0, stdout=strings_text)
            raise AssertionError(f"unexpected argv: {argv}")

        with mock.patch.dict(os.environ, {"ANDROID_NDK_HOME": "/opt/ndk"}), \
             mock.patch.object(run_module, "run", side_effect=fake), \
             _Cwd(self._tmp()):
            rc, out, _ = _run_captured(orientation._android_scan, "native/build-android/libdng_decoder_native.so", "android_so_strings.txt")
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "assert-orientation-android.markers").read_text()
        self.assertEqual(out, expected)

    # ---- macOS (compiled probe) ------------------------------------------

    def test_macos_uses_compiled_probe_not_strings(self):
        def fake(argv, cwd=None, env=None):
            if argv[0] == "cmake":
                return _fake_run_result(returncode=0, stdout="Build complete\n")
            return _fake_run_result(returncode=0, stdout="ORIENT_CAPABILITY_PROBE: all present\n")

        with mock.patch.dict(os.environ, {"DYLIB": "/tmp_build/arm64/libdng_decoder_native.dylib"}), \
             mock.patch.object(run_module, "run", side_effect=fake), \
             mock.patch.object(orientation, "_strings_scan") as fake_scan, \
             _Cwd(self._tmp()):
            rc, out, _ = _run_captured(orientation.assert_orientation, "macos")
        self.assertEqual(rc, 0)
        fake_scan.assert_not_called()

    def test_macos_probe_failure_returns_1(self):
        """C-G4 item 3, PORTED AS-IS: exit literal 1 on failure, not the
        probe's own (possibly informative) nonzero rc."""

        def fake(argv, cwd=None, env=None):
            if argv[0] == "cmake":
                return _fake_run_result(returncode=0, stdout="Build complete\n")
            return _fake_run_result(returncode=17, stdout="PROBE_RESULT=missing:dng_render_stage4_metadata\n")

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, err = _run_captured(
                orientation._compiled_probe, "macos", "/tmp_build/arm64/libdng_decoder_native.dylib"
            )
        self.assertEqual(rc, 1, "the wrapper must return 1, not the probe's rc=17")
        self.assertIn("ORIENT_CAPABILITY_PROBE_RC=17", out)
        self.assertIn("does not export the fused-orientation kernel(s)", err)

    def test_emission_matches_golden_macos(self):
        def fake(argv, cwd=None, env=None):
            if argv[0] == "cmake":
                return _fake_run_result(returncode=0, stdout="Build complete\n")
            return _fake_run_result(returncode=0, stdout="ORIENT_CAPABILITY_PROBE: all present\n")

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(
                orientation._compiled_probe, "macos", "/tmp_build/arm64/libdng_decoder_native.dylib"
            )
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "assert-orientation-macos.markers").read_text()
        self.assertEqual(out, expected)


if __name__ == "__main__":
    unittest.main()
