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

    # S-F1 (min_runtime) direct-behaviour tests deleted here (P-10, push 7):
    # `verify_artifact.min_runtime` itself was deleted as an orphaned
    # duplicate of `ci/minruntime.py`'s four-platform generalisation -- see
    # verify_artifact.py's comment at the old function's former location.
    # Equivalent coverage (READ_MIN_RUNTIME_RC ungated, drift-failure RC,
    # golden marker match) lives in test_minruntime.py against the real
    # (non-orphaned) implementation.

    # ---- macOS verify_artifact (file exists + lipo arch match) -----------

    def test_macos_verify_artifact_missing_dylib_fails_no_stderr_redirect(self):
        with _Cwd(self._tmp()):
            rc, out, err = _run_captured(
                verify_artifact.verify_artifact,
                "macos", "arm64", dylib_path="does/not/exist.dylib",
            )
        self.assertEqual(rc, 1)
        self.assertIn("::error::Expected dylib not found", out)
        self.assertEqual(err, "")

    def test_macos_verify_artifact_arch_mismatch_fails(self):
        with _Cwd(self._tmp()):
            dylib = Path(self._tmp()) / "libdng_decoder_native.dylib"
            dylib.write_bytes(b"")

            def fake(argv, cwd=None, env=None):
                if argv[0] == "file":
                    return _fake_run_result(returncode=0, stdout=f"{dylib}: Mach-O\n")
                if argv[0] == "lipo":
                    return _fake_run_result(returncode=0, stdout="x86_64\n")
                raise AssertionError(f"unexpected argv: {argv}")

            with mock.patch.object(run_module, "run", side_effect=fake):
                rc, out, err = _run_captured(
                    verify_artifact.verify_artifact, "macos", "arm64", dylib_path=str(dylib)
                )
        self.assertEqual(rc, 1)
        self.assertIn("Expected architecture 'arm64' but the dylib reports 'x86_64'", out)
        self.assertEqual(err, "")

    def test_macos_verify_artifact_success(self):
        with _Cwd(self._tmp()):
            dylib = Path(self._tmp()) / "libdng_decoder_native.dylib"
            dylib.write_bytes(b"")

            def fake(argv, cwd=None, env=None):
                if argv[0] == "file":
                    return _fake_run_result(returncode=0, stdout=f"{dylib}: Mach-O\n")
                if argv[0] == "lipo":
                    return _fake_run_result(returncode=0, stdout="arm64\n")
                if argv[0] == "otool":
                    return _fake_run_result(returncode=0, stdout=f"{dylib}:\n\t@rpath/libheif.1.dylib\n")
                raise AssertionError(f"unexpected argv: {argv}")

            with mock.patch.object(run_module, "run", side_effect=fake):
                rc, out, _ = _run_captured(
                    verify_artifact.verify_artifact, "macos", "arm64", dylib_path=str(dylib)
                )
        self.assertEqual(rc, 0)
        self.assertIn("lipo -archs => arm64", out)

    # ---- Windows verify_artifact (file exists + non-zero size) -----------

    def test_windows_verify_artifact_missing_dll_fails_with_stderr(self):
        with _Cwd(self._tmp()):
            rc, out, err = _run_captured(verify_artifact.verify_artifact, "windows")
        self.assertEqual(rc, 1)
        self.assertIn("::error::", err)

    def test_windows_verify_artifact_zero_byte_dll_fails(self):
        with _Cwd(self._tmp()):
            so = Path(_SO)  # "native/build-windows/dng_decoder_native.dll" via targets.py
            with mock.patch.object(verify_artifact, "_artifact_path", return_value=str(so)):
                so.parent.mkdir(parents=True, exist_ok=True)
                so.write_bytes(b"")

                def fake(argv, cwd=None, env=None):
                    if argv[0] == "file":
                        return _fake_run_result(returncode=0, stdout="data\n")
                    raise AssertionError(f"unexpected argv: {argv}")

                with mock.patch.object(run_module, "run", side_effect=fake):
                    rc, out, err = _run_captured(verify_artifact.verify_artifact, "windows")
        self.assertEqual(rc, 1)
        self.assertIn("DLL_SIZE_BYTES=0", out)
        self.assertIn("::error::DLL is zero bytes.", err)

    def test_windows_verify_artifact_success(self):
        with _Cwd(self._tmp()):
            so = Path("native/build-windows/dng_decoder_native.dll")
            with mock.patch.object(verify_artifact, "_artifact_path", return_value=str(so)):
                so.parent.mkdir(parents=True, exist_ok=True)
                so.write_bytes(b"not empty")

                def fake(argv, cwd=None, env=None):
                    if argv[0] == "file":
                        return _fake_run_result(returncode=0, stdout="PE32+ executable\n")
                    raise AssertionError(f"unexpected argv: {argv}")

                with mock.patch.object(run_module, "run", side_effect=fake):
                    rc, out, _ = _run_captured(verify_artifact.verify_artifact, "windows")
        self.assertEqual(rc, 0)
        self.assertIn("DLL_SIZE_BYTES=9", out)

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

    # ---- macOS staged-companion gates (arch + reachability + rpath) -------

    def _macos_companions(self):
        import read_shipped_files

        return read_shipped_files.load_declaration()["macos"]["companions"]

    def test_staged_companions_all_gates_pass(self):
        companions = self._macos_companions()
        dylib = "native/build-macos-arm64/libdng_decoder_native.dylib"

        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "lipo":
                return _fake_run_result(returncode=0, stdout="arm64\n")
            raise AssertionError(f"unexpected run(): {argv}")

        def fake_run_to_file(argv, out_path, cwd=None, env=None):
            self.assertEqual(argv[0], "otool")
            lines = [dylib, "\t@rpath/libdng_decoder_native.dylib (compatibility ...)"]
            for c in companions:
                lines.append(f"\t@rpath/{c} (compatibility version 1.0.0)")
            Path(out_path).write_text("\n".join(lines) + "\n")
            return _fake_run_result(returncode=0)

        with mock.patch.object(run_module, "run", side_effect=fake_run), \
             mock.patch.object(run_module, "run_to_file", side_effect=fake_run_to_file), \
             _Cwd(self._tmp()):
            for c in companions:
                (Path(self._tmp()) / c).touch()
            Path("artifacts/native").mkdir(parents=True, exist_ok=True)
            for c in companions:
                (Path("artifacts/native") / c).touch()
            rc, out, _ = _run_captured(
                verify_artifact.verify_staged_companions,
                "macos", dylib, "artifacts", "arm64",
            )
        self.assertEqual(rc, 0)
        self.assertIn("Gate 1: architecture", out)
        self.assertIn("Gates 2+3: reachability", out)
        for c in companions:
            self.assertIn(f"dependency line for {c}:", out)

    def test_staged_companions_arch_mismatch_fails_no_stderr_redirect(self):
        companions = self._macos_companions()
        dylib = "native/build-macos-arm64/libdng_decoder_native.dylib"

        def fake_run(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout="x86_64\n")

        with mock.patch.object(run_module, "run", side_effect=fake_run), _Cwd(self._tmp()):
            Path("artifacts/native").mkdir(parents=True, exist_ok=True)
            for c in companions:
                (Path("artifacts/native") / c).touch()
            rc, out, err = _run_captured(
                verify_artifact.verify_staged_companions,
                "macos", dylib, "artifacts", "arm64",
            )
        self.assertEqual(rc, 1)
        # PORTED AS-IS: this step's error lines carry no `>&2` in the
        # original shell, unlike every other error in this module.
        self.assertIn("::error::", out)
        self.assertEqual(err, "")

    def test_staged_companions_unlinked_companion_fails(self):
        companions = self._macos_companions()
        dylib = "native/build-macos-arm64/libdng_decoder_native.dylib"

        def fake_run(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout="arm64\n")

        def fake_run_to_file(argv, out_path, cwd=None, env=None):
            # First companion is simply absent from the dependency graph.
            lines = [dylib, "\t@rpath/libdng_decoder_native.dylib (compatibility ...)"]
            for c in companions[1:]:
                lines.append(f"\t@rpath/{c} (compatibility version 1.0.0)")
            Path(out_path).write_text("\n".join(lines) + "\n")
            return _fake_run_result(returncode=0)

        with mock.patch.object(run_module, "run", side_effect=fake_run), \
             mock.patch.object(run_module, "run_to_file", side_effect=fake_run_to_file), \
             _Cwd(self._tmp()):
            Path("artifacts/native").mkdir(parents=True, exist_ok=True)
            for c in companions:
                (Path("artifacts/native") / c).touch()
            rc, out, err = _run_captured(
                verify_artifact.verify_staged_companions,
                "macos", dylib, "artifacts", "arm64",
            )
        self.assertEqual(rc, 1)
        self.assertIn(f"does not depend on {companions[0]} at all", out)
        self.assertEqual(err, "")

    def test_staged_companions_non_rpath_reference_fails(self):
        companions = self._macos_companions()
        dylib = "native/build-macos-arm64/libdng_decoder_native.dylib"

        def fake_run(argv, cwd=None, env=None):
            return _fake_run_result(returncode=0, stdout="arm64\n")

        def fake_run_to_file(argv, out_path, cwd=None, env=None):
            lines = [dylib, "\t@rpath/libdng_decoder_native.dylib (compatibility ...)"]
            # First companion is reachable but via an absolute path, not @rpath.
            lines.append(f"\t/opt/homebrew/lib/{companions[0]} (compatibility version 1.0.0)")
            for c in companions[1:]:
                lines.append(f"\t@rpath/{c} (compatibility version 1.0.0)")
            Path(out_path).write_text("\n".join(lines) + "\n")
            return _fake_run_result(returncode=0)

        with mock.patch.object(run_module, "run", side_effect=fake_run), \
             mock.patch.object(run_module, "run_to_file", side_effect=fake_run_to_file), \
             _Cwd(self._tmp()):
            Path("artifacts/native").mkdir(parents=True, exist_ok=True)
            for c in companions:
                (Path("artifacts/native") / c).touch()
            rc, out, err = _run_captured(
                verify_artifact.verify_staged_companions,
                "macos", dylib, "artifacts", "arm64",
            )
        self.assertEqual(rc, 1)
        self.assertIn("non-relative reference", out)
        self.assertEqual(err, "")

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
