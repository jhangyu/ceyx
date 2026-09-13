"""Unit tests for native/scripts/ci/stage.py (WI-8)."""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))  # native/scripts/

from ci import markerdiff, stage  # noqa: E402
from ci import run as ci_run  # noqa: E402

_GOLDEN_DIR = Path(__file__).resolve().parent / "golden" / "expected"
_REPO_ROOT = Path(__file__).resolve().parents[4]
_CI_ENTRYPOINT = _REPO_ROOT / "native" / "scripts" / "ci.py"


def _emit(fn, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = fn(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


class TestStage(unittest.TestCase):
    """``stage()`` -- copies declared files into <artifact_dir>/native."""

    def _dirs(self):
        base = Path(tempfile.mkdtemp())
        native_dir = base / "native"
        (native_dir / "build-linux").mkdir(parents=True)
        artifact_dir = base / "artifacts"
        return native_dir, artifact_dir

    def test_happy_path_copies_all(self) -> None:
        native_dir, artifact_dir = self._dirs()
        for name in ("libdng_decoder_native.so", "libheif.so.1", "libde265.so.0"):
            (native_dir / "build-linux" / name).write_bytes(b"\x7fELF")
        with mock.patch.object(
            stage,
            "declared_names",
            lambda p: ["libdng_decoder_native.so", "libheif.so.1", "libde265.so.0"],
        ):
            rc, out, err = _emit(stage.stage, "linux", str(artifact_dir), str(native_dir))
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        dest = artifact_dir / "native"
        for name in ("libdng_decoder_native.so", "libheif.so.1", "libde265.so.0"):
            self.assertTrue((dest / name).exists(), f"{name} not staged")
        # The step's only output today is the ls -la listing.
        self.assertIn("libdng_decoder_native.so", out)

    def test_missing_declared_source_fails(self) -> None:
        native_dir, artifact_dir = self._dirs()
        (native_dir / "build-linux" / "libdng_decoder_native.so").write_bytes(b"\x7fELF")
        # libheif.so.1 declared but never produced by the build.
        with mock.patch.object(
            stage, "declared_names", lambda p: ["libdng_decoder_native.so", "libheif.so.1"]
        ):
            rc, out, err = _emit(stage.stage, "linux", str(artifact_dir), str(native_dir))
        self.assertEqual(rc, 1)
        self.assertIn("::error::declared shipped file 'libheif.so.1' is missing from", err)
        self.assertFalse((artifact_dir / "native" / "libheif.so.1").exists())

    def test_emission_matches_golden(self) -> None:
        """stage() emits no `MARKER=value` lines -- the golden fixture is a
        deliberately-empty-of-markers file (see its own comment header, non-
        zero bytes/lines so this is distinguishable from a failed/absent
        capture). ``assertEqual([], [])`` alone would be a can't-fail
        assertion (it would stay green even if the extractor regressed to
        returning [] unconditionally), so this also positively asserts the
        listing content stage() is supposed to print -- if stage() silently
        stopped printing anything, THIS assertion catches it."""
        native_dir, artifact_dir = self._dirs()
        (native_dir / "build-linux" / "libdng_decoder_native.so").write_bytes(b"\x7fELF")
        with mock.patch.object(stage, "declared_names", lambda p: ["libdng_decoder_native.so"]):
            rc, out, _ = _emit(stage.stage, "linux", str(artifact_dir), str(native_dir))
        self.assertEqual(rc, 0)

        golden = (_GOLDEN_DIR / "stage-linux.markers").read_text(encoding="utf-8")
        self.assertGreater(len(golden.encode("utf-8")), 0, "golden fixture must not be 0 bytes")
        expected = [ln for ln in golden.splitlines() if not ln.startswith("#")]
        emitted = markerdiff.extract(out)
        self.assertEqual(emitted, expected)

        # Positive assertion: the listing itself must actually be there.
        self.assertIn("libdng_decoder_native.so", out)


class TestAssertStagedGroup(unittest.TestCase):
    """``assert_staged_group()`` -- the atomic-group completeness gate."""

    def _staged(self, names):
        d = Path(tempfile.mkdtemp())
        native = d / "artifacts" / "native"
        native.mkdir(parents=True)
        for name in names:
            (native / name).write_bytes(b"\x7fELF")
        return d / "artifacts"

    def test_shared_lib_count_zero_errors(self) -> None:
        artifact_dir = self._staged([])
        with mock.patch.object(stage, "declared_names", lambda p: ["libdng_decoder_native.so"]):
            rc, out, err = _emit(stage.assert_staged_group, "linux", str(artifact_dir))
        self.assertEqual(rc, 1)
        self.assertIn("SHARED_LIB_COUNT=0", out)
        self.assertIn("::error::No libdng_decoder_native*.so found in", err)

    def test_missing_declared_file_is_a_mismatch(self) -> None:
        artifact_dir = self._staged(["libdng_decoder_native.so"])
        with mock.patch.object(
            stage, "declared_names", lambda p: ["libdng_decoder_native.so", "libheif.so.1"]
        ):
            rc, out, err = _emit(stage.assert_staged_group, "linux", str(artifact_dir))
        self.assertEqual(rc, 1)
        self.assertIn(
            "::error::Linux staged set does not match the shipped_files.toml declaration.", err
        )

    def test_extra_undeclared_so_is_a_mismatch(self) -> None:
        """The 2026-09-01 incident's direction: an UNDECLARED extra shared
        object staged alongside the declared set must fail the group gate,
        not merely a missing declared file."""
        artifact_dir = self._staged(["libdng_decoder_native.so", "libextra.so"])
        with mock.patch.object(stage, "declared_names", lambda p: ["libdng_decoder_native.so"]):
            rc, out, err = _emit(stage.assert_staged_group, "linux", str(artifact_dir))
        self.assertEqual(rc, 1)
        self.assertIn(
            "::error::Linux staged set does not match the shipped_files.toml declaration.", err
        )

    def test_atomic_group_success_line_is_exact(self) -> None:
        artifact_dir = self._staged(["libdng_decoder_native.so", "libheif.so.1", "libde265.so.0"])
        with mock.patch.object(
            stage,
            "declared_names",
            lambda p: ["libdng_decoder_native.so", "libheif.so.1", "libde265.so.0"],
        ):
            rc, out, _ = _emit(stage.assert_staged_group, "linux", str(artifact_dir))
        self.assertEqual(rc, 0)
        self.assertIn("ATOMIC_GROUP_COMPLETE=1 (EXPECTED_SET == STAGED_SET)", out)
        self.assertIn("EXPECTED_SET:", out)
        self.assertIn("STAGED_SET:", out)
        self.assertIn("SHARED_LIB_COUNT=1", out)

    def test_emission_matches_golden(self) -> None:
        artifact_dir = self._staged(["libdng_decoder_native.so", "libheif.so.1", "libde265.so.0"])
        with mock.patch.object(
            stage,
            "declared_names",
            lambda p: ["libdng_decoder_native.so", "libheif.so.1", "libde265.so.0"],
        ):
            rc, out, _ = _emit(stage.assert_staged_group, "linux", str(artifact_dir))
        self.assertEqual(rc, 0)
        emitted = markerdiff.extract(out)
        golden = (_GOLDEN_DIR / "assert-staged-group-linux.markers").read_text(encoding="utf-8")
        self.assertEqual(emitted, golden.splitlines())


class TestStageBareScriptInvocation(unittest.TestCase):
    """Owed item (leader ruling on WI-8 signoff): a genuine subprocess
    invocation of ``python3 native/scripts/ci.py stage ...`` -- the exact
    call shape linux_build.yml uses (WI-9, plan:1083/1085) -- not merely an
    in-process call through ``ci_entrypoint.main()``. The rationale is
    deliberately NOT delegated to the WI-9-dependency wiring commit's own
    tests (which import ``ci_entrypoint`` and call ``main()`` in-process):
    the author of the calling code is the worst person to be the sole
    tester that the call works, and a module verified only through imports
    has never actually been run (the exact defect that held push 2 for a
    round). Routed through ``ci.run.run()``, NOT a raw ``subprocess.run()``
    call in this file: `check_no_test_execution_in_ci.py`'s WI-5(b) AST
    scan flags any `subprocess.*` call under `native/scripts/ci/**.py`
    outside `run.py` as `[subprocess-outside-run]` regardless of whether
    the file is a test -- unlike the sibling shell-prohibition lint, this
    scan does NOT exempt `test_*.py` (found by running the guard locally
    after a first draft used raw `subprocess.run` directly; see
    tmp/verify/pyci-wi8-e2e-pytest.txt for the RED capture)."""

    def _run_ci(self, *argv: str):
        return ci_run.run([sys.executable, str(_CI_ENTRYPOINT), *argv], cwd=str(_REPO_ROOT))

    def test_stage_and_assert_staged_group_bare_script(self) -> None:
        base = Path(tempfile.mkdtemp())
        native_dir = base / "native"
        (native_dir / "build-linux").mkdir(parents=True)
        artifact_dir = base / "artifacts"

        import read_shipped_files

        entry = read_shipped_files.load_declaration()["linux"]
        for name in [entry["decoder"], *entry["companions"]]:
            (native_dir / "build-linux" / name).write_bytes(b"")

        result = self._run_ci(
            "stage", "--platform", "linux",
            "--artifact-dir", str(artifact_dir),
            "--native-dir", str(native_dir),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((artifact_dir / "native" / entry["decoder"]).exists())

        result2 = self._run_ci(
            "assert-staged-group", "--platform", "linux",
            "--artifact-dir", str(artifact_dir),
        )
        self.assertEqual(result2.returncode, 0, result2.stderr)
        self.assertIn("ATOMIC_GROUP_COMPLETE=1 (EXPECTED_SET == STAGED_SET)", result2.stdout)
        self.assertIn("SHARED_LIB_COUNT=1", result2.stdout)

    def test_stage_bare_script_missing_source_exits_nonzero(self) -> None:
        base = Path(tempfile.mkdtemp())
        native_dir = base / "native"
        (native_dir / "build-linux").mkdir(parents=True)
        artifact_dir = base / "artifacts"
        # No files written -- every declared source is missing.
        result = self._run_ci(
            "stage", "--platform", "linux",
            "--artifact-dir", str(artifact_dir),
            "--native-dir", str(native_dir),
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("::error::declared shipped file", result.stderr)


class TestStageWindows(unittest.TestCase):
    """``stage_windows()`` -- 4 required files hard-fail, 5th (.lib) optional."""

    def _dirs(self):
        base = Path(tempfile.mkdtemp())
        source_dir = base / "native" / "build-windows"
        source_dir.mkdir(parents=True)
        artifact_dir = base / "artifacts"
        return source_dir, artifact_dir

    _REQUIRED = ("dng_decoder_native.dll", "heif.dll", "libde265.dll", "libomp140.x86_64.dll")

    def test_happy_path_without_optional_lib_emits_notice(self) -> None:
        source_dir, artifact_dir = self._dirs()
        for name in self._REQUIRED:
            (source_dir / name).write_bytes(b"MZ")
        with mock.patch.object(stage, "declared_names", lambda p: list(self._REQUIRED)):
            rc, out, _ = _emit(stage.stage_windows, str(source_dir), str(artifact_dir))
        self.assertEqual(rc, 0)
        dest = artifact_dir / "native"
        for name in self._REQUIRED:
            self.assertTrue((dest / name).exists())
        self.assertFalse((dest / "dng_decoder_native.lib").exists())
        self.assertIn("::notice::dng_decoder_native.lib not present", out)

    def test_optional_lib_copied_when_present(self) -> None:
        source_dir, artifact_dir = self._dirs()
        for name in (*self._REQUIRED, "dng_decoder_native.lib"):
            (source_dir / name).write_bytes(b"MZ")
        with mock.patch.object(stage, "declared_names", lambda p: list(self._REQUIRED)):
            rc, out, _ = _emit(stage.stage_windows, str(source_dir), str(artifact_dir))
        self.assertEqual(rc, 0)
        self.assertTrue((artifact_dir / "native" / "dng_decoder_native.lib").exists())
        self.assertNotIn("::notice::", out)

    def test_missing_required_file_fails(self) -> None:
        source_dir, artifact_dir = self._dirs()
        (source_dir / "dng_decoder_native.dll").write_bytes(b"MZ")
        # heif.dll declared but never produced by the build.
        with mock.patch.object(stage, "declared_names", lambda p: ["dng_decoder_native.dll", "heif.dll"]):
            rc, _, err = _emit(stage.stage_windows, str(source_dir), str(artifact_dir))
        self.assertEqual(rc, 1)
        self.assertIn("::error::declared shipped file 'heif.dll' is missing from", err)


class TestAssertStagedGroupWindows(unittest.TestCase):
    """``assert_staged_group_windows()`` -- *.dll-only symmetric compare, the
    .lib import library is excluded from both sets."""

    def _staged(self, names):
        d = Path(tempfile.mkdtemp())
        native = d / "artifacts" / "native"
        native.mkdir(parents=True)
        for name in names:
            (native / name).write_bytes(b"MZ")
        return d / "artifacts"

    def test_matching_set_including_lib_present_passes_silently(self) -> None:
        artifact_dir = self._staged(
            ["dng_decoder_native.dll", "heif.dll", "libde265.dll", "dng_decoder_native.lib"]
        )
        with mock.patch.object(
            stage, "declared_names", lambda p: ["dng_decoder_native.dll", "heif.dll", "libde265.dll"]
        ):
            rc, out, err = _emit(stage.assert_staged_group_windows, str(artifact_dir))
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        # The .lib must not appear in either compared set.
        self.assertNotIn("dng_decoder_native.lib", out.split("STAGED_DLL_SET=")[1].splitlines()[0])

    def test_marker_values_carry_the_shell_pipeline_trailing_space(self) -> None:
        """windows_build.yml:867/869 pipe both sides through
        ``sort | tr '\\n' ' '`` -- ``tr`` converts sort's trailing newline
        into a trailing SPACE too, not just the separators between items.
        Byte-verified against the real shell pipeline with ``od -c``
        (impl-pyci-15-sonnet); lead6 ruling: reproduce it verbatim, do not
        "tidy" it into a bare ``" ".join(...)``."""
        artifact_dir = self._staged(["dng_decoder_native.dll", "heif.dll", "libde265.dll"])
        with mock.patch.object(
            stage, "declared_names", lambda p: ["dng_decoder_native.dll", "heif.dll", "libde265.dll"]
        ):
            rc, out, _ = _emit(stage.assert_staged_group_windows, str(artifact_dir))
        self.assertEqual(rc, 0)
        expected_line = "EXPECTED_DLL_SET=dng_decoder_native.dll heif.dll libde265.dll "
        staged_line = "STAGED_DLL_SET=dng_decoder_native.dll heif.dll libde265.dll "
        lines = out.splitlines()
        self.assertIn(expected_line, lines, "EXPECTED_DLL_SET is missing its trailing space")
        self.assertIn(staged_line, lines, "STAGED_DLL_SET is missing its trailing space")

    def test_missing_dll_is_a_mismatch(self) -> None:
        artifact_dir = self._staged(["dng_decoder_native.dll"])
        with mock.patch.object(
            stage, "declared_names", lambda p: ["dng_decoder_native.dll", "heif.dll"]
        ):
            rc, _, err = _emit(stage.assert_staged_group_windows, str(artifact_dir))
        self.assertEqual(rc, 1)
        self.assertIn(
            "::error::staged Windows DLL set does not match native/deps/shipped_files.toml's "
            "declaration",
            err,
        )


class TestStageMacos(unittest.TestCase):
    """``stage_macos()`` -- copy-only, no arch/reachability/rpath gates
    (those belong to verify_artifact.py, ruling 1)."""

    def _dirs(self):
        base = Path(tempfile.mkdtemp())
        native_dylib_dir = base / "build-macos-arm64"
        native_dylib_dir.mkdir(parents=True)
        artifact_dir = base / "artifacts"
        return native_dylib_dir, artifact_dir

    def test_happy_path_copies_decoder_and_companions(self) -> None:
        native_dylib_dir, artifact_dir = self._dirs()
        dylib = native_dylib_dir / "libdng_decoder_native.dylib"
        dylib.write_bytes(b"\xcf\xfa\xed\xfe")
        (native_dylib_dir / "libheif.1.dylib").write_bytes(b"\xcf\xfa\xed\xfe")
        (native_dylib_dir / "libde265.0.dylib").write_bytes(b"\xcf\xfa\xed\xfe")
        rc, out, err = _emit(
            stage.stage_macos,
            str(dylib),
            ["libheif.1.dylib", "libde265.0.dylib"],
            str(artifact_dir),
        )
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        dest = artifact_dir / "native"
        for name in ("libdng_decoder_native.dylib", "libheif.1.dylib", "libde265.0.dylib"):
            self.assertTrue((dest / name).exists(), f"{name} not staged")
        self.assertIn("libdng_decoder_native.dylib", out)

    def test_missing_companion_fails(self) -> None:
        native_dylib_dir, artifact_dir = self._dirs()
        dylib = native_dylib_dir / "libdng_decoder_native.dylib"
        dylib.write_bytes(b"\xcf\xfa\xed\xfe")
        # libheif.1.dylib never produced by the build.
        rc, _, err = _emit(
            stage.stage_macos, str(dylib), ["libheif.1.dylib"], str(artifact_dir)
        )
        self.assertEqual(rc, 1)
        self.assertIn(
            "::error::expected companion dylib libheif.1.dylib not found in", err
        )
        self.assertFalse((artifact_dir / "native" / "libheif.1.dylib").exists())


class TestStageAndroid(unittest.TestCase):
    """``stage_android()`` -- soft-fail copy, never errors (matches the
    shell's ``|| true`` on both the find and the listing)."""

    def _dirs(self):
        base = Path(tempfile.mkdtemp())
        source_dir = base / "native" / "build-android" / "android-arm64"
        source_dir.mkdir(parents=True)
        artifact_dir = base / "artifacts"
        return source_dir, artifact_dir

    def test_happy_path_copies_all_so_files(self) -> None:
        source_dir, artifact_dir = self._dirs()
        for name in ("libdng_decoder_native.so", "libheif.so", "libde265.so"):
            (source_dir / name).write_bytes(b"\x7fELF")
        rc, out, err = _emit(stage.stage_android, str(source_dir), str(artifact_dir))
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        dest = artifact_dir / "native"
        for name in ("libdng_decoder_native.so", "libheif.so", "libde265.so"):
            self.assertTrue((dest / name).exists())

    def test_missing_source_dir_does_not_fail(self) -> None:
        """Soft-fail: a nonexistent source dir must not raise or return
        nonzero -- matches ``find ... || true``."""
        base = Path(tempfile.mkdtemp())
        source_dir = base / "does-not-exist"
        artifact_dir = base / "artifacts"
        rc, _, err = _emit(stage.stage_android, str(source_dir), str(artifact_dir))
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")


class TestAssertStagedGroupAndroid(unittest.TestCase):
    """``assert_staged_group_android()`` -- asymmetric completeness: required
    companions present, decoder nonzero, NO undeclared-extra detection."""

    def _staged(self, names):
        d = Path(tempfile.mkdtemp())
        native = d / "artifacts" / "native"
        native.mkdir(parents=True)
        for name in names:
            (native / name).write_bytes(b"\x7fELF")
        return d / "artifacts"

    def test_no_decoder_errors(self) -> None:
        artifact_dir = self._staged([])
        with mock.patch.object(stage, "declared_companions", lambda p: []):
            rc, _, err = _emit(stage.assert_staged_group_android, str(artifact_dir))
        self.assertEqual(rc, 1)
        self.assertIn("::error::No libdng_decoder_native*.so found in", err)

    def test_missing_companion_fails(self) -> None:
        artifact_dir = self._staged(["libdng_decoder_native.so"])
        with mock.patch.object(stage, "declared_companions", lambda p: ["libheif.so"]):
            rc, _, err = _emit(stage.assert_staged_group_android, str(artifact_dir))
        self.assertEqual(rc, 1)
        self.assertIn(
            "::error::missing required companion .so(s) in", err
        )
        self.assertIn("libheif.so", err)

    def test_undeclared_extra_so_is_not_a_mismatch(self) -> None:
        """The android-specific asymmetry (ruling 3): an extra undeclared
        .so alongside a complete required set must still PASS -- unlike
        linux/windows, this is deliberately not a symmetric diff."""
        artifact_dir = self._staged(["libdng_decoder_native.so", "libheif.so", "libc++_shared.so"])
        with mock.patch.object(stage, "declared_companions", lambda p: ["libheif.so"]):
            rc, out, _ = _emit(stage.assert_staged_group_android, str(artifact_dir))
        self.assertEqual(rc, 0)
        self.assertIn("ANDROID_COMPANION_GROUP_COMPLETE=1", out)

    def test_success_marker_is_android_specific(self) -> None:
        artifact_dir = self._staged(["libdng_decoder_native.so", "libheif.so", "libde265.so"])
        with mock.patch.object(stage, "declared_companions", lambda p: ["libheif.so", "libde265.so"]):
            rc, out, _ = _emit(stage.assert_staged_group_android, str(artifact_dir))
        self.assertEqual(rc, 0)
        self.assertIn("ANDROID_COMPANION_GROUP_COMPLETE=1", out)
        self.assertNotIn("ATOMIC_GROUP_COMPLETE", out)


if __name__ == "__main__":
    unittest.main()
