"""Tests for native/scripts/ci.py (WI-1)."""
from __future__ import annotations

import importlib
import io
import os
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

# Reentrancy guard for TestSelftest below: `ci.py selftest` discovers and
# runs every test under native/scripts/ci/tests/, INCLUDING this file. A
# test in this class that invokes `main(["selftest"])` in-process would
# otherwise trigger that discovery again, which finds this same test again,
# which invokes selftest again -- unbounded recursion. The guard makes the
# nested invocation's copy of this test a no-op instead of a second real run.
_SELFTEST_REENTRANCY_ENV = "_CI_TEST_DISPATCH_SELFTEST_NESTED"

CI_PY_DIR = Path(__file__).resolve().parents[2]  # native/scripts/
if str(CI_PY_DIR) not in sys.path:
    sys.path.insert(0, str(CI_PY_DIR))

# Q5: `native/scripts/ci.py` and the package `native/scripts/ci/` share a
# name, and the package wins `import ci` (a package directory takes
# precedence over a same-named module in the path finder). To exercise the
# ENTRY POINT specifically -- not the package -- load it directly from its
# file path under a distinct module name.
import importlib.util

_spec = importlib.util.spec_from_file_location(
    "ci_entrypoint_under_test", CI_PY_DIR / "ci.py"
)
ci_entrypoint = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ci_entrypoint)


FROZEN_CLI_SURFACE = {
    "selftest",
    "marker-diff",
    "verify-artifact",
    "import-closure",
    "min-runtime",
    "assert-exports",
    "assert-no-avx512",
    "assert-orientation",
    "codec-probe",
    "capability-vector",
    "stage",
    "assert-staged-group",
    "dt-needed",
    "vcpkg-baseline",
    "vcpkg-bootstrap",
    "vcpkg-install",
    "assert-vcpkg-artefacts",
    "verify-interpreter",
    "ensure-cmake",
    "build-zlib",
}


class TestFrozenCliSurface(unittest.TestCase):
    def test_frozen_cli_surface(self):
        parser = ci_entrypoint.build_parser()
        sub_actions = [
            a for a in parser._subparsers._group_actions if hasattr(a, "choices")
        ]
        names = set(sub_actions[0].choices.keys())
        self.assertEqual(names, FROZEN_CLI_SURFACE)


class TestSelftest(unittest.TestCase):
    def test_selftest_exits_0(self):
        if os.environ.get(_SELFTEST_REENTRANCY_ENV):
            self.skipTest("nested selftest invocation (reentrancy guard)")
            return
        os.environ[_SELFTEST_REENTRANCY_ENV] = "1"
        try:
            rc = ci_entrypoint.main(["selftest"])
        finally:
            del os.environ[_SELFTEST_REENTRANCY_ENV]
        self.assertEqual(rc, 0)

    def test_selftest_prints_summary_line(self):
        if os.environ.get(_SELFTEST_REENTRANCY_ENV):
            self.skipTest("nested selftest invocation (reentrancy guard)")
            return
        os.environ[_SELFTEST_REENTRANCY_ENV] = "1"
        buf = io.StringIO()
        try:
            with redirect_stdout(buf):
                ci_entrypoint.main(["selftest"])
        finally:
            del os.environ[_SELFTEST_REENTRANCY_ENV]
        self.assertRegex(buf.getvalue(), r"CI-SELFTEST: tests=\d+ failures=0 errors=0")


class TestArchGate(unittest.TestCase):
    def test_min_runtime_windows_with_arch_is_argparse_error(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(["min-runtime", "--platform", "windows", "--arch", "x86_64"])
        self.assertEqual(ctx.exception.code, 2)

    def test_min_runtime_macos_without_arch_is_argparse_error(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(["min-runtime", "--platform", "macos"])
        self.assertEqual(ctx.exception.code, 2)

    def test_min_runtime_macos_with_arch_is_accepted_by_argparse(self):
        # Not yet implemented (P0), but must get PAST arg parsing to the
        # "not implemented" path, not fail as an argparse error.
        rc = ci_entrypoint.main(
            ["min-runtime", "--platform", "macos", "--arch", "arm64"]
        )
        self.assertEqual(rc, 2)  # _not_yet(), not an argparse SystemExit


class TestPackageImportResolution(unittest.TestCase):
    def test_package_import_resolves_to_package_not_entrypoint(self):
        # native/scripts/ci.py and the package native/scripts/ci/ share a
        # name; a package directory takes precedence over a same-named
        # module in the path finder (Q5). `import ci.targets` must resolve
        # into the PACKAGE, not fail because `ci` resolved to the entry
        # point module (which has no `targets` attribute of its own).
        mod = importlib.import_module("ci.targets")
        self.assertTrue(hasattr(mod, "TARGETS"))
        self.assertTrue(mod.__file__.endswith("targets.py"))


if __name__ == "__main__":
    unittest.main()
