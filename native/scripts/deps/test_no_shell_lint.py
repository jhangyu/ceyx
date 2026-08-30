"""AST-based lint: no `shell=True` and no bare-string subprocess argv
anywhere under native/scripts/, and no `grep` token in any argv-shaped
list literal. Spec A3.1/A3.2.

This is deliberately demonstrated red as part of the test itself
(TestLintCatchesViolations) so the check is proven to actually fire,
per spec §5's "each lint demonstrated red" convention applied to D3.

Run with: python3 -m pytest native/scripts/deps/test_no_shell_lint.py -v
"""
from __future__ import annotations

import ast
import unittest
from pathlib import Path
from typing import List

SCRIPTS_ROOT = Path(__file__).resolve().parents[1]  # native/scripts/


def _iter_python_files(root: Path):
    for path in root.rglob("*.py"):
        # Skip vendored/third-party trees and this module's own __pycache__.
        # `tmp/` is native/scripts/tmp/, the project's scratch directory
        # (CLAUDE.md "Subprocess / Bash 執行規範": scratch scripts and fetched
        # build byproducts live there, never /tmp/) -- it can contain
        # third-party fetched sources (e.g. a vendored Python-2 test script)
        # that are not part of this project's own code and are not subject
        # to this project's Python-3/no-shell rules.
        # Skip our own test files: they deliberately contain bad-pattern
        # fixtures (bare-string argv, pipe/grep tokens) as negative-test
        # data for the OTHER tests in this package (e.g. test_run.py's
        # TestRunArgvDiscipline/TestRunPipeDiscipline feed exactly these
        # strings to run.run() to assert it rejects them). This scan is
        # for production code under native/scripts/, not test fixtures.
        parts = path.parts
        if "__pycache__" in parts or "tmp" in parts:
            continue
        if path.name.startswith("test_"):
            continue
        yield path


def find_violations(source: str, filename: str = "<string>") -> List[str]:
    """Return a list of human-readable violation strings for `source`."""
    violations: List[str] = []
    tree = ast.parse(source, filename=filename)
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            for keyword in node.keywords:
                if keyword.arg == "shell" and isinstance(keyword.value, ast.Constant) and keyword.value.value is True:
                    violations.append(f"{filename}:{node.lineno}: shell=True literal")
            # subprocess.run/Popen/call/check_call/check_output first positional
            # argument must not be a bare string.
            func = node.func
            func_name = getattr(func, "attr", getattr(func, "id", None))
            if func_name in ("run", "Popen", "call", "check_call", "check_output") and node.args:
                first = node.args[0]
                if isinstance(first, ast.Constant) and isinstance(first.value, str):
                    violations.append(f"{filename}:{node.lineno}: subprocess argv is a bare string, not a list")
        if isinstance(node, (ast.List, ast.Tuple)):
            for element in node.elts:
                if isinstance(element, ast.Constant) and isinstance(element.value, str):
                    if "grep" in element.value or "|" in element.value:
                        violations.append(
                            f"{filename}:{node.lineno}: argv-shaped list element contains 'grep' or '|': {element.value!r}"
                        )
    return violations


class TestNoShellLintAppliesToRepo(unittest.TestCase):
    def test_no_violations_under_native_scripts(self) -> None:
        all_violations: List[str] = []
        for path in _iter_python_files(SCRIPTS_ROOT):
            source = path.read_text(encoding="utf-8")
            all_violations.extend(find_violations(source, str(path)))
        self.assertEqual(all_violations, [], "\n".join(all_violations))


class TestLintCatchesViolations(unittest.TestCase):
    """Demonstrated red: the lint must actually fire on deliberately
    malformed input, not merely pass on innocent code."""

    def test_catches_shell_true(self) -> None:
        source = "import subprocess\nsubprocess.run('echo hi', shell=True)\n"
        violations = find_violations(source, "synthetic.py")
        self.assertTrue(any("shell=True" in v for v in violations), violations)

    def test_catches_bare_string_argv(self) -> None:
        source = "import subprocess\nsubprocess.run('echo hi')\n"
        violations = find_violations(source, "synthetic.py")
        self.assertTrue(any("bare string" in v for v in violations), violations)

    def test_catches_grep_in_argv_list(self) -> None:
        source = "argv = ['sh', '-c', 'echo hi | grep hi']\n"
        violations = find_violations(source, "synthetic.py")
        self.assertTrue(any("grep" in v or "'|'" in v for v in violations), violations)


if __name__ == "__main__":
    unittest.main()
