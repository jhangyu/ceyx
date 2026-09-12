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


class TestPushThreeDispatch(unittest.TestCase):
    """End-to-end: invoke through main(argv) exactly as CI/a laptop would,
    not through direct module imports -- a dispatcher verified only through
    imports has never been run (push-2 lesson)."""

    def setUp(self):
        import tempfile

        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)
        self.tmp = Path(self._tmpdir.name)

    def test_stage_and_assert_staged_group_end_to_end(self):
        import ci.targets as targets

        native_dir = self.tmp / "native_out"
        dist_dir_name = targets.spec("linux")["dist_dir"].split("/")[-1]
        dist_dir = native_dir / dist_dir_name
        dist_dir.mkdir(parents=True)
        # Real declared companions for linux, per shipped_files.toml, faked
        # as zero-byte placeholders -- stage() only copies bytes, it never
        # inspects content.
        import read_shipped_files

        entry = read_shipped_files.load_declaration()["linux"]
        for name in [entry["decoder"], *entry["companions"]]:
            (dist_dir / name).write_bytes(b"")

        artifact_dir = self.tmp / "artifact"
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = ci_entrypoint.main(
                [
                    "stage",
                    "--platform",
                    "linux",
                    "--artifact-dir",
                    str(artifact_dir),
                    "--native-dir",
                    str(native_dir),
                ]
            )
        self.assertEqual(rc, 0)
        staged = artifact_dir / "native" / entry["decoder"]
        self.assertTrue(staged.exists())

        buf2 = io.StringIO()
        with redirect_stdout(buf2):
            rc2 = ci_entrypoint.main(
                [
                    "assert-staged-group",
                    "--platform",
                    "linux",
                    "--artifact-dir",
                    str(artifact_dir),
                ]
            )
        self.assertEqual(rc2, 0)
        self.assertIn("ATOMIC_GROUP_COMPLETE=1", buf2.getvalue())
        self.assertIn("SHARED_LIB_COUNT=1", buf2.getvalue())

    def test_stage_missing_required_flags_is_argparse_error_not_traceback(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(["stage", "--platform", "linux"])
        self.assertEqual(ctx.exception.code, 2)

    def test_dt_needed_end_to_end_missing_artifact_is_handled_not_traceback(self):
        # No staged .so present: run.run_to_file/readelf against a missing
        # path must surface as a handled failure through the CLI, not an
        # unhandled exception -- this exercises the real dispatch call shape.
        artifact_dir = self.tmp / "artifact_missing"
        (artifact_dir / "native").mkdir(parents=True)
        runner_temp = self.tmp / "runner_temp"
        runner_temp.mkdir()
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = ci_entrypoint.main(
                [
                    "dt-needed",
                    "--platform",
                    "linux",
                    "--artifact-dir",
                    str(artifact_dir),
                    "--runner-temp",
                    str(runner_temp),
                ]
            )
        self.assertEqual(rc, 1)
        self.assertIn("DT_NEEDED", buf.getvalue())

    def test_verify_artifact_missing_binary_is_handled_end_to_end(self):
        # verify-artifact against a platform whose declared artifact_path
        # doesn't exist on this machine must fail cleanly through `file`,
        # not raise -- exercises the real dispatch path for a command that
        # takes no extra flags at all.
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = ci_entrypoint.main(["verify-artifact", "--platform", "linux"])
        self.assertEqual(rc, 1)


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
