"""Unit tests for native/scripts/ci/provision.py (push 8, WI-22)."""

from __future__ import annotations

import io
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

import sys

from .. import provision
from .. import run as run_module

_REPO_ROOT = Path(__file__).resolve().parents[4]
_CI_ENTRYPOINT = _REPO_ROOT / "native" / "scripts" / "ci.py"


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

    def test_assert_vcpkg_artefacts_rejects_android_by_name(self):
        """Named pin, not just implied by the generic non-linux case above:
        this function's platform gate has already been silently widened
        once this push (linux-only -> {linux, macos}); a test naming
        android specifically fails loudly the next time someone widens the
        set without checking whether android actually has a ported shape
        (it doesn't -- no android vcpkg-artefact step exists in any
        workflow, confirmed by grep)."""
        with self.assertRaises(ValueError):
            provision.assert_vcpkg_artefacts("android", "arm64-v8a-android-heif", str(self.tmp))

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

    # ---- assert_vcpkg_artefacts (macOS) ------------------------------------
    # Three libraries, three different properties -- one negative test per
    # assertion, not per library, so a shared green can't hide a broken one.

    def _macos_lib_dir(self):
        lib_dir = self.tmp / "vcpkg-installed" / "arm64-osx-heif" / "lib"
        lib_dir.mkdir(parents=True)
        return lib_dir

    def _lipo_fake(self, archs_by_name):
        def fake(argv, cwd=None, env=None):
            if argv[0] == "lipo":
                name = Path(argv[-1]).name
                return _fake_run_result(returncode=0, stdout=archs_by_name.get(name, ""))
            return _fake_run_result(returncode=0, stdout="total 0\n")

        return fake

    def test_assert_vcpkg_artefacts_macos_rejects_missing_arch_tag(self):
        with self.assertRaises(ValueError):
            provision.assert_vcpkg_artefacts("macos", "arm64-osx-heif", str(self.tmp))

    def test_assert_vcpkg_artefacts_macos_all_correct_passes(self):
        lib_dir = self._macos_lib_dir()
        (lib_dir / "libwebp.a").write_bytes(b"")
        (lib_dir / "libde265.dylib").write_bytes(b"")
        (lib_dir / "libaom.a").write_bytes(b"")
        fake = self._lipo_fake({"libwebp.a": "arm64", "libde265.dylib": "arm64", "libaom.a": "arm64"})
        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.assert_vcpkg_artefacts, "macos", "arm64-osx-heif", str(self.tmp), "arm64"
            )
        self.assertEqual(rc, 0)

    def test_assert_vcpkg_artefacts_macos_libwebp_dylib_present_fails(self):
        lib_dir = self._macos_lib_dir()
        (lib_dir / "libwebp.a").write_bytes(b"")
        (lib_dir / "libwebp.1.dylib").write_bytes(b"")
        (lib_dir / "libde265.dylib").write_bytes(b"")
        (lib_dir / "libaom.a").write_bytes(b"")
        fake = self._lipo_fake({"libwebp.a": "arm64", "libde265.dylib": "arm64"})
        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.assert_vcpkg_artefacts, "macos", "arm64-osx-heif", str(self.tmp), "arm64"
            )
        self.assertEqual(rc, 1)
        self.assertIn("libwebp dylib(s) present", out)

    def test_assert_vcpkg_artefacts_macos_libwebp_wrong_arch_fails(self):
        lib_dir = self._macos_lib_dir()
        (lib_dir / "libwebp.a").write_bytes(b"")
        (lib_dir / "libde265.dylib").write_bytes(b"")
        (lib_dir / "libaom.a").write_bytes(b"")
        fake = self._lipo_fake({"libwebp.a": "x86_64", "libde265.dylib": "arm64"})
        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.assert_vcpkg_artefacts, "macos", "arm64-osx-heif", str(self.tmp), "arm64"
            )
        self.assertEqual(rc, 1)
        self.assertIn("libwebp.a archs 'x86_64' do not include arm64", out)

    def test_assert_vcpkg_artefacts_macos_libde265_not_shared_fails(self):
        lib_dir = self._macos_lib_dir()
        (lib_dir / "libwebp.a").write_bytes(b"")
        (lib_dir / "libaom.a").write_bytes(b"")
        fake = self._lipo_fake({"libwebp.a": "arm64"})
        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.assert_vcpkg_artefacts, "macos", "arm64-osx-heif", str(self.tmp), "arm64"
            )
        self.assertEqual(rc, 1)
        self.assertIn("no libde265 dylib", out)
        self.assertIn("A5.2", out)

    def test_assert_vcpkg_artefacts_macos_libde265_still_static_fails(self):
        lib_dir = self._macos_lib_dir()
        (lib_dir / "libwebp.a").write_bytes(b"")
        (lib_dir / "libde265.dylib").write_bytes(b"")
        (lib_dir / "libde265.a").write_bytes(b"")
        (lib_dir / "libaom.a").write_bytes(b"")
        fake = self._lipo_fake({"libwebp.a": "arm64", "libde265.dylib": "arm64"})
        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.assert_vcpkg_artefacts, "macos", "arm64-osx-heif", str(self.tmp), "arm64"
            )
        self.assertEqual(rc, 1)
        self.assertIn("libde265.a present", out)
        self.assertIn("A5.2", out)

    def test_assert_vcpkg_artefacts_macos_libde265_wrong_arch_fails(self):
        lib_dir = self._macos_lib_dir()
        (lib_dir / "libwebp.a").write_bytes(b"")
        (lib_dir / "libde265.dylib").write_bytes(b"")
        (lib_dir / "libaom.a").write_bytes(b"")
        fake = self._lipo_fake({"libwebp.a": "arm64", "libde265.dylib": "x86_64"})
        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.assert_vcpkg_artefacts, "macos", "arm64-osx-heif", str(self.tmp), "arm64"
            )
        self.assertEqual(rc, 1)
        self.assertIn("libde265 archs 'x86_64' do not include arm64", out)

    def test_assert_vcpkg_artefacts_macos_aom_missing_static_fails(self):
        lib_dir = self._macos_lib_dir()
        (lib_dir / "libwebp.a").write_bytes(b"")
        (lib_dir / "libde265.dylib").write_bytes(b"")
        (lib_dir / "libaom.dylib").write_bytes(b"")
        fake = self._lipo_fake({"libwebp.a": "arm64", "libde265.dylib": "arm64"})
        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.assert_vcpkg_artefacts, "macos", "arm64-osx-heif", str(self.tmp), "arm64"
            )
        self.assertEqual(rc, 1)
        self.assertIn("libaom.a absent", out)

    def test_assert_vcpkg_artefacts_macos_aom_dylib_present_fails(self):
        lib_dir = self._macos_lib_dir()
        (lib_dir / "libwebp.a").write_bytes(b"")
        (lib_dir / "libde265.dylib").write_bytes(b"")
        (lib_dir / "libaom.a").write_bytes(b"")
        (lib_dir / "libaom.dylib").write_bytes(b"")
        fake = self._lipo_fake({"libwebp.a": "arm64", "libde265.dylib": "arm64"})
        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.assert_vcpkg_artefacts, "macos", "arm64-osx-heif", str(self.tmp), "arm64"
            )
        self.assertEqual(rc, 1)
        self.assertIn("libaom dylib(s) present", out)

    def test_assert_vcpkg_artefacts_macos_aom_wrong_arch_fails(self):
        """CORRECTED 2026-09-13 (lead8-pyci-opus ruling, parity restoration):
        this test used to assert the OPPOSITE -- that a wrong-arch aom
        .a still passed, on a false premise that the shell never checked
        aom's arch. macos_build.yml:364-367 has always run `lipo -archs`
        against libaom.a and failed on a mismatch (the shell's own comment:
        aom follows the HOST cpu on the cross leg unless AOM_TARGET_CPU is
        forced). The old test pinned a dropped check and made it look
        deliberate; this one pins the restored check instead."""
        lib_dir = self._macos_lib_dir()
        (lib_dir / "libwebp.a").write_bytes(b"")
        (lib_dir / "libde265.dylib").write_bytes(b"")
        (lib_dir / "libaom.a").write_bytes(b"")
        fake = self._lipo_fake({"libwebp.a": "arm64", "libde265.dylib": "arm64", "libaom.a": "x86_64"})
        with mock.patch.object(run_module, "run", side_effect=fake):
            rc, out, _ = _run_captured(
                provision.assert_vcpkg_artefacts, "macos", "arm64-osx-heif", str(self.tmp), "arm64"
            )
        self.assertEqual(rc, 1)
        self.assertIn("libaom.a archs 'x86_64' do not include arm64", out)

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


