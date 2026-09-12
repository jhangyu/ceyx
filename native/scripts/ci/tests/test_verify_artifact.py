"""Unit tests for native/scripts/ci/verify_artifact.py (WI-7).

Every sub-check is exercised with a FAKE tool output (no real `file`/`nm`/
`ldd`/`readelf`/child-script invocation) written or returned by a stand-in
for `run.run`, per the plan's own instruction to use fake tool outputs in
`tmp_path`. `assert_exports.run` is patched directly (it is a sibling
module's already-tested entry point; this suite only proves
`verify_artifact.py` calls it correctly and reports its RC).
"""

from __future__ import annotations

import io
import os
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import run as run_module
from .. import verify_artifact

_HERE = Path(__file__).resolve().parent
_GOLDEN_DIR = _HERE / "golden" / "expected"

_SO = "native/build-linux/libdng_decoder_native.so"


def _fake_run_result(returncode=0, stdout="", stderr=""):
    return run_module.RunResult(argv=[], returncode=returncode, stdout=stdout, stderr=stderr)


class _Cwd:
    """Chdir to a tmp directory for the duration of the `with` block --
    verify_artifact.py reads/writes fixed-name files (`nm_dynsyms.txt`,
    `readelf_dynamic.txt`, ...) relative to the process cwd, mirroring the
    real `working-directory: ${{ github.workspace }}` step."""

    def __init__(self, path):
        self._path = str(path)
        self._old = os.getcwd()

    def __enter__(self):
        self._old = os.getcwd()
        os.chdir(self._path)
        return self._path

    def __exit__(self, *_exc):
        os.chdir(self._old)


