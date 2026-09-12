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

_GOLDEN_DIR = Path(__file__).resolve().parent / "golden" / "expected"


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
        native_dir, artifact_dir = self._dirs()
        (native_dir / "build-linux" / "libdng_decoder_native.so").write_bytes(b"\x7fELF")
        with mock.patch.object(stage, "declared_names", lambda p: ["libdng_decoder_native.so"]):
            rc, out, _ = _emit(stage.stage, "linux", str(artifact_dir), str(native_dir))
        self.assertEqual(rc, 0)
        emitted = markerdiff.extract(out)
        golden = (_GOLDEN_DIR / "stage-linux.markers").read_text(encoding="utf-8")
        expected = golden.splitlines() if golden.strip() else []
        self.assertEqual(emitted, expected)


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


if __name__ == "__main__":
    unittest.main()
