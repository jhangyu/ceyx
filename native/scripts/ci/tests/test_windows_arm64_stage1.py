"""windows-arm64 leg (R-9): assert_vs_component + cross_stage1."""
from __future__ import annotations

import io
import os
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import run as run_module
from .. import windows_toolchain as wt


def _res(rc=0, out=""):
    return run_module.RunResult(argv=[], returncode=rc, stdout=out, stderr="")


class _Cwd:
    def __enter__(self):
        self.old = os.getcwd()
        self.tmp = tempfile.mkdtemp()
        os.chdir(self.tmp)
        return Path(self.tmp)

    def __exit__(self, *_):
        os.chdir(self.old)


def _cap(fn, *a):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = fn(*a)
    return rc, out.getvalue(), err.getvalue()


class AssertVsComponent(unittest.TestCase):
    def test_present(self):
        with _Cwd(), mock.patch.object(run_module, "run", return_value=_res(0, "C:\\VS\\Enterprise\n")):
            rc, out, _ = _cap(wt.assert_vs_component, "Microsoft.VisualStudio.Component.VC.Tools.ARM64")
        self.assertEqual(rc, 0)
        self.assertIn("VSWHERE_RC=0", out)

    def test_empty_answer_is_a_failure_even_with_rc0(self):
        # vswhere exits 0 with no output when nothing matches -requires.
        with _Cwd(), mock.patch.object(run_module, "run", return_value=_res(0, "")):
            rc, _, err = _cap(wt.assert_vs_component, "Microsoft.VisualStudio.Component.VC.Tools.ARM64")
        self.assertEqual(rc, 1)
        self.assertIn("no Visual Studio instance provides", err)


class CrossStage1(unittest.TestCase):
    def test_passes_x64_target_and_aot_override_and_checks_output(self):
        calls = []

        def fake(argv, cwd=None, env=None):
            calls.append((argv, env))
            if "--build" in argv:
                Path("hostgen/halide_generated").mkdir(parents=True, exist_ok=True)
                Path("hostgen/halide_generated/halide_runtime.lib").write_bytes(b"!<arch>")
            return _res(0, "ok\n")

        with _Cwd(), mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _cap(wt.cross_stage1, "hostgen", "arm-64-windows-vulkan-no_asserts")
        self.assertEqual(rc, 0)
        configure, env = calls[0]
        self.assertIn("-DDNG_HOST_GENERATORS_ONLY=ON", configure)
        self.assertIn("-DDNG_AOT_TARGET_OVERRIDE=arm-64-windows-vulkan-no_asserts", configure)
        self.assertEqual(env["CXXFLAGS"], "--target=x86_64-pc-windows-msvc")
        self.assertEqual(env["CFLAGS"], "--target=x86_64-pc-windows-msvc")
        # never -DCMAKE_CXX_FLAGS: it would replace CMake's MSVC defaults (N16)
        self.assertFalse([a for a in configure if a.startswith("-DCMAKE_CXX_FLAGS")])
        self.assertIn("CROSS_STAGE1_RC=0", out)

    def test_green_build_with_empty_aot_dir_is_red(self):
        with _Cwd(), mock.patch.object(run_module, "run", return_value=_res(0, "")):
            rc, _, err = _cap(wt.cross_stage1, "hostgen", "arm-64-windows")
        self.assertEqual(rc, 1)
        self.assertIn("is empty", err)

    def test_configure_failure_stops_before_build(self):
        calls = []

        def fake(argv, cwd=None, env=None):
            calls.append(argv)
            return _res(1, "CMake Error\n")

        with _Cwd(), mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _cap(wt.cross_stage1, "hostgen", "arm-64-windows")
        self.assertEqual(rc, 1)
        self.assertEqual(len(calls), 1)
        self.assertIn("CROSS_STAGE1_CONFIGURE_RC=1", out)


if __name__ == "__main__":
    unittest.main()
