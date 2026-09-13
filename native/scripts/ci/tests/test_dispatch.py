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
    "assert-configure-log",
    "assert-staged-companions",
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
    "locate-clang-cl",
    "verify-vulkan-lib",
    # WI-29/WI-30 (push 8b, dispatch wiring extension ruled by lead9-pyci-opus):
    # dist_build.py + vcpkg.py's argv surfaces.
    "dist-build",
    "dist-list",
    "provision",
    "vcpkg",
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
        # Push 6 (WI-16b): min-runtime is no longer P0-scaffolding for
        # macOS -- it must get PAST arg parsing AND past `_not_yet()` into
        # ci/minruntime.py itself. Spied rather than actually invoked: this
        # dev host has no staged macOS .dylib, so a real call spawns real
        # `read_min_runtime.py`/`assert_min_runtime_matches_declared.py`
        # child processes that print real `READ_MIN_RUNTIME_RC=1`/
        # `MIN_RUNTIME_DRIFT_RC=1`/`MIN_RUNTIME_DRIFT_RESULT=FAIL` marker
        # text -- `redirect_stderr` alone does not catch it (these are
        # stdout-level `report.rc`/`report.marker` writes), and even a full
        # `redirect_stdout` would not stop an inherited-fd child's own
        # argparse errors from reaching the real job log. That marker text
        # leaked into a GREEN `nativetests` CI run (push-6 AC-2 finding),
        # where it registered as unlisted `+N` additions. A spy proves
        # dispatch reached `ci.minruntime.min_runtime` without executing
        # anything.
        import ci.minruntime as minruntime_module
        from unittest import mock

        with mock.patch.object(minruntime_module, "min_runtime", return_value=0) as mocked:
            rc = ci_entrypoint.main(
                ["min-runtime", "--platform", "macos", "--arch", "arm64"]
            )
        self.assertEqual(rc, 0)
        mocked.assert_called_once_with("macos", "arm64")


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


