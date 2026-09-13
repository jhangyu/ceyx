"""Unit tests for native/scripts/ci/windows_toolchain.py (WI-24).

Every sub-check is exercised with a FAKE tool output (no real `clang-cl`/
`ls` invocation, no real filesystem probe of `/c/Program Files/...`) via
patches on `shutil.which`, `os.access` and `run.run` -- the same fake-tool
convention `test_verify_artifact.py` uses."""

from __future__ import annotations

import io
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import run as run_module
from .. import windows_toolchain


def _fake_run_result(returncode=0, stdout="", stderr=""):
    return run_module.RunResult(argv=[], returncode=returncode, stdout=stdout, stderr=stderr)


def _run_captured(func, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = func(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


class LocateClangClTests(unittest.TestCase):
    def test_found_on_path_returns_version_rc(self):
        def fake_run(argv, cwd=None, env=None):
            self.assertEqual(argv, ["/usr/bin/clang-cl", "--version"])
            return _fake_run_result(returncode=0, stdout="clang version 18.0.0\n")

        with mock.patch("shutil.which", return_value="/usr/bin/clang-cl"), mock.patch.object(
            run_module, "run", side_effect=fake_run
        ):
            rc, out, err = _run_captured(windows_toolchain.locate_clang_cl)
        self.assertEqual(rc, 0)
        self.assertIn("clang-cl: /usr/bin/clang-cl", out)
        self.assertIn("clang version 18.0.0", out)
        self.assertEqual(err, "")

    def test_version_nonzero_rc_propagates(self):
        def fake_run(argv, cwd=None, env=None):
            return _fake_run_result(returncode=3, stdout="", stderr="boom")

        with mock.patch("shutil.which", return_value="/usr/bin/clang-cl"), mock.patch.object(
            run_module, "run", side_effect=fake_run
        ):
            rc, out, err = _run_captured(windows_toolchain.locate_clang_cl)
        self.assertEqual(rc, 3)
        self.assertIn("boom", out)

    def test_fallback_path_appends_github_path_verbatim(self):
        github_path = Path(self._tmp()) / "github_path.txt"
        github_path.write_text("")

        def fake_run(argv, cwd=None, env=None):
            self.assertEqual(argv[0], "/c/Program Files/LLVM/bin/clang-cl.exe")
            return _fake_run_result(returncode=0, stdout="clang version 18.0.0\n")

        with mock.patch("shutil.which", return_value=None), mock.patch(
            "os.access", return_value=True
        ), mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, err = _run_captured(
                windows_toolchain.locate_clang_cl, github_path=str(github_path)
            )
        self.assertEqual(rc, 0)
        # Single backslashes, not doubled -- see the module's own docstring
        # on the shell's `\\` -> `\` double-quote collapse.
        self.assertEqual(github_path.read_text(), "C:\\Program Files\\LLVM\\bin\n")
        self.assertIn("clang-cl: /c/Program Files/LLVM/bin/clang-cl.exe", out)

    def test_fallback_without_github_path_returns_1_and_does_not_touch_disk(self):
        with mock.patch("shutil.which", return_value=None), mock.patch(
            "os.access", return_value=True
        ), mock.patch.object(run_module, "run") as fake_run:
            rc, out, err = _run_captured(windows_toolchain.locate_clang_cl, github_path=None)
        self.assertEqual(rc, 1)
        self.assertIn("::error::", err)
        fake_run.assert_not_called()

    def test_not_found_anywhere_returns_1(self):
        with mock.patch("shutil.which", return_value=None), mock.patch(
            "os.access", return_value=False
        ), mock.patch.object(run_module, "run") as fake_run:
            rc, out, err = _run_captured(windows_toolchain.locate_clang_cl)
        self.assertEqual(rc, 1)
        self.assertIn("clang-cl not found on the runner", err)
        fake_run.assert_not_called()

    def _tmp(self):
        import tempfile

        return tempfile.mkdtemp()


class VerifyVulkanLibTests(unittest.TestCase):
    def _tmp(self):
        import tempfile

        return Path(tempfile.mkdtemp())

    def test_present_prints_sdk_and_returns_ls_rc(self):
        sdk = self._tmp()
        (sdk / "Lib").mkdir()
        (sdk / "Lib" / "vulkan-1.lib").write_bytes(b"\x00")

        def fake_run(argv, cwd=None, env=None):
            self.assertEqual(argv, ["ls", "-la", str(sdk / "Lib" / "vulkan-1.lib")])
            return _fake_run_result(returncode=0, stdout="-rw-r--r-- 1 vulkan-1.lib\n")

        with mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, err = _run_captured(windows_toolchain.verify_vulkan_lib, str(sdk))
        self.assertEqual(rc, 0)
        self.assertIn(f"VULKAN_SDK={sdk}", out)
        self.assertIn("vulkan-1.lib", out)
        self.assertEqual(err, "")

    def test_missing_prints_error_and_returns_1_regardless_of_ls_rc(self):
        sdk = self._tmp()
        (sdk / "Lib").mkdir()

        def fake_run(argv, cwd=None, env=None):
            self.assertEqual(argv, ["ls", "-la", str(sdk / "Lib")])
            # PORTED AS-IS: the shell's `|| true` swallows this RC entirely.
            return _fake_run_result(returncode=2, stdout="")

        with mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, err = _run_captured(windows_toolchain.verify_vulkan_lib, str(sdk))
        self.assertEqual(rc, 1)
        self.assertIn("vulkan-1.lib missing under VULKAN_SDK", err)

    def test_unset_prints_placeholder_and_fails_closed(self):
        with mock.patch.object(run_module, "run") as fake_run:
            fake_run.return_value = _fake_run_result(returncode=2, stdout="")
            rc, out, err = _run_captured(windows_toolchain.verify_vulkan_lib, "")
        self.assertEqual(rc, 1)
        self.assertIn("VULKAN_SDK=<unset>", out)


if __name__ == "__main__":
    unittest.main()
