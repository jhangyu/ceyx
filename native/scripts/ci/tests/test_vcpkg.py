"""Unit tests for native/scripts/ci/vcpkg.py (push 8b, WI-30)."""

from __future__ import annotations

import io
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import run as run_module
from .. import vcpkg


def _fake_run_result(returncode=0, stdout="", stderr=""):
    return run_module.RunResult(argv=[], returncode=returncode, stdout=stdout, stderr=stderr)


def _run_captured(func, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = func(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


class VcpkgTests(unittest.TestCase):
    def setUp(self):
        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)
        self.tmp = Path(self._tmpdir.name)

    # ---- baseline -----------------------------------------------------

    def test_baseline_reads_json_and_appends_to_github_env(self):
        manifest = self.tmp / "vcpkg.json"
        manifest.write_text('{"builtin-baseline": "deadbeef"}')
        github_env = self.tmp / "github_env"
        github_env.write_text("")

        rc = vcpkg.baseline(str(manifest), str(github_env))
        self.assertEqual(rc, 0)
        self.assertEqual(github_env.read_text(), "VCPKG_BASELINE=deadbeef\n")

    def test_baseline_appends_never_truncates(self):
        """Red-then-green for the append contract: a pre-populated
        GITHUB_ENV must keep its existing line."""
        manifest = self.tmp / "vcpkg.json"
        manifest.write_text('{"builtin-baseline": "cafef00d"}')
        github_env = self.tmp / "github_env"
        github_env.write_text("SOME_OTHER_VAR=1\n")

        vcpkg.baseline(str(manifest), str(github_env))
        self.assertEqual(github_env.read_text(), "SOME_OTHER_VAR=1\nVCPKG_BASELINE=cafef00d\n")

    # ---- bootstrap ------------------------------------------------------

    def test_bootstrap_runs_clone_checkout_bat_version_in_order(self):
        calls = []

        def fake(argv, cwd=None, env=None):
            calls.append(argv)
            return _fake_run_result(returncode=0, stdout="ok\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, _, _ = _run_captured(vcpkg.bootstrap, str(self.tmp), "deadbeef")
        self.assertEqual(rc, 0)
        self.assertEqual(len(calls), 4)
        self.assertEqual(calls[0][:2], ["git", "clone"])
        self.assertEqual(calls[1][:3], ["git", "-C", str(self.tmp / "vcpkg")])
        self.assertIn("deadbeef", calls[1])
        self.assertTrue(calls[2][0].endswith("bootstrap-vcpkg.bat"))
        self.assertTrue(calls[3][0].endswith("vcpkg.exe"))
        self.assertEqual(calls[3][1], "version")

    def test_bootstrap_clone_failure_short_circuits(self):
        def fake(argv, cwd=None, env=None):
            if argv[:2] == ["git", "clone"]:
                return _fake_run_result(returncode=128, stderr="fatal: could not resolve host\n")
            raise AssertionError(f"should not reach: {argv}")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, _, err = _run_captured(vcpkg.bootstrap, str(self.tmp), "deadbeef")
        self.assertEqual(rc, 128)
        self.assertIn("git clone", err)

    # ---- install ----------------------------------------------------------

    def test_install_builds_correct_argv_and_marker(self):
        captured_argv = []

        def fake(argv, cwd=None, env=None):
            captured_argv.extend(argv)
            return _fake_run_result(returncode=0, stdout="Installing...\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                vcpkg.install,
                str(self.tmp / "native" / "vcpkg"),
                str(self.tmp / "vcpkg-installed"),
                "x64-windows-heif",
                ["de265", "aom"],
                "VCPKG_INSTALL_RC",
            )
        self.assertEqual(rc, 0)
        self.assertIn("VCPKG_INSTALL_RC=0", out)
        joined = " ".join(captured_argv)
        self.assertTrue(captured_argv[0].endswith("vcpkg.exe"))
        self.assertIn(f"--x-manifest-root={self.tmp / 'native' / 'vcpkg'}", joined)
        self.assertIn(f"--x-install-root={self.tmp / 'vcpkg-installed'}", joined)
        self.assertIn("--triplet=x64-windows-heif", joined)
        self.assertIn("--x-no-default-features", joined)
        self.assertIn("--x-feature=de265", joined)
        self.assertIn("--x-feature=aom", joined)
        self.assertIn("--no-print-usage", joined)

    def test_install_failure_still_prints_marker(self):
        """Red-then-green: a non-zero child rc propagates AND the marker
        still prints -- the marker line is not gated on success."""
        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=1, stderr="error: build failed\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                vcpkg.install, "/ws/native/vcpkg", "/tmp/rt/vcpkg-installed",
                "x64-windows-heif", ["de265"], "VCPKG_INSTALL_RC",
            )
        self.assertEqual(rc, 1)
        self.assertIn("VCPKG_INSTALL_RC=1", out)

    # ---- assert_aom_artifact -----------------------------------------------

    def test_assert_aom_artifact_missing_lib_fails_with_exact_message(self):
        """Demonstrated red: no aom.lib at all."""
        prefix = self.tmp / "prefix"
        (prefix / "lib").mkdir(parents=True)

        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout="total 0\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(vcpkg.assert_aom_artifact, str(prefix))
        self.assertEqual(rc, 1)
        self.assertIn(
            f"FAIL: aom.lib absent (expected a static archive) under {prefix}/lib", out
        )

    def test_assert_aom_artifact_missing_copyright_fails_with_exact_message(self):
        """Demonstrated red: aom.lib present, copyright absent."""
        prefix = self.tmp / "prefix"
        (prefix / "lib").mkdir(parents=True)
        (prefix / "lib" / "aom.lib").write_bytes(b"")

        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout="total 0\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(vcpkg.assert_aom_artifact, str(prefix))
        self.assertEqual(rc, 1)
        self.assertIn(
            "FAIL: share/aom/copyright absent -- vendor_licences() needs it for the PATENTS grant",
            out,
        )

    def test_assert_aom_artifact_success_green_after_both_reds(self):
        """Green: both artefacts present, following the two reds above."""
        prefix = self.tmp / "prefix"
        (prefix / "lib").mkdir(parents=True)
        (prefix / "lib" / "aom.lib").write_bytes(b"")
        (prefix / "share" / "aom").mkdir(parents=True)
        (prefix / "share" / "aom" / "copyright").write_bytes(b"")

        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout="total 0\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(vcpkg.assert_aom_artifact, str(prefix))
        self.assertEqual(rc, 0)
        self.assertNotIn("FAIL", out)

    # ---- export_prefix ------------------------------------------------

    def test_export_prefix_appends_never_truncates(self):
        github_env = self.tmp / "github_env"
        github_env.write_text("SOME_OTHER_VAR=1\n")

        rc = vcpkg.export_prefix("/tmp/rt/vcpkg-installed/x64-windows-heif", str(github_env))
        self.assertEqual(rc, 0)
        self.assertEqual(
            github_env.read_text(),
            "SOME_OTHER_VAR=1\nCEYX_VCPKG_PREFIX=/tmp/rt/vcpkg-installed/x64-windows-heif\n",
        )


if __name__ == "__main__":
    unittest.main()
