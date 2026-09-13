"""Unit tests for native/scripts/ci/provision.py (push 8, WI-22)."""

from __future__ import annotations

import io
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import provision
from .. import run as run_module


def _fake_run_result(returncode=0, stdout="", stderr=""):
    return run_module.RunResult(argv=[], returncode=returncode, stdout=stdout, stderr=stderr)


def _run_captured(func, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = func(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)
        self.tmp = Path(self._tmpdir.name)

    # ---- vcpkg_baseline ----------------------------------------------

    def test_vcpkg_baseline_reads_json_and_appends_to_github_env(self):
        vcpkg_json = self.tmp / "vcpkg.json"
        vcpkg_json.write_text('{"builtin-baseline": "abc123"}')
        github_env = self.tmp / "github_env"
        github_env.write_text("")

        rc = provision.vcpkg_baseline(str(vcpkg_json), str(github_env))
        self.assertEqual(rc, 0)
        self.assertEqual(github_env.read_text(), "VCPKG_BASELINE=abc123\n")

    # ---- vcpkg_bootstrap ------------------------------------------------

    def test_vcpkg_bootstrap_runs_clone_checkout_bootstrap_version_in_order(self):
        calls = []

        def fake(argv, cwd=None, env=None):
            calls.append(argv[0] if argv[0] != "git" else f"git:{argv[2] if len(argv) > 2 else ''}")
            return _fake_run_result(returncode=0, stdout="ok\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(provision.vcpkg_bootstrap, "abc123", str(self.tmp))
        self.assertEqual(rc, 0)
        # git clone, git checkout, bootstrap-vcpkg.sh, vcpkg version -- in order.
        self.assertEqual(len(calls), 4)

    def test_vcpkg_bootstrap_clone_failure_short_circuits(self):
        def fake(argv, cwd=None, env=None):
            if argv[0] == "git" and "clone" in argv:
                return _fake_run_result(returncode=128, stderr="fatal: could not resolve host\n")
            raise AssertionError(f"should not reach: {argv}")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, err = _run_captured(provision.vcpkg_bootstrap, "abc123", str(self.tmp))
        self.assertEqual(rc, 128)
        self.assertIn("git clone", err)

    # ---- vcpkg_install ----------------------------------------------------

    def test_vcpkg_install_builds_correct_argv(self):
        captured_argv = []

        def fake(argv, cwd=None, env=None):
            captured_argv.extend(argv)
            return _fake_run_result(returncode=0, stdout="Installing...\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.vcpkg_install, "x64-linux-heif", "/ws", "/tmp/rt", ["de265", "aom"]
            )
        self.assertEqual(rc, 0)
        self.assertIn("VCPKG_INSTALL_RC=0", out)
        joined = " ".join(captured_argv)
        self.assertIn("--x-manifest-root=/ws/native/vcpkg", joined)
        self.assertIn("--x-install-root=/tmp/rt/vcpkg-installed", joined)
        self.assertIn("--triplet=x64-linux-heif", joined)
        self.assertIn("--x-no-default-features", joined)
        self.assertIn("--x-feature=de265", joined)
        self.assertIn("--x-feature=aom", joined)

    def test_vcpkg_install_failure_reports_rc(self):
        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=1, stderr="error: build failed\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.vcpkg_install, "x64-linux-heif", "/ws", "/tmp/rt", ["de265"]
            )
        self.assertEqual(rc, 1)
        self.assertIn("VCPKG_INSTALL_RC=1", out)

    # ---- assert_vcpkg_artefacts --------------------------------------------

    def test_assert_vcpkg_artefacts_rejects_non_linux(self):
        with self.assertRaises(ValueError):
            provision.assert_vcpkg_artefacts("macos", "arm64-osx-heif", str(self.tmp))

    def test_assert_vcpkg_artefacts_success(self):
        lib_dir = self.tmp / "vcpkg-installed" / "x64-linux-heif" / "lib"
        lib_dir.mkdir(parents=True)
        (lib_dir / "libwebp.a").write_bytes(b"")

        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout="total 0\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.assert_vcpkg_artefacts, "linux", "x64-linux-heif", str(self.tmp)
            )
        self.assertEqual(rc, 0)

    def test_assert_vcpkg_artefacts_missing_static_lib_fails(self):
        lib_dir = self.tmp / "vcpkg-installed" / "x64-linux-heif" / "lib"
        lib_dir.mkdir(parents=True)

        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout="total 0\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.assert_vcpkg_artefacts, "linux", "x64-linux-heif", str(self.tmp)
            )
        self.assertEqual(rc, 1)
        self.assertIn("FAIL: libwebp.a absent", out)

    def test_assert_vcpkg_artefacts_shared_lib_present_fails(self):
        lib_dir = self.tmp / "vcpkg-installed" / "x64-linux-heif" / "lib"
        lib_dir.mkdir(parents=True)
        (lib_dir / "libwebp.a").write_bytes(b"")
        (lib_dir / "libwebp.so.1").write_bytes(b"")

        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout="total 0\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.assert_vcpkg_artefacts, "linux", "x64-linux-heif", str(self.tmp)
            )
        self.assertEqual(rc, 1)
        self.assertIn("libwebp shared object(s) present", out)

    # ---- verify_interpreter ------------------------------------------------

    def test_verify_interpreter_rejects_hostedtoolcache(self):
        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout="/opt/hostedtoolcache/Python/3.11.16/x64/bin/python3\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, err = _run_captured(provision.verify_interpreter, True)
        self.assertEqual(rc, 1)
        self.assertIn("::error::", err)
        self.assertIn("hostedtoolcache", err)

    def test_verify_interpreter_accepts_system_python(self):
        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout="/usr/bin/python3\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(provision.verify_interpreter, True)
        self.assertEqual(rc, 0)
        self.assertIn("interpreter alive:", out)

    # ---- ensure_cmake -------------------------------------------------

    def test_ensure_cmake_passes_when_version_is_new_enough(self):
        def fake(argv, cwd=None, env=None):
            if argv[0] == "cmake":
                return _fake_run_result(returncode=0, stdout="cmake version 3.30.1\n")
            return _fake_run_result(returncode=0, stdout="Successfully installed cmake\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(provision.ensure_cmake, "3.28")
        self.assertEqual(rc, 0)

    def test_ensure_cmake_fails_when_version_is_too_old(self):
        def fake(argv, cwd=None, env=None):
            if argv[0] == "cmake":
                return _fake_run_result(returncode=0, stdout="cmake version 3.22.1\n")
            return _fake_run_result(returncode=0, stdout="Successfully installed cmake\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, err = _run_captured(provision.ensure_cmake, "3.28")
        self.assertEqual(rc, 1)
        self.assertIn("older than 3.28", err)

    def test_ensure_cmake_pip_failure_short_circuits(self):
        def fake(argv, cwd=None, env=None):
            return _fake_run_result(returncode=1, stderr="ERROR: could not find a version\n")

        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, err = _run_captured(provision.ensure_cmake, "3.28")
        self.assertEqual(rc, 1)
        self.assertIn("pip install cmake", err)


if __name__ == "__main__":
    unittest.main()
