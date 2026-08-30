"""Unit tests for native/scripts/deps/run.py (D3 round 1, A3.1/A3.2/A3.4/A3.5).

Run with: python3 -m pytest native/scripts/deps/test_run.py -v
(or python3 native/scripts/deps/test_run.py to run standalone via unittest).
"""
from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import run as run_module  # noqa: E402


class TestRunArgvDiscipline(unittest.TestCase):
    """A3.1: subprocess is always called with a list, shell=False."""

    def test_rejects_string_argv(self) -> None:
        with self.assertRaises(TypeError):
            run_module.run("echo hello")  # type: ignore[arg-type]

    def test_rejects_bytes_argv(self) -> None:
        with self.assertRaises(TypeError):
            run_module.run(b"echo hello")  # type: ignore[arg-type]

    def test_rejects_non_str_element(self) -> None:
        with self.assertRaises(TypeError):
            run_module.run(["echo", 123])  # type: ignore[list-item]

    def test_rejects_empty_argv(self) -> None:
        with self.assertRaises(ValueError):
            run_module.run([])

    def test_never_sets_shell_true(self) -> None:
        with mock.patch.object(run_module.subprocess, "run") as mock_run:
            mock_run.return_value = subprocess.CompletedProcess(
                args=["true"], returncode=0, stdout="", stderr=""
            )
            run_module.run(["true"])
            self.assertIn("shell", mock_run.call_args.kwargs)
            self.assertIs(mock_run.call_args.kwargs["shell"], False)


class TestRunPipeDiscipline(unittest.TestCase):
    """A3.2: no `grep` in any argv list; no `|` in any argv element."""

    def test_rejects_pipe_in_element(self) -> None:
        with self.assertRaises(ValueError):
            run_module.run(["sh", "-c", "echo hi | grep hi"])

    def test_rejects_pipe_only_element(self) -> None:
        with self.assertRaises(ValueError):
            run_module.run(["cmd", "arg|withpipe"])


class TestRunExitCodeDiscipline(unittest.TestCase):
    """A3.5: exit codes come from CompletedProcess.returncode; a
    non-zero tool exit propagates and is not swallowed."""

    def test_nonzero_exit_raises_with_check_true(self) -> None:
        with self.assertRaises(run_module.SubprocessError) as ctx:
            run_module.run([sys.executable, "-c", "import sys; sys.exit(3)"])
        self.assertEqual(ctx.exception.returncode, 3)

    def test_nonzero_exit_does_not_raise_with_check_false(self) -> None:
        result = run_module.run(
            [sys.executable, "-c", "import sys; sys.exit(7)"], check=False
        )
        self.assertEqual(result.returncode, 7)

    def test_zero_exit_returns_completed_process(self) -> None:
        result = run_module.run([sys.executable, "-c", "import sys; sys.exit(0)"])
        self.assertEqual(result.returncode, 0)

    def test_argv_passes_through_byte_exact(self) -> None:
        # A drive-letter-bearing-looking path and a slash-flag token must
        # reach the child process character-for-character -- this is the
        # structural guarantee that replaces the MSYS re-interpretation
        # this module exists to make impossible (handoff §A).
        payload = "D:/a/ceyx/build/probe.exe"
        flag = "/DWIN32"
        result = run_module.run(
            [sys.executable, "-c", "import sys; print(repr(sys.argv[1:]))", payload, flag]
        )
        self.assertEqual(result.stdout.strip(), repr([payload, flag]))


class TestNativeWindowsInterpreterAssertion(unittest.TestCase):
    """A3.4: on Windows, reject an MSYS/Cygwin Python interpreter."""

    def test_noop_on_non_windows(self) -> None:
        with mock.patch.object(run_module.os, "name", "posix"):
            run_module.assert_native_windows_interpreter()  # must not raise

    def test_rejects_msys_style_executable_path(self) -> None:
        with mock.patch.object(run_module.os, "name", "nt"), mock.patch.object(
            run_module.sys, "executable", "C:/msys64/usr/bin/python3.exe"
        ):
            with self.assertRaises(RuntimeError):
                run_module.assert_native_windows_interpreter()

    def test_rejects_executable_with_no_drive_letter(self) -> None:
        with mock.patch.object(run_module.os, "name", "nt"), mock.patch.object(
            run_module.sys, "executable", "/usr/bin/python3"
        ):
            with self.assertRaises(RuntimeError):
                run_module.assert_native_windows_interpreter()

    def test_accepts_native_windows_executable_path(self) -> None:
        with mock.patch.object(run_module.os, "name", "nt"), mock.patch.object(
            run_module.sys, "executable", "C:\\Python311\\python.exe"
        ):
            run_module.assert_native_windows_interpreter()  # must not raise

    def test_rejects_msys_platform_even_when_os_name_reports_posix(self) -> None:
        # Round-1 review finding F1: MSYS2/Cygwin CPython reports
        # os.name == "posix" (it's a POSIX-emulation layer), so the check
        # must not be gated on os.name == "nt" -- it must fire on
        # sys.platform == "msys"/"cygwin" regardless of os.name.
        with mock.patch.object(run_module.os, "name", "posix"), mock.patch.object(
            run_module.sys, "platform", "msys"
        ):
            with self.assertRaises(RuntimeError):
                run_module.assert_native_windows_interpreter()

    def test_rejects_cygwin_platform_even_when_os_name_reports_posix(self) -> None:
        with mock.patch.object(run_module.os, "name", "posix"), mock.patch.object(
            run_module.sys, "platform", "cygwin"
        ):
            with self.assertRaises(RuntimeError):
                run_module.assert_native_windows_interpreter()

    def test_accepts_real_posix_platform(self) -> None:
        # A genuine macOS/Linux interpreter (os.name == "posix",
        # sys.platform == "darwin"/"linux") must not be rejected.
        with mock.patch.object(run_module.os, "name", "posix"), mock.patch.object(
            run_module.sys, "platform", "darwin"
        ):
            run_module.assert_native_windows_interpreter()  # must not raise


class TestRunCallsInterpreterAssertion(unittest.TestCase):
    """Related F1 note: run() must call the assertion itself so a direct
    importer cannot bypass the startup check that build_deps.py performs."""

    def test_run_rejects_msys_platform_without_explicit_startup_call(self) -> None:
        with mock.patch.object(run_module.sys, "platform", "msys"):
            with self.assertRaises(RuntimeError):
                run_module.run(["true"])


if __name__ == "__main__":
    unittest.main()