class TestOrientationDispatch(unittest.TestCase):
    """Push 4: assert-orientation is a genuine three-algorithm dispatch
    (macos/android/linux+windows), with platform-scoped flags rejected
    rather than silently ignored on the wrong platform."""

    def setUp(self):
        import tempfile

        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)
        self.tmp = Path(self._tmpdir.name)

    def test_macos_missing_dylib_path_is_argparse_error(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(
                    ["assert-orientation", "--platform", "macos", "--arch", "arm64"]
                )
        self.assertEqual(ctx.exception.code, 2)

    def test_android_missing_artifact_dir_or_ndk_home_is_argparse_error(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(
                    ["assert-orientation", "--platform", "android", "--ndk-home", "/x"]
                )
        self.assertEqual(ctx.exception.code, 2)

    def test_linux_rejects_macos_and_android_only_flags(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(
                    ["assert-orientation", "--platform", "linux", "--dylib-path", "/x"]
                )
        self.assertEqual(ctx.exception.code, 2)

    def test_macos_rejects_android_only_flags(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(
                    [
                        "assert-orientation",
                        "--platform",
                        "macos",
                        "--arch",
                        "arm64",
                        "--dylib-path",
                        "/x",
                        "--artifact-dir",
                        "/y",
                    ]
                )
        self.assertEqual(ctx.exception.code, 2)

    def test_linux_end_to_end_missing_binary_is_handled_not_traceback(self):
        # Exercises the real dispatch path with no platform-specific flags
        # at all (linux takes none) -- a missing strings tool or missing
        # artifact must fail cleanly through the module, not raise.
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = ci_entrypoint.main(["assert-orientation", "--platform", "linux"])
        self.assertEqual(rc, 1)

    def test_android_end_to_end_missing_so_is_handled_not_traceback(self):
        artifact_dir = self.tmp / "artifact_empty"
        (artifact_dir / "native").mkdir(parents=True)
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = ci_entrypoint.main(
                [
                    "assert-orientation",
                    "--platform",
                    "android",
                    "--artifact-dir",
                    str(artifact_dir),
                    "--ndk-home",
                    str(self.tmp / "ndk"),
                ]
            )
        self.assertEqual(rc, 1)


class TestCodecProbeDispatch(unittest.TestCase):
    """Push 5: codec-probe is a three-leg command (linux/macos/windows) with
    a macOS-only --dist-dir, enforced in both directions at the CLI layer
    (mirroring _enforce_orientation_flags) in addition to codec_probe.py's
    own in-module validation -- and android is rejected permanently, not
    via `_linux_only_commands`'s temporary "not yet implemented" shape."""

    def setUp(self):
        import tempfile

        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)
        self.tmp = Path(self._tmpdir.name)

    def test_macos_missing_dist_dir_is_argparse_error(self):
        # Regression pin: codec-probe carries NO --arch parameter at all
        # (codec_probe() has no such argument), so this must fail naming
        # --dist-dir specifically -- not --arch, which is what the generic
        # C-G9 helper wrongly demanded before _add_platform_command grew
        # `with_arch=False` and _enforce_arch_requirement grew a
        # presence-based (not value-based) applicability check.
        buf = io.StringIO()
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(buf):
                ci_entrypoint.main(
                    ["codec-probe", "--platform", "macos", "--workspace", str(self.tmp)]
                )
        self.assertEqual(ctx.exception.code, 2)
        self.assertIn("--dist-dir is required", buf.getvalue())
        self.assertNotIn("--arch", buf.getvalue())

    def test_codec_probe_has_no_arch_flag_at_all(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(
                    [
                        "codec-probe",
                        "--platform",
                        "macos",
                        "--workspace",
                        str(self.tmp),
                        "--dist-dir",
                        "x",
                        "--arch",
                        "arm64",
                    ]
                )
        # argparse's own "unrecognized arguments" error, not C-G9's --
        # codec-probe's parser never defines --arch, so passing it is a
        # plain argparse failure, distinct from the C-G9 accept/reject path.
        self.assertEqual(ctx.exception.code, 2)

    def test_macos_with_dist_dir_reaches_the_module_not_argparse(self):
        # The fixed shape: supplying --dist-dir (and no --arch at all) must
        # get PAST argument parsing entirely and into codec_probe.py itself
        # -- proven by main() returning normally (no SystemExit at all,
        # i.e. no argparse rejection) rather than the pre-fix behaviour of
        # a SystemExit(2) demanding --arch.
        buf_out = io.StringIO()
        buf_err = io.StringIO()
        with redirect_stdout(buf_out), redirect_stderr(buf_err):
            rc = ci_entrypoint.main(
                [
                    "codec-probe",
                    "--platform",
                    "macos",
                    "--workspace",
                    str(self.tmp),
                    "--dist-dir",
                    "x",
                ]
            )
        self.assertIsInstance(rc, int)  # no SystemExit raised -- past argparse
        self.assertNotIn("--dist-dir", buf_err.getvalue())
        self.assertNotIn("--arch", buf_err.getvalue())

    def test_linux_rejects_dist_dir(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(
                    [
                        "codec-probe",
                        "--platform",
                        "linux",
                        "--workspace",
                        str(self.tmp),
                        "--dist-dir",
                        "x",
                    ]
                )
        self.assertEqual(ctx.exception.code, 2)

    def test_windows_rejects_dist_dir(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(
                    [
                        "codec-probe",
                        "--platform",
                        "windows",
                        "--workspace",
                        str(self.tmp),
                        "--dist-dir",
                        "x",
                    ]
                )
        self.assertEqual(ctx.exception.code, 2)

    def test_missing_workspace_is_argparse_error_not_traceback(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(["codec-probe", "--platform", "linux"])
        self.assertEqual(ctx.exception.code, 2)

    def test_android_is_rejected_permanently_not_via_not_yet(self):
        # android has no probe_codecs step at all -- this must NOT be the
        # `_not_yet()` P0-scaffolding message ("not implemented yet"),
        # which would wrongly invite building an android twin later.
        buf = io.StringIO()
        with redirect_stderr(buf):
            rc = ci_entrypoint.main(
                ["codec-probe", "--platform", "android", "--workspace", str(self.tmp)]
            )
        self.assertEqual(rc, 2)
        self.assertNotIn("not implemented yet", buf.getvalue())
        self.assertIn("no probe_codecs step", buf.getvalue())

    def test_linux_end_to_end_missing_dist_is_handled_not_traceback(self):
        # No HEIF dist tree at all in this empty workspace: the compiler
        # invocation (or tool resolution) must fail cleanly through the
        # module, never raise -- exercises the real dispatch call shape.
        buf_out = io.StringIO()
        buf_err = io.StringIO()
        with redirect_stdout(buf_out), redirect_stderr(buf_err):
            rc = ci_entrypoint.main(
                ["codec-probe", "--platform", "linux", "--workspace", str(self.tmp)]
            )
        self.assertNotEqual(rc, 0)


class TestMinRuntimeDispatchGeneralised(unittest.TestCase):
    """Push 6, WI-16b's follow-on: `min-runtime` dispatches to the new
    `ci/minruntime.py` (four-platform `min_runtime_source` dispatch), not
    the old linux-only `ci/verify_artifact.py:min_runtime`, and is no
    longer gated by `_linux_only_commands` -- pinning both the widening
    (non-linux platforms now reach the module) and that the widening is
    narrow (a genuinely-still-linux-only sibling command is untouched)."""

    def setUp(self):
        import tempfile

        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)
        self.tmp = Path(self._tmpdir.name)

    def _assert_dispatches_to_minruntime_not_verify_artifact(self, argv, expected_call):
        # `assertNotIn("not implemented yet", ...)` / `assertIsInstance(rc,
        # int)` were proved too weak to guard this: repointing dispatch at
        # the old `ci.verify_artifact.min_runtime` twin left the windows
        # variant of this test GREEN (that twin doesn't crash for windows,
        # returns a plausible int, prints no "not implemented yet" text --
        # the assertions cannot tell the twin from the real module). The
        # android/macos variants happened to ERROR under that same
        # regression, but only because `verify_artifact.min_runtime`'s
        # `_artifact_path()` returns `None` off-linux and `run.run_to_file`
        # chokes on it -- a coincidence of the TWIN's internals, not a
        # property of the test, and a future None-handling tidy-up there
        # would silently convert those into windows' same blind spot.
        #
        # UPDATED (push 7, P-10): the twin (`verify_artifact.min_runtime`)
        # has now been DELETED -- it was the orphaned duplicate the class
        # docstring already described. A `mock.patch.object` on a deleted
        # attribute raises `AttributeError`, so the "twin not called" half
        # is no longer a spy at all: it is the STRONGER claim that the twin
        # cannot exist to be called, checked structurally via `hasattr`.
        # The positive half (new module called with the expected arguments)
        # is kept unchanged -- it is the half that actually proves
        # production dispatch reaches the right implementation, which is
        # the exact failure this whole class exists to catch.
        from unittest import mock

        import ci.minruntime as minruntime_module
        import ci.verify_artifact as verify_artifact_module

        self.assertFalse(
            hasattr(verify_artifact_module, "min_runtime"),
            "verify_artifact.min_runtime was deleted as an orphan (P-10) -- "
            "its reappearance would recreate the same-name-diverges defect "
            "this class exists to catch.",
        )
        with mock.patch.object(
            minruntime_module, "min_runtime", return_value=0
        ) as mocked_new:
            rc = ci_entrypoint.main(argv)
        self.assertEqual(rc, 0)
        mocked_new.assert_called_once_with(*expected_call)

    def test_windows_min_runtime_reaches_minruntime_not_verify_artifact(self):
        self._assert_dispatches_to_minruntime_not_verify_artifact(
            ["min-runtime", "--platform", "windows"], ("windows", None)
        )

    def test_android_min_runtime_reaches_minruntime_not_verify_artifact(self):
        self._assert_dispatches_to_minruntime_not_verify_artifact(
            ["min-runtime", "--platform", "android"], ("android", None)
        )

    def test_macos_min_runtime_reaches_minruntime_not_verify_artifact(self):
        self._assert_dispatches_to_minruntime_not_verify_artifact(
            ["min-runtime", "--platform", "macos", "--arch", "arm64"],
            ("macos", "arm64"),
        )

    def test_a_still_linux_only_sibling_command_is_still_rejected_off_platform(self):
        # Narrowness check, updated for f651550f (WI-34, push 8 follow-on)
        # and again for WI-36 (push 8, `_linux_only_commands` scaffold
        # retired outright -- it had been an empty, permanently-unreachable
        # literal since WI-34, confirmed dead by a repo-wide grep before
        # deletion, not merely left inert): `import-closure` now has its
        # own explicit `_IMPORT_CLOSURE_PLATFORMS` allowlist in dispatch().
        # P-23 (parking-lot round) REMOVED windows from the excluded side:
        # its stated blocker ("no PE parser") was stale -- the parser landed
        # with WI-4 and verify_artifact now has the PE dump-capture leg too.
        # macOS is the only permanent exclusion left (no DT_NEEDED-shaped
        # step in its YAML at all), so macOS is what this test now isolates:
        # rejected with RC 2 via a named `::error::`, never a silent pass and
        # never the old generic "not implemented yet" scaffolding message.
        # `--arch` is supplied because macOS trips ci.py's
        # `_enforce_arch_requirement` FIRST and would exit 2 through argparse
        # instead of through the gate under test -- two different RC-2s, and
        # only one of them proves what this test claims.
        buf = io.StringIO()
        with redirect_stderr(buf):
            rc = ci_entrypoint.main(["import-closure", "--platform", "macos", "--arch", "arm64"])
        self.assertEqual(rc, 2)
        self.assertIn(
            "::error::import-closure has no --platform 'macos' leg", buf.getvalue()
        )

    def test_import_closure_windows_is_accepted_and_reaches_verify_artifact(self):
        """P-23's counterpart to the macOS rejection above: windows must now
        DISPATCH rather than be refused at the CLI layer. Spies on the module
        attribute instead of running the gate, so this asserts ROUTING only --
        the gate's own behaviour is test_verify_artifact.py's subject."""
        from unittest import mock

        import ci.verify_artifact as verify_artifact

        with mock.patch.object(verify_artifact, "import_closure", return_value=0) as mocked:
            rc = ci_entrypoint.main(["import-closure", "--platform", "windows"])
        self.assertEqual(rc, 0)
        mocked.assert_called_once_with("windows", artifact_dir=None, ndk_home=None)

    def test_min_runtime_dispatch_calls_minruntime_module_not_verify_artifact(self):
        # The regression this whole class exists to prevent: a behavioural
        # test on linux alone cannot distinguish `ci.minruntime.min_runtime`
        # from a same-named twin elsewhere -- same name, same signature,
        # both produce a plausible-looking int on linux. Spy on the real
        # module's attribute so a future wrong import (a revert, a merge
        # conflict resolved the wrong way) fails loudly here; the twin
        # itself is gone (P-10, push 7), so its non-existence is asserted
        # structurally rather than spied on -- see the sibling method above
        # for the full rationale.
        from unittest import mock

        import ci.minruntime as minruntime_module
        import ci.verify_artifact as verify_artifact_module

        self.assertFalse(hasattr(verify_artifact_module, "min_runtime"))
        with mock.patch.object(
            minruntime_module, "min_runtime", return_value=0
        ) as mocked_new:
            rc = ci_entrypoint.main(["min-runtime", "--platform", "linux"])
        self.assertEqual(rc, 0)
        mocked_new.assert_called_once_with("linux", None)


class TestCapabilityVectorDispatch(unittest.TestCase):
    """Push 6: capability-vector is a three-leg command (linux/macos/windows,
    same PERMANENT android exclusion shape as codec-probe), no --arch flag
    at all, with `--source configure-log` accepted only for macOS and
    `--dylib-path` required for macOS's `source=probe` (default) leg --
    enforced at the CLI layer in addition to capability.py's own
    ValueErrors, mirroring _enforce_orientation_flags/_enforce_codec_probe_
    flags."""

    def test_android_is_rejected_permanently_not_via_not_yet(self):
        buf = io.StringIO()
        with redirect_stderr(buf):
            rc = ci_entrypoint.main(
                ["capability-vector", "--platform", "android", "--kind", "codec"]
            )
        self.assertEqual(rc, 2)
        self.assertNotIn("not implemented yet", buf.getvalue())
        self.assertIn("no capability-vector step", buf.getvalue())

    def test_capability_vector_has_no_arch_flag_at_all(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(
                    [
                        "capability-vector",
                        "--platform",
                        "linux",
                        "--kind",
                        "codec",
                        "--arch",
                        "x86_64",
                    ]
                )
        self.assertEqual(ctx.exception.code, 2)

    def test_macos_probe_missing_dylib_path_is_argparse_error(self):
        buf = io.StringIO()
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(buf):
                ci_entrypoint.main(
                    ["capability-vector", "--platform", "macos", "--kind", "codec"]
                )
        self.assertEqual(ctx.exception.code, 2)
        self.assertIn("--dylib-path is required", buf.getvalue())

    def test_configure_log_rejects_non_macos(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(
                    [
                        "capability-vector",
                        "--platform",
                        "linux",
                        "--kind",
                        "codec",
                        "--source",
                        "configure-log",
                    ]
                )
        self.assertEqual(ctx.exception.code, 2)

    def test_configure_log_rejects_dylib_path(self):
        with self.assertRaises(SystemExit) as ctx:
            with redirect_stderr(io.StringIO()):
                ci_entrypoint.main(
                    [
                        "capability-vector",
                        "--platform",
                        "macos",
                        "--kind",
                        "codec",
                        "--source",
                        "configure-log",
                        "--dylib-path",
                        "x",
                    ]
                )
        self.assertEqual(ctx.exception.code, 2)

    def test_linux_probe_reaches_module_not_not_yet(self):
        # No staged .so on this machine: the module's own probe must fail
        # cleanly (missing artifact), never emit `_not_yet()`'s
        # P0-scaffolding message -- proves dispatch reached
        # ci/capability.py, same discriminator shape as the codec-probe/
        # orientation dispatch tests. A real --expect token is supplied so
        # the failure is capability.py's artifact-missing path, not
        # codec_capability_probe.py's own inner argparse usage error.
        buf_out = io.StringIO()
        buf_err = io.StringIO()
        with redirect_stdout(buf_out), redirect_stderr(buf_err):
            rc = ci_entrypoint.main(
                [
                    "capability-vector",
                    "--platform",
                    "linux",
                    "--kind",
                    "codec",
                    "--expect",
                    "HEIF:decode",
                ]
            )
        self.assertNotIn("not implemented yet", buf_err.getvalue())
        self.assertIsInstance(rc, int)


class TestAssertVcpkgArtefactsMacosDispatch(unittest.TestCase):
    """Regression pin for the gate-widening step: impl-18 committed
    `_assert_vcpkg_artefacts_macos` (81fc2b94) but flagged that its branch
    was UNREACHABLE from the CLI until `_VCPKG_ARTEFACT_PLATFORMS` widened
    to admit macOS -- a passing module-level test proves the function
    works, not that dispatch reaches it (the same class of gap
    `build-zlib` sat in for two pushes)."""

    def test_macos_reaches_provision_with_arch_tag(self):
        from unittest import mock

        import ci.provision as provision_module

        with mock.patch.object(
            provision_module, "assert_vcpkg_artefacts", return_value=0
        ) as mocked:
            rc = ci_entrypoint.main(
                [
                    "assert-vcpkg-artefacts", "--platform", "macos",
                    "--triplet", "arm64-osx-heif", "--runner-temp", "/rt",
                    "--arch-tag", "arm64",
                ]
            )
        self.assertEqual(rc, 0)
        mocked.assert_called_once_with("macos", "arm64-osx-heif", "/rt", arch_tag="arm64")

    def test_windows_still_rejected_narrowness_check(self):
        buf = io.StringIO()
        with redirect_stderr(buf):
            rc = ci_entrypoint.main(
                [
                    "assert-vcpkg-artefacts", "--platform", "windows",
                    "--triplet", "x64-windows-heif", "--runner-temp", "/rt",
                ]
            )
        self.assertEqual(rc, 2)
        self.assertIn("::error::", buf.getvalue())


class TestBuildZlibDispatch(unittest.TestCase):
    """Regression pin for the defect lead7 found at ae56dc82: `build-zlib`
    had a subparser and a frozen-surface docstring entry (implying it was
    wired) but NO dispatch branch, so it silently fell through to
    `_not_yet()`. `ci.zlib_build` does not exist yet (impl-15's module,
    landing separately) -- a fake module is injected into `sys.modules` so
    this test asserts the DISPATCH reaches `zlib_build.build_zlib(version)`
    with the right argument, not merely that argparse accepts the flag."""

    def test_build_zlib_reaches_zlib_build_module_not_not_yet(self):
        from unittest import mock

        import ci.zlib_build as zlib_build_module

        with mock.patch.object(zlib_build_module, "build_zlib", return_value=0) as mocked:
            buf = io.StringIO()
            with redirect_stderr(buf):
                rc = ci_entrypoint.main(
                    ["build-zlib", "--version", "1.3.1", "--workspace", "/ws"]
                )
        self.assertEqual(rc, 0)
        mocked.assert_called_once_with("1.3.1", "/ws")
        self.assertNotIn("not implemented yet", buf.getvalue())


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
