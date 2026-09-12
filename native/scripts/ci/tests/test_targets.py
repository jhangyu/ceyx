"""Tests for native/scripts/ci/targets.py (WI-1)."""
from __future__ import annotations

import ast
import unittest
from pathlib import Path

import ci.targets as targets

TARGETS_PY = Path(__file__).resolve().parents[1] / "targets.py"


class TestKeyUniformity(unittest.TestCase):
    def test_every_platform_has_every_required_key(self):
        for name, entry in targets.TARGETS.items():
            missing = [k for k in targets.REQUIRED_KEYS if k not in entry]
            self.assertEqual(missing, [], f"platform {name!r} missing keys {missing}")

    def test_no_platform_has_unexpected_keys(self):
        for name, entry in targets.TARGETS.items():
            extra = [k for k in entry if k not in targets.REQUIRED_KEYS]
            self.assertEqual(extra, [], f"platform {name!r} has unexpected keys {extra}")


class TestProvenance(unittest.TestCase):
    def test_module_docstring_cites_file_line_provenance(self):
        text = TARGETS_PY.read_text()
        # Every documented key must cite at least one "<file>:<line>"-shaped
        # reference in the module's header comment block.
        import re

        self.assertTrue(
            re.search(r"\.ya?ml:\d+", text),
            "targets.py header must cite workflow file:line provenance",
        )

    def test_no_import_subprocess(self):
        text = TARGETS_PY.read_text()
        tree = ast.parse(text)
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                for alias in node.names:
                    self.assertNotEqual(alias.name, "subprocess")
            if isinstance(node, ast.ImportFrom):
                self.assertNotEqual(node.module, "subprocess")


class TestSpec(unittest.TestCase):
    def test_unknown_platform_raises_keyerror_naming_known_list(self):
        with self.assertRaises(KeyError) as ctx:
            targets.spec("beos")
        msg = str(ctx.exception)
        self.assertIn("beos", msg)
        for name in targets.platform_names():
            self.assertIn(name, msg)

    def test_macos_requires_arch(self):
        self.assertTrue(targets.spec("macos")["requires_arch"])

    def test_platform_requiring_arch_without_arch_is_flagged_in_data(self):
        # targets.py states the fact only; enforcement is ci.py's job (C-G9).
        # This test pins that the DATA distinguishes macos from every other
        # platform, which is the precondition ci.py's enforcement depends on.
        non_macos_requires_arch = [
            name
            for name, entry in targets.TARGETS.items()
            if name != "macos" and entry["requires_arch"]
        ]
        self.assertEqual(non_macos_requires_arch, [])


if __name__ == "__main__":
    unittest.main()