class TestAssertVcpkgArtefactsBareScriptInvocation(unittest.TestCase):
    """One real invocation through ci.py itself. macOS was gated out at
    ci.py's dispatch layer when this class was first written -- that gate
    has since been widened (impl-16, `7d44807d`, --arch-tag wired) and this
    class was updated to match: macOS now gets a REAL success-path
    invocation (genuine subprocess `lipo -archs` against real Mach-O
    binaries, not mocked), and the rejection-path coverage moved to
    windows, which is still correctly excluded. A test that starts failing
    because the behaviour it pinned changed ON PURPOSE is a correct test,
    not a broken one -- this is that case, not a case of "fix the test to
    match", so the fix here is retargeting, not loosening."""

    def _run_ci(self, *argv: str):
        return run_module.run([sys.executable, str(_CI_ENTRYPOINT), *argv], cwd=str(_REPO_ROOT))

    def test_assert_vcpkg_artefacts_windows_is_cleanly_rejected_by_cli(self) -> None:
        # windows has no vcpkg-artefact shape ported at all (neither
        # linux's .so-absence glob nor macOS's three-property dylib/lipo
        # check) -- still correctly excluded, so this is the rejection-path
        # coverage this class always existed to provide: a clean
        # ::error::/exit 2, never an uncaught traceback.
        result = self._run_ci(
            "assert-vcpkg-artefacts", "--platform", "windows",
            "--triplet", "x64-windows-heif", "--runner-temp", "/tmp/does-not-matter",
        )
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("Traceback", result.stderr)
        self.assertIn("::error::", result.stderr)
        self.assertIn("no --platform 'windows' leg", result.stderr)

    def test_assert_vcpkg_artefacts_android_is_cleanly_rejected_by_cli(self) -> None:
        # Named pin through the real CLI too (see the module-level test's
        # docstring for why "implied by the generic case" isn't enough).
        result = self._run_ci(
            "assert-vcpkg-artefacts", "--platform", "android",
            "--triplet", "arm64-v8a-android-heif", "--runner-temp", "/tmp/does-not-matter",
        )
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("Traceback", result.stderr)
        self.assertIn("::error::", result.stderr)
        self.assertIn("no --platform 'android' leg", result.stderr)

    def test_assert_vcpkg_artefacts_macos_success_path_through_real_cli(self) -> None:
        # Real subprocess call, real `lipo -archs` against real Mach-O
        # binaries (a copy of /usr/bin/true stands in for each staged
        # library -- same shape impl-16 used to prove ci.py's own wiring),
        # not the mocked `run.run` the unit tests above use. This is what
        # was previously impossible: the CLI rejected --platform macos
        # outright, so no test could reach this branch through ci.py.
        true_bin = Path("/usr/bin/true")
        if not true_bin.is_file():
            self.skipTest("/usr/bin/true not present on this host")
        archs = run_module.run(["lipo", "-archs", str(true_bin)]).stdout.split()
        if not archs:
            self.skipTest("lipo -archs produced no output for /usr/bin/true on this host")
        arch_tag = archs[0]

        d = Path(tempfile.mkdtemp())
        lib_dir = d / "vcpkg-installed" / "arm64-osx-heif" / "lib"
        lib_dir.mkdir(parents=True)
        import shutil

        shutil.copy(true_bin, lib_dir / "libwebp.a")
        shutil.copy(true_bin, lib_dir / "libde265.dylib")
        shutil.copy(true_bin, lib_dir / "libaom.a")

        result = self._run_ci(
            "assert-vcpkg-artefacts", "--platform", "macos",
            "--triplet", "arm64-osx-heif", "--runner-temp", str(d),
            "--arch-tag", arch_tag,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_assert_vcpkg_artefacts_macos_aom_wrong_arch_rejected_by_real_cli(self) -> None:
        # Same real-subprocess technique, but requesting an --arch-tag that
        # does NOT appear in /usr/bin/true's real archs -- exercises the
        # restored aom arch check's REJECT path end to end through the
        # actual CLI, not just the mocked unit test above.
        true_bin = Path("/usr/bin/true")
        if not true_bin.is_file():
            self.skipTest("/usr/bin/true not present on this host")
        archs = run_module.run(["lipo", "-archs", str(true_bin)]).stdout.split()
        if not archs:
            self.skipTest("lipo -archs produced no output for /usr/bin/true on this host")
        bogus_arch_tag = "not-a-real-arch-" + "".join(archs)

        d = Path(tempfile.mkdtemp())
        lib_dir = d / "vcpkg-installed" / "arm64-osx-heif" / "lib"
        lib_dir.mkdir(parents=True)
        import shutil

        shutil.copy(true_bin, lib_dir / "libwebp.a")
        shutil.copy(true_bin, lib_dir / "libde265.dylib")
        shutil.copy(true_bin, lib_dir / "libaom.a")

        result = self._run_ci(
            "assert-vcpkg-artefacts", "--platform", "macos",
            "--triplet", "arm64-osx-heif", "--runner-temp", str(d),
            "--arch-tag", bogus_arch_tag,
        )
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("Traceback", result.stderr)
        self.assertIn(f"do not include {bogus_arch_tag}", result.stdout)


if __name__ == "__main__":
    unittest.main()
