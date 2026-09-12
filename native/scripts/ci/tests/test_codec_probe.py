"""Unit tests for native/scripts/ci/codec_probe.py (WI-13).

Fake tool outputs only (no real clang/clang-cl compile, no real probe
execution). Three platforms, three divergent shapes -- tested separately,
never assumed identical (see the module's own "falsified premise" note).
"""

from __future__ import annotations

import io
import os
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import codec_probe as cp
from .. import run as run_module
from .. import tools as tools_module

_HAPPY_PROBE_TEXT = "HEVC-ENC=1 AV1-ENC=1 HEVC-DEC=1 AV1-DEC=1\n"

_GOLDEN_DIR = Path(__file__).resolve().parent / "golden" / "expected"


def _fake_result(rc=0, stdout="", stderr=""):
    return run_module.RunResult(argv=[], returncode=rc, stdout=stdout, stderr=stderr)


def _emit(fn, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = fn(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


class _FakeRun:
    """Dispatches on argv[0]'s basename. `responses` maps a basename to
    `(rc, stdout)`; anything unlisted defaults to `(0, "")`."""

    def __init__(self, responses):
        self.responses = dict(responses)
        self.calls = []

    def _lookup(self, argv):
        name = Path(str(argv[0])).name
        return self.responses.get(name, (0, ""))

    def run(self, argv, cwd=None, env=None):
        self.calls.append(("run", list(argv), env))
        rc, out = self._lookup(argv)
        return _fake_result(rc, out)

    def run_to_file(self, argv, out_path, cwd=None, env=None):
        self.calls.append(("run_to_file", list(argv), env))
        rc, out = self._lookup(argv)
        Path(out_path).write_text(out, encoding="utf-8")
        return _fake_result(rc, out)


def _cc_for(platform):
    return {"linux": "clang", "macos": "clang", "windows": "clang-cl"}[platform]


class CodecProbeTests(unittest.TestCase):
    def setUp(self):
        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)
        self.ws = self._tmpdir.name
        Path(self.ws, "native", "scripts", "deps", "probe").mkdir(parents=True)

    def _run(self, platform, responses, dist_dir=None, patch_environ=None):
        if platform == "windows":
            # shutil.copy2 always does real filesystem I/O regardless of the
            # run() fakes, so every windows test needs real DLL fixtures.
            dist = Path(self.ws, "native", "third_party", "heif-dist-windows", "bin")
            if not dist.exists():
                dist.mkdir(parents=True)
                (dist / "heif.dll").write_bytes(b"MZ")
                (dist / "libde265.dll").write_bytes(b"MZ")
        fake = _FakeRun(responses)
        patches = [
            mock.patch.object(run_module, "run", side_effect=fake.run),
            mock.patch.object(run_module, "run_to_file", side_effect=fake.run_to_file),
            mock.patch.object(tools_module, "resolve", return_value=_cc_for(platform)),
        ]
        if patch_environ is not None:
            patches.append(mock.patch.dict(os.environ, patch_environ, clear=True))
        for p in patches:
            p.start()
        self.addCleanup(lambda: [p.stop() for p in patches])
        rc, out, err = _emit(cp.codec_probe, platform, self.ws, dist_dir)
        return rc, out, err, fake

    # ---- C-G1: the sharpest AC ------------------------------------------

    def test_probe_codecs_rc_is_emitted_twice(self):
        rc, out, _err, _fake = self._run(
            "linux",
            {"clang": (0, ""), "file": (0, "ELF 64-bit"), "probe_codecs": (0, _HAPPY_PROBE_TEXT)},
        )
        self.assertEqual(rc, 0)
        self.assertEqual(
            out.count("PROBE_CODECS_RC=0"),
            2,
            "tee prints it once and the cat replay prints it again (linux_build.yml)",
        )

    # ---- windows argv shape (C-G7) ---------------------------------------

    def test_windows_argv_uses_positional_lib_and_no_dash_l(self):
        rc, _out, _err, fake = self._run(
            "windows",
            {
                "clang-cl": (0, ""),
                "probe_codecs.exe": (0, _HAPPY_PROBE_TEXT),
            },
        )
        self.assertEqual(rc, 0)
        compile_calls = [c for c in fake.calls if c[0] == "run" and "clang-cl" in c[1][0]]
        self.assertEqual(len(compile_calls), 1)
        argv = compile_calls[0][1]
        self.assertTrue(any(a.endswith(".lib") for a in argv), argv)
        self.assertFalse(
            [a for a in argv if a.startswith("-L") or a.startswith("-l")], argv
        )
        # No `file` line on windows (divergence 2).
        self.assertFalse([c for c in fake.calls if c[1] and c[1][0] == "file"])

    def test_windows_no_file_line(self):
        rc, out, _err, _fake = self._run(
            "windows",
            {"clang-cl": (0, ""), "probe_codecs.exe": (0, _HAPPY_PROBE_TEXT)},
        )
        self.assertEqual(rc, 0)
        self.assertNotIn("ELF", out)

    def test_windows_copies_both_dlls_beside_probe(self):
        dist = Path(self.ws, "native", "third_party", "heif-dist-windows")
        (dist / "bin").mkdir(parents=True)
        (dist / "bin" / "heif.dll").write_bytes(b"MZ")
        (dist / "bin" / "libde265.dll").write_bytes(b"MZ")
        rc, _out, _err, _fake = self._run(
            "windows",
            {"clang-cl": (0, ""), "probe_codecs.exe": (0, _HAPPY_PROBE_TEXT)},
        )
        self.assertEqual(rc, 0)
        probe_dir = Path(self.ws, "native", "scripts", "deps", "probe")
        self.assertTrue((probe_dir / "heif.dll").exists())
        self.assertTrue((probe_dir / "libde265.dll").exists())

    # ---- linux env (belt-and-braces LD_LIBRARY_PATH) ----------------------

    def test_linux_env_carries_ld_library_path(self):
        rc, _out, _err, fake = self._run(
            "linux",
            {"clang": (0, ""), "file": (0, "ELF"), "probe_codecs": (0, _HAPPY_PROBE_TEXT)},
            patch_environ={"PATH": "/usr/bin"},
        )
        self.assertEqual(rc, 0)
        run_calls = [c for c in fake.calls if c[0] == "run_to_file"]
        self.assertEqual(len(run_calls), 1)
        env = run_calls[0][2]
        self.assertIsNotNone(env)
        expected_prefix = f"{self.ws}/native/third_party/heif-dist-linux/lib"
        self.assertTrue(env["LD_LIBRARY_PATH"].startswith(expected_prefix), env["LD_LIBRARY_PATH"])

    def test_macos_run_env_has_no_library_path_var(self):
        rc, _out, _err, fake = self._run(
            "macos",
            {"clang": (0, ""), "file": (0, "Mach-O"), "probe_codecs": (0, _HAPPY_PROBE_TEXT)},
            dist_dir="native/third_party/heif-dist",
        )
        self.assertEqual(rc, 0)
        run_calls = [c for c in fake.calls if c[0] == "run_to_file"]
        self.assertEqual(len(run_calls), 1)
        env = run_calls[0][2]
        self.assertIsNone(env, "macOS passes env=None -- no library-path var of any kind")

    # ---- file line present on linux+macos, absent on windows -------------

    def test_file_line_emitted_on_linux_and_macos_only(self):
        rc, out, _err, _fake = self._run(
            "linux",
            {"clang": (0, ""), "file": (0, "ELF 64-bit LSB"), "probe_codecs": (0, _HAPPY_PROBE_TEXT)},
        )
        self.assertEqual(rc, 0)
        self.assertIn("ELF 64-bit LSB", out)

        rc2, out2, _err2, _fake2 = self._run(
            "macos",
            {"clang": (0, ""), "file": (0, "Mach-O 64-bit"), "probe_codecs": (0, _HAPPY_PROBE_TEXT)},
            dist_dir="native/third_party/heif-dist",
        )
        self.assertEqual(rc2, 0)
        self.assertIn("Mach-O 64-bit", out2)

        rc3, out3, _err3, _fake3 = self._run(
            "windows",
            {"clang-cl": (0, ""), "probe_codecs.exe": (0, _HAPPY_PROBE_TEXT)},
        )
        self.assertEqual(rc3, 0)
        self.assertNotIn("Mach-O", out3)
        self.assertNotIn("ELF", out3)

    # ---- error line goes to stdout, not stderr ----------------------------

    def test_error_line_goes_to_stdout_not_stderr(self):
        rc, out, err = self._run(
            "linux",
            {"clang": (0, ""), "file": (0, "ELF"), "probe_codecs": (3, "HEVC-ENC=0\n")},
        )[:3]
        self.assertEqual(rc, 3)
        self.assertEqual(err, "", "the original has no >&2 on this echo")
        self.assertIn("::error::probe_codecs exited 3 (nonzero bits = missing capabilities)", out)

    # ---- compiler not found ------------------------------------------------

    def test_compiler_not_found_names_search_list(self):
        with mock.patch.object(
            tools_module, "resolve", side_effect=tools_module.ToolNotFound("no cc tool found; searched: clang")
        ):
            rc, _out, err = _emit(cp.codec_probe, "linux", self.ws)
        self.assertEqual(rc, 2)
        self.assertIn("searched", err)

    # ---- success tail line -------------------------------------------------

    def test_success_tail_line_is_verbatim(self):
        rc, out, _err, _fake = self._run(
            "linux",
            {"clang": (0, ""), "file": (0, "ELF"), "probe_codecs": (0, _HAPPY_PROBE_TEXT)},
        )
        self.assertEqual(rc, 0)
        self.assertIn(
            "A-CAP-HEVC-ENC / A-CAP-AV1-ENC / A-CAP-HEVC-DEC / A-CAP-AV1-DEC all present: RC=0",
            out,
        )

    # ---- macOS --dist-dir plumbing -----------------------------------------

    def test_macos_dist_dir_comes_from_flag_not_targets(self):
        rc, _out, _err, fake = self._run(
            "macos",
            {"clang": (0, ""), "file": (0, "Mach-O"), "probe_codecs": (0, _HAPPY_PROBE_TEXT)},
            dist_dir="native/third_party/heif-dist-x86_64",
        )
        self.assertEqual(rc, 0)
        compile_calls = [c for c in fake.calls if c[0] == "run"]
        argv = compile_calls[0][1]
        self.assertTrue(any("heif-dist-x86_64" in a for a in argv), argv)
        src = Path(cp.__file__).read_text()
        self.assertNotIn("heif_dist_dir", src)

    def test_macos_requires_dist_dir(self):
        rc, _out, err = _emit(cp.codec_probe, "macos", self.ws)
        self.assertEqual(rc, 2)
        self.assertIn("--dist-dir is required for --platform macos", err)

    def test_linux_rejects_dist_dir(self):
        rc, _out, err = _emit(cp.codec_probe, "linux", self.ws, "some/dist")
        self.assertEqual(rc, 2)
        self.assertIn("--dist-dir is not accepted for --platform 'linux'", err)

    def test_windows_rejects_dist_dir(self):
        rc, _out, err = _emit(cp.codec_probe, "windows", self.ws, "some/dist")
        self.assertEqual(rc, 2)
        self.assertIn("--dist-dir is not accepted for --platform 'windows'", err)

    # ---- golden emission parity --------------------------------------------

    def test_emission_matches_golden_linux(self):
        rc, out, _err, _fake = self._run(
            "linux",
            {"clang": (0, ""), "file": (0, "ELF 64-bit"), "probe_codecs": (0, _HAPPY_PROBE_TEXT)},
        )
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "codec-probe-linux.markers").read_text(encoding="utf-8")
        self.assertEqual(out, expected)

    def test_emission_matches_golden_macos(self):
        rc, out, _err, _fake = self._run(
            "macos",
            {"clang": (0, ""), "file": (0, "Mach-O 64-bit"), "probe_codecs": (0, _HAPPY_PROBE_TEXT)},
            dist_dir="native/third_party/heif-dist",
        )
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "codec-probe-macos.markers").read_text(encoding="utf-8")
        self.assertEqual(out, expected)

    def test_emission_matches_golden_windows(self):
        rc, out, _err, _fake = self._run(
            "windows",
            {"clang-cl": (0, ""), "probe_codecs.exe": (0, _HAPPY_PROBE_TEXT)},
        )
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "codec-probe-windows.markers").read_text(encoding="utf-8")
        self.assertEqual(out, expected)


if __name__ == "__main__":
    unittest.main()
