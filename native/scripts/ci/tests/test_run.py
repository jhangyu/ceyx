"""Tests for native/scripts/ci/run.py — WI-2."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))  # native/scripts/

from ci import run  # noqa: E402


class TestRunBasics(unittest.TestCase):
    def test_capture_returns_text(self) -> None:
        rc, text = run.capture([sys.executable, "-c", "print('hello')"])
        self.assertEqual(rc, 0)
        self.assertEqual(text, "hello\n")

    def test_nonzero_child_returns_its_rc_without_raising(self) -> None:
        result = run.run([sys.executable, "-c", "import sys; sys.exit(3)"])
        self.assertEqual(result.returncode, 3)

    def test_stdout_and_stderr_are_captured_separately(self) -> None:
        result = run.run([
            sys.executable, "-c",
            "import sys; sys.stdout.write('out'); sys.stderr.write('err')",
        ])
        self.assertEqual(result.stdout, "out")
        self.assertEqual(result.stderr, "err")


class TestMissingExecutable(unittest.TestCase):
    def test_missing_executable_is_127_not_exception(self) -> None:
        result = run.run(["definitely-not-a-real-executable-xyz"])
        self.assertEqual(result.returncode, 127)
        self.assertNotEqual(result.stderr, "")


class TestRunToFile(unittest.TestCase):
    def test_run_to_file_writes_combined_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_path = Path(tmp) / "out.txt"
            result = run.run_to_file(
                [sys.executable, "-c", "import sys; sys.stdout.write('a'); sys.stderr.write('b')"],
                out_path,
            )
            self.assertEqual(result.returncode, 0)
            self.assertEqual(out_path.read_text(encoding="utf-8"), "ab")


class TestArgvDiscipline(unittest.TestCase):
    """Negative-test fixtures for the shell/argv discipline the module's
    docstring claims. These strings are deliberately shaped like violations
    but live in a test_*.py file, which the AST lint in
    native/scripts/deps/test_no_shell_lint.py explicitly exempts (it skips
    files whose name starts with test_) because they are fixture data for
    THIS test, not production code."""

    def test_run_never_uses_shell_true_literal(self) -> None:
        import ast

        run_py = Path(__file__).resolve().parents[1] / "run.py"
        tree = ast.parse(run_py.read_text(encoding="utf-8"), filename=str(run_py))
        for node in ast.walk(tree):
            if isinstance(node, ast.Call):
                for keyword in node.keywords:
                    if keyword.arg == "shell":
                        self.assertFalse(
                            isinstance(keyword.value, ast.Constant) and keyword.value.value is True,
                            "run.py must never pass shell=True",
                        )

    def test_report_module_does_not_import_subprocess(self) -> None:
        # report.py (WI-2's other file) must stay a leaf module with no
        # subprocess dependency; run.py is the package's sole subprocess
        # importer among the WI-2-owned files (pre-existing sibling scripts
        # in ci/, e.g. check_cmake_sources_tracked.py, predate this
        # migration and are out of WI-2's scope).
        report_py = Path(__file__).resolve().parents[1] / "report.py"
        self.assertNotIn("import subprocess", report_py.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