def _run_captured(func, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = func(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


class VerifyArtifactTests(unittest.TestCase):
    # ---- AC-L2/L3/L4 (verify_artifact) --------------------------------

    def test_ac_l2_file_failure_returns_1(self):
        def fake(argv, cwd=None, env=None):
            if argv[0] == "file":
                return _fake_run_result(returncode=1, stdout="")
            raise AssertionError(f"unexpected argv in this test: {argv}")

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, err = _run_captured(verify_artifact.verify_artifact, "linux")
        self.assertEqual(rc, 1)
        self.assertIn("RC=1", out)
        self.assertIn(f"::error::file '{_SO}' failed (rc=1); the Linux .so is missing.", err)

    def test_ac_l2_file_success_continues_to_l3(self):
        calls = []

        def fake(argv, cwd=None, env=None):
            calls.append(argv[0])
            if argv[0] == "file":
                return _fake_run_result(returncode=0, stdout=f"{_SO}: ELF 64-bit\n")
            if argv[0] == "nm":
                return _fake_run_result(returncode=0, stdout="... T halide_vulkan_device_interface\n")
            if argv[0] == "ldd":
                return _fake_run_result(returncode=0, stdout="libc.so.6 => /lib/libc.so.6\n")
            raise AssertionError(f"unexpected argv: {argv}")

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(verify_artifact.verify_artifact, "linux")
        self.assertEqual(rc, 0)
        self.assertIn("nm", calls)
        self.assertIn("ldd", calls)

    def test_ac_l3_symbol_absent_returns_1(self):
        def fake(argv, cwd=None, env=None):
            if argv[0] == "file":
                return _fake_run_result(returncode=0, stdout=f"{_SO}: ELF\n")
            if argv[0] == "nm":
                return _fake_run_result(returncode=0, stdout="... T some_other_symbol\n")
            raise AssertionError(f"unexpected argv: {argv}")

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, err = _run_captured(verify_artifact.verify_artifact, "linux")
        self.assertEqual(rc, 1)
        self.assertIn("RC=1", out)
        self.assertIn("halide_vulkan_device_interface not found", err)
        self.assertIn("AC-L3", err)

    def test_ac_l4_libvulkan_present_fails_regardless_of_rc(self):
        def fake(argv, cwd=None, env=None):
            if argv[0] == "file":
                return _fake_run_result(returncode=0, stdout=f"{_SO}: ELF\n")
            if argv[0] == "nm":
                return _fake_run_result(returncode=0, stdout="... T halide_vulkan_device_interface\n")
            if argv[0] == "ldd":
                # RC=0 (ldd itself "succeeded") but the output names libvulkan.
                return _fake_run_result(returncode=0, stdout="libvulkan.so.1 => /lib/libvulkan.so.1\n")
            raise AssertionError(f"unexpected argv: {argv}")

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, err = _run_captured(verify_artifact.verify_artifact, "linux")
        self.assertEqual(rc, 1)
        self.assertIn("link-time Vulkan dependency regressed", err)

    def test_ldd_gate_is_substring_not_rc(self):
        """PORTED AS-IS (pre-existing, d33cc607 linux_build.yml:534): a
        nonzero ldd RC with no libvulkan line still PASSES -- the gate is
        the substring test, not ldd's own exit code."""

        def fake(argv, cwd=None, env=None):
            if argv[0] == "file":
                return _fake_run_result(returncode=0, stdout=f"{_SO}: ELF\n")
            if argv[0] == "nm":
                return _fake_run_result(returncode=0, stdout="... T halide_vulkan_device_interface\n")
            if argv[0] == "ldd":
                # ldd itself reports failure (e.g. rc=1), but no libvulkan.
                return _fake_run_result(returncode=1, stdout="libc.so.6 => /lib/libc.so.6\n")
            raise AssertionError(f"unexpected argv: {argv}")

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(verify_artifact.verify_artifact, "linux")
        self.assertEqual(rc, 0, "nonzero ldd RC alone must not fail the gate (ported-as-is)")
        self.assertIn("RC=1", out)

    def test_emission_matches_golden_verify_artifact(self):
        def fake(argv, cwd=None, env=None):
            if argv[0] == "file":
                return _fake_run_result(
                    returncode=0,
                    stdout=f"{_SO}: ELF 64-bit LSB shared object, x86-64, dynamically linked\n",
                )
            if argv[0] == "nm":
                return _fake_run_result(
                    returncode=0,
                    stdout=(
                        "0000000000012340 T halide_vulkan_device_interface\n"
                        "0000000000012350 T other_symbol\n"
                    ),
                )
            if argv[0] == "ldd":
                return _fake_run_result(
                    returncode=0,
                    stdout=(
                        "linux-vdso.so.1 (0x00007fff)\n"
                        "libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f1)\n"
                    ),
                )
            raise AssertionError(f"unexpected argv: {argv}")

        with mock.patch.object(run_module, "run", side_effect=fake), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(verify_artifact.verify_artifact, "linux")
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "verify-artifact-linux.markers").read_text()
        self.assertEqual(out, expected)

    # ---- S-B3 (import_closure) -----------------------------------------

    def test_import_closure_success(self):
        def fake_readelf(argv, cwd=None, env=None):
            return _fake_run_result(
                returncode=0,
                stdout=(
                    "Dynamic section at offset 0x1000 contains 2 entries:\n"
                    " 0x0000000000000001 (NEEDED) Shared library: [libc.so.6]\n"
                ),
            )

        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "readelf":
                return fake_readelf(argv, cwd, env)
            return _fake_run_result(
                returncode=0,
                stdout=(
                    "IMPORT libc.so.6 -> OS_ALLOWLIST\n"
                    "IMPORT_CLOSURE_RESULT=PASS\n"
                    "IMPORT_CLOSURE PASS (linux): 1 imports, 0 staged companions\n"
                ),
            )

        with mock.patch.object(run_module, "run", side_effect=fake_run), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(verify_artifact.import_closure, "linux")
        self.assertEqual(rc, 0)
        self.assertIn("IMPORT_CLOSURE_RC=0", out)

    def test_import_closure_failure_returns_nonzero_and_errors(self):
        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "readelf":
                return _fake_run_result(returncode=0, stdout="Dynamic section ...\n")
            return _fake_run_result(
                returncode=1,
                stdout="IMPORT libfoo.so -> MISSING\nIMPORT_CLOSURE_RESULT=FAIL\n",
            )

        with mock.patch.object(run_module, "run", side_effect=fake_run), _Cwd(self._tmp()):
            rc, out, err = _run_captured(verify_artifact.import_closure, "linux")
        self.assertEqual(rc, 1)
        self.assertIn("IMPORT_CLOSURE_RC=1", out)
        self.assertIn("import-closure gate failed", err)

    def test_emission_matches_golden_import_closure(self):
        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "readelf":
                return _fake_run_result(
                    returncode=0,
                    stdout=(
                        "Dynamic section at offset 0x1000 contains 2 entries:\n"
                        " 0x0000000000000001 (NEEDED) Shared library: [libc.so.6]\n"
                    ),
                )
            return _fake_run_result(
                returncode=0,
                stdout=(
                    "IMPORT libc.so.6 -> OS_ALLOWLIST\n"
                    "IMPORT_CLOSURE_RESULT=PASS\n"
                    "IMPORT_CLOSURE PASS (linux): 1 imports, 0 staged companions\n"
                ),
            )

        with mock.patch.object(run_module, "run", side_effect=fake_run), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(verify_artifact.import_closure, "linux")
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "import-closure-linux.markers").read_text()
        self.assertEqual(out, expected)

    # ---- S-F1 (min_runtime: measure + drift) ---------------------------

    def test_min_runtime_read_rc_is_not_gated(self):
        """C-G4 item 2, PORTED AS-IS: a nonzero READ_MIN_RUNTIME_RC alone
        must not fail the command -- only the drift check can."""

        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "readelf":
                return _fake_run_result(returncode=0, stdout="Symbol table ...\n")
            joined = " ".join(map(str, argv))
            if "read_min_runtime.py" in joined:
                Path("min_runtime.txt").write_text("MIN_RUNTIME_linux=2.35\n")
                # The read step itself "fails" (rc=1)...
                return _fake_run_result(returncode=1, stdout="READ_MIN_RUNTIME_RC=1\n")
            if "assert_min_runtime_matches_declared.py" in joined:
                # ...but the drift check still passes.
                return _fake_run_result(
                    returncode=0,
                    stdout="MIN_RUNTIME_DRIFT_RESULT=PASS (measured=2.35, declared=2.35 from [linux])\n",
                )
            raise AssertionError(f"unexpected argv: {argv}")

        with mock.patch.object(run_module, "run", side_effect=fake_run), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(verify_artifact.min_runtime, "linux")
        self.assertEqual(rc, 0, "a nonzero READ_MIN_RUNTIME_RC alone must not fail min_runtime()")
        self.assertIn("READ_MIN_RUNTIME_RC=1", out)

    def test_min_runtime_drift_failure_returns_nonzero(self):
        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "readelf":
                return _fake_run_result(returncode=0, stdout="Symbol table ...\n")
            joined = " ".join(map(str, argv))
            if "read_min_runtime.py" in joined:
                Path("min_runtime.txt").write_text("MIN_RUNTIME_linux=2.36\n")
                return _fake_run_result(returncode=0, stdout="MIN_RUNTIME_linux=2.36\nREAD_MIN_RUNTIME_RC=0\n")
            if "assert_min_runtime_matches_declared.py" in joined:
                return _fake_run_result(
                    returncode=1,
                    stdout="",
                    stderr="error: measured MIN_RUNTIME_linux=2.36 != declared ...=2.35\n",
                )
            raise AssertionError(f"unexpected argv: {argv}")

        with mock.patch.object(run_module, "run", side_effect=fake_run), _Cwd(self._tmp()):
            rc, out, err = _run_captured(verify_artifact.min_runtime, "linux")
        self.assertEqual(rc, 1)
        # No ::error:: line is manufactured here -- the plan is explicit
        # that none exists in the original for this block.
        self.assertNotIn("::error::", out)
        self.assertNotIn("::error::", err)

    def test_emission_matches_golden_min_runtime(self):
        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "readelf":
                return _fake_run_result(returncode=0, stdout="Symbol table '.dynsym' contains 10 entries:\n")
            joined = " ".join(map(str, argv))
            if "read_min_runtime.py" in joined:
                Path("min_runtime.txt").write_text("MIN_RUNTIME_linux=2.35\n  GLIBC_2.35\n")
                return _fake_run_result(
                    returncode=0,
                    stdout="MIN_RUNTIME_linux=2.35\n  GLIBC_2.35\nREAD_MIN_RUNTIME_RC=0\n",
                )
            if "assert_min_runtime_matches_declared.py" in joined:
                return _fake_run_result(
                    returncode=0,
                    stdout="MIN_RUNTIME_DRIFT_RESULT=PASS (measured=2.35, declared=2.35 from [linux])\n",
                )
            raise AssertionError(f"unexpected argv: {argv}")

        with mock.patch.object(run_module, "run", side_effect=fake_run), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(verify_artifact.min_runtime, "linux")
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "min-runtime-linux.markers").read_text()
        self.assertEqual(out, expected)

    # ---- AC-L5 (assert_exports) -----------------------------------------

    def test_assert_exports_success(self):
        with _Cwd(self._tmp()):
            Path("nm_dynsyms.txt").write_text("... T foo\n")
            with mock.patch.object(verify_artifact, "_assert_exports_script") as fake_script:
                def fake_export_run(manifest_path, platform, dump_text):
                    print("SYMBOL foo -> PRESENT")
                    print("EXPORTS_RESULT=PASS")
                    print("EXPORTS_CHECKED=1")
                    return 0

                fake_script.run.side_effect = fake_export_run
                rc, out, _ = _run_captured(verify_artifact.assert_exports, "linux")
        self.assertEqual(rc, 0)
        self.assertIn("ASSERT_EXPORTS_RC=0", out)

    def test_assert_exports_failure_returns_nonzero_and_errors(self):
        with _Cwd(self._tmp()):
            Path("nm_dynsyms.txt").write_text("... T foo\n")
            with mock.patch.object(verify_artifact, "_assert_exports_script") as fake_script:
                def fake_export_run(manifest_path, platform, dump_text):
                    print("SYMBOL bar -> MISSING")
                    print("EXPORTS_RESULT=FAIL")
                    print("EXPORTS_CHECKED=1")
                    return 1

                fake_script.run.side_effect = fake_export_run
                rc, out, err = _run_captured(verify_artifact.assert_exports, "linux")
        self.assertEqual(rc, 1)
        self.assertIn("ASSERT_EXPORTS_RC=1", out)
        self.assertIn("does not export the FFI surface", err)

    def test_emission_matches_golden_assert_exports(self):
        with _Cwd(self._tmp()):
            Path("nm_dynsyms.txt").write_text("... T foo\n")
            with mock.patch.object(verify_artifact, "_assert_exports_script") as fake_script:
                def fake_export_run(manifest_path, platform, dump_text):
                    print("SYMBOL foo -> PRESENT")
                    print("EXPORTS_RESULT=PASS")
                    print("EXPORTS_CHECKED=1")
                    return 0

                fake_script.run.side_effect = fake_export_run
                rc, out, _ = _run_captured(verify_artifact.assert_exports, "linux")
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "assert-exports-linux.markers").read_text()
        self.assertEqual(out, expected)

    # ---- AVX-512 wrapper --------------------------------------------------

    def test_avx512_failure_returns_1_not_rc(self):
        """rc=2 from the child ("could not run at all") must still yield
        return 1 from the wrapper -- ported as-is, the shell always
        `exit 1` regardless of the child's actual rc."""

        def fake_run(argv, cwd=None, env=None):
            return _fake_run_result(returncode=2, stdout="error: objdump not found\n")

        with mock.patch.object(run_module, "run", side_effect=fake_run), _Cwd(self._tmp()):
            rc, out, err = _run_captured(verify_artifact.assert_no_avx512, "linux")
        self.assertEqual(rc, 1)
        self.assertIn("RC=2 (avx512 gate)", out)
        self.assertIn("rc=2 means the check could not run at all", err)

    def test_avx512_success(self):
        def fake_run(argv, cwd=None, env=None):
            return _fake_run_result(
                returncode=0,
                stdout="AVX512-GATE: 0 evex lines found in libdng_decoder_native.so (PASS)\n",
            )

        with mock.patch.object(run_module, "run", side_effect=fake_run), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(verify_artifact.assert_no_avx512, "linux")
        self.assertEqual(rc, 0)
        self.assertIn("RC=0 (avx512 gate)", out)

    def test_emission_matches_golden_assert_no_avx512(self):
        def fake_run(argv, cwd=None, env=None):
            return _fake_run_result(
                returncode=0,
                stdout="AVX512-GATE: 0 evex lines found in libdng_decoder_native.so (PASS)\n",
            )

        with mock.patch.object(run_module, "run", side_effect=fake_run), _Cwd(self._tmp()):
            rc, out, _ = _run_captured(verify_artifact.assert_no_avx512, "linux")
        self.assertEqual(rc, 0)
        expected = (_GOLDEN_DIR / "assert-no-avx512-linux.markers").read_text()
        self.assertEqual(out, expected)

    # ---- helpers ------------------------------------------------------

    def setUp(self):
        import tempfile

        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)

    def _tmp(self):
        return self._tmpdir.name


if __name__ == "__main__":
    unittest.main()
