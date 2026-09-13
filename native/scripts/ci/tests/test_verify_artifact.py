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

    # ---- S-B3 android (WI-34, push 8 follow-on) -------------------------

    def _android_dirs(self, decoder_name: str | None = "libdng_decoder_native.so"):
        base = Path(self._tmp())
        ndk_home = base / "ndk"
        llvm_readelf = ndk_home / "toolchains" / "llvm" / "prebuilt" / "linux-x86_64" / "bin" / "llvm-readelf"
        llvm_readelf.parent.mkdir(parents=True)
        llvm_readelf.write_bytes(b"")
        llvm_readelf.chmod(0o755)
        artifact_dir = base / "artifacts"
        native_dir = artifact_dir / "native"
        native_dir.mkdir(parents=True)
        if decoder_name:
            (native_dir / decoder_name).write_bytes(b"")
        return str(artifact_dir), str(ndk_home)

    def test_import_closure_android_requires_artifact_dir_and_ndk_home(self):
        """Direct-call contract: the module itself raises, independent of
        whatever the CLI layer's argparse enforcement does (ci.py's
        `_enforce_import_closure_flags` is a separate, CLI-only guard)."""
        with self.assertRaises(ValueError):
            verify_artifact.import_closure("android")
        with self.assertRaises(ValueError):
            verify_artifact.import_closure("android", artifact_dir="/x")
        with self.assertRaises(ValueError):
            verify_artifact.import_closure("android", ndk_home="/y")

    def test_import_closure_android_missing_llvm_readelf_errors(self):
        artifact_dir, ndk_home = self._android_dirs()
        # Remove the fixture's llvm-readelf so the not-found branch fires.
        import os as _os

        _os.remove(
            Path(ndk_home) / "toolchains" / "llvm" / "prebuilt" / "linux-x86_64" / "bin" / "llvm-readelf"
        )
        rc, _, err = _run_captured(
            verify_artifact.import_closure, "android", artifact_dir=artifact_dir, ndk_home=ndk_home
        )
        self.assertEqual(rc, 1)
        self.assertIn("::error::llvm-readelf not found at", err)

    def test_import_closure_android_no_decoder_errors(self):
        """R19-shaped negative: no `libdng_decoder_native*.so` under
        <artifact_dir>/native. ADDED, not a port -- the source shell
        (android_build.yml:422) has no equivalent guard and would fall
        through to readelf on an empty/garbage path; flagged in the
        module docstring as a deliberate addition, not silently decided
        equivalent."""
        artifact_dir, ndk_home = self._android_dirs(decoder_name=None)
        rc, _, err = _run_captured(
            verify_artifact.import_closure, "android", artifact_dir=artifact_dir, ndk_home=ndk_home
        )
        self.assertEqual(rc, 1)
        self.assertIn("no libdng_decoder_native*.so found under", err)

    def test_import_closure_android_success_has_no_section_banner_or_dump_echo(self):
        """PORTED AS-IS pin: android_build.yml:423 redirects llvm-readelf's
        output straight into a file with no `cat`/`echo` of the dump and no
        section title anywhere in that half of the step -- unlike linux's
        `echo "== S-B3: ... =="` + `cat readelf_dynamic.txt`
        (test_import_closure_success above). A future "helpful" symmetry
        fix that adds a banner/echo to the android branch must fail here."""
        artifact_dir, ndk_home = self._android_dirs()

        def fake_run(argv, cwd=None, env=None):
            if str(argv[0]).endswith("llvm-readelf"):
                return _fake_run_result(
                    returncode=0,
                    stdout=(
                        "Dynamic section at offset 0x1000 contains 2 entries:\n"
                        " 0x0000000000000001 (NEEDED) Shared library: [libc.so]\n"
                    ),
                )
            return _fake_run_result(
                returncode=0,
                stdout=(
                    "IMPORT libc.so -> OS_ALLOWLIST\n"
                    "IMPORT_CLOSURE_RESULT=PASS\n"
                    "IMPORT_CLOSURE PASS (android): 1 imports, 0 staged companions\n"
                ),
            )

        with mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, err = _run_captured(
                verify_artifact.import_closure,
                "android",
                artifact_dir=artifact_dir,
                ndk_home=ndk_home,
            )
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertNotIn("==", out)
        self.assertNotIn("Dynamic section", out)  # the raw dump is never echoed
        self.assertIn("IMPORT_CLOSURE_RC=0", out)

    def test_import_closure_android_failure_message_names_the_script(self):
        """PORTED AS-IS pin, the other direction from linux's message
        (test_import_closure_failure_returns_nonzero_and_errors): android's
        source YAML literal is
        '::error::import-closure gate (WI-4/assert_import_closure.py)
        failed for ...' -- a DIFFERENT string from linux's, kept as two
        different literal strings here on purpose (P-10 discipline), not
        unified into one platform-generic message."""
        artifact_dir, ndk_home = self._android_dirs()

        def fake_run(argv, cwd=None, env=None):
            if str(argv[0]).endswith("llvm-readelf"):
                return _fake_run_result(returncode=0, stdout="Dynamic section ...\n")
            return _fake_run_result(
                returncode=1,
                stdout="IMPORT libmystery.so -> MISSING\nIMPORT_CLOSURE_RESULT=FAIL\n",
            )

        with mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, err = _run_captured(
                verify_artifact.import_closure,
                "android",
                artifact_dir=artifact_dir,
                ndk_home=ndk_home,
            )
        self.assertEqual(rc, 1)
        self.assertIn("IMPORT_CLOSURE_RC=1", out)
        self.assertIn("import-closure gate (WI-4/assert_import_closure.py) failed for", err)
        # Linux's own (shorter) message must not ALSO appear -- proves this
        # is the distinct android string, not the linux one with extra text
        # appended elsewhere.
        self.assertEqual(err.count("import-closure gate"), 1)

    # ---- S-B3 windows (P-23, parking-lot round) -------------------------
    #
    # These fake the two PE dumpers and the child gate the same way the
    # linux/android tests fake readelf. The gate's behaviour against REAL PE
    # binaries (the shipped v0.1.23 Windows set) was demonstrated separately,
    # red-before-green, with the depth-one and transitive failures isolated
    # from each other -- see tmp/verify/wi56/PREREGISTERED.md and the s1/s2/s3
    # observation files. These tests are the regression net for that.

    def _windows_dirs(self, companions=("heif.dll", "libde265.dll")):
        """Creates `native/build-windows/` (targets.spec('windows')'s
        artifact_path/dist_dir, both relative -- so this works inside _Cwd)."""
        build = Path("native/build-windows")
        build.mkdir(parents=True, exist_ok=True)
        (build / "dng_decoder_native.dll").write_bytes(b"")
        for name in companions:
            (build / name).write_bytes(b"")
        return build

    @staticmethod
    def _windows_fake_run(*, dumps, gate_results):
        """dumps: binary-basename -> dump text. gate_results: dump-basename ->
        (rc, stdout). Any dumpbin call fails (rc=1) so the llvm-objdump
        fallback leg is what produces the dump -- the same leg the real
        runner uses only when dumpbin is unavailable, exercised here because
        it is the harder path (two markers instead of one)."""
        def fake_run(argv, cwd=None, env=None):
            if argv[0] == "dumpbin":
                return _fake_run_result(returncode=1, stderr="dumpbin: not found\n")
            if argv[0] == "llvm-objdump":
                name = Path(argv[-1]).name
                return _fake_run_result(returncode=0, stdout=dumps.get(name, ""))
            # the child gate: `sys.executable native/scripts/assert_import_closure.py --dump D ...`
            dump_arg = Path(argv[argv.index("--dump") + 1]).name
            rc, out = gate_results.get(dump_arg, (0, "IMPORT_CLOSURE_RESULT=PASS\n"))
            return _fake_run_result(returncode=rc, stdout=out)
        return fake_run

    def test_import_closure_windows_success_walks_transitively(self):
        dumps = {
            "dng_decoder_native.dll": "Dump of file x\n    DLL Name: heif.dll\n",
            "heif.dll": "Dump of file y\n    DLL Name: libde265.dll\n",
            "libde265.dll": "Dump of file z\n    DLL Name: KERNEL32.dll\n",
        }
        with _Cwd(self._tmp()):
            self._windows_dirs()
            with mock.patch.object(
                run_module, "run",
                side_effect=self._windows_fake_run(dumps=dumps, gate_results={}),
            ):
                rc, out, err = _run_captured(verify_artifact.import_closure, "windows")
        self.assertEqual(rc, 0)
        self.assertIn("DEPENDENTS_RC=1", out)
        self.assertIn("LLVM_OBJDUMP_RC=0", out)
        self.assertIn("ASSERT dep heif.dll RC=0", out)
        self.assertIn("IMPORT_CLOSURE_RC=0", out)
        self.assertIn("HEIF_DEPENDENTS_RC=1", out)
        self.assertIn("ASSERT dep libde265.dll RC=0", out)
        self.assertEqual(err, "")
        # The walk actually reached both companions, not just depth one.
        self.assertIn("transitive(heif.dll):", out)
        self.assertIn("transitive(libde265.dll):", out)

    def test_import_closure_windows_transitive_failure_fails_the_step(self):
        """THE test that keeps the transitive walk from being decorative: the
        DECODER's own closure passes and only a staged companion's does not.
        A depth-one gate is green on exactly this input (run 33294722901's
        failure class)."""
        dumps = {
            "dng_decoder_native.dll": "Dump of file x\n    DLL Name: heif.dll\n",
            "heif.dll": "Dump of file y\n    DLL Name: libde265.dll\n",
        }
        gate = {
            "dll_dependents_body.txt": (0, "IMPORT heif.dll -> STAGED\nIMPORT_CLOSURE_RESULT=PASS\n"),
            "transitive_heif.dll.body.txt": (
                1, "IMPORT libde265.dll -> MISSING\nIMPORT_CLOSURE_RESULT=FAIL\n"
            ),
        }
        with _Cwd(self._tmp()):
            self._windows_dirs(companions=("heif.dll",))
            with mock.patch.object(
                run_module, "run",
                side_effect=self._windows_fake_run(dumps=dumps, gate_results=gate),
            ):
                rc, out, err = _run_captured(verify_artifact.import_closure, "windows")
        self.assertEqual(rc, 1)
        self.assertIn("IMPORT_CLOSURE_RC=1", out)
        self.assertIn("import-closure gate failed for staged companion heif.dll", err)

    def test_import_closure_windows_emits_exactly_one_import_closure_rc(self):
        """AC-2 shape: the transitive walk must not multiply the step's
        markers. One `IMPORT_CLOSURE_RC=` for the whole graph, and no
        `transitive(...)` line may register as a marker at all."""
        from .. import markerdiff

        dumps = {
            "dng_decoder_native.dll": "Dump of file x\n    DLL Name: heif.dll\n",
            "heif.dll": "Dump of file y\n    DLL Name: libde265.dll\n",
            "libde265.dll": "Dump of file z\n    DLL Name: KERNEL32.dll\n",
        }
        with _Cwd(self._tmp()):
            self._windows_dirs()
            with mock.patch.object(
                run_module, "run",
                side_effect=self._windows_fake_run(dumps=dumps, gate_results={}),
            ):
                _rc, out, _err = _run_captured(verify_artifact.import_closure, "windows")
        markers = markerdiff.extract(out)
        self.assertEqual(
            len([m for m in markers if m.startswith("IMPORT_CLOSURE_RC=")]), 1
        )
        self.assertEqual([m for m in markers if "transitive(" in m], [])

    def test_import_closure_windows_missing_heif_import_names_the_fix(self):
        dumps = {"dng_decoder_native.dll": "Dump of file x\n    DLL Name: KERNEL32.dll\n"}
        with _Cwd(self._tmp()):
            self._windows_dirs()
            with mock.patch.object(
                run_module, "run",
                side_effect=self._windows_fake_run(dumps=dumps, gate_results={}),
            ):
                rc, out, err = _run_captured(verify_artifact.import_closure, "windows")
        self.assertEqual(rc, 1)
        self.assertIn("ASSERT dep heif.dll RC=1", out)
        self.assertIn("does not import heif.dll", err)
        self.assertIn("Do NOT 'fix' this by disabling HEIF", err)

    def test_import_closure_windows_unreadable_dump_is_unverified_not_pass(self):
        """Both dumpers failing must REFUSE, never fall through to a green --
        `dependency presence is UNVERIFIED` is the whole point of the step."""
        def fake_run(argv, cwd=None, env=None):
            return _fake_run_result(returncode=1, stderr="no such tool\n")

        with _Cwd(self._tmp()):
            self._windows_dirs()
            with mock.patch.object(run_module, "run", side_effect=fake_run):
                rc, out, err = _run_captured(verify_artifact.import_closure, "windows")
        self.assertEqual(rc, 1)
        self.assertIn("DEPENDENTS_RC=1", out)
        self.assertIn("LLVM_OBJDUMP_RC=1", out)
        self.assertIn("dependency presence is UNVERIFIED", err)
        self.assertNotIn("IMPORT_CLOSURE_RC=0", out)

    def test_import_closure_windows_strips_self_matching_dump_header(self):
        """The header names the dumped file's PATH. If that path contains
        'heif.dll', the un-stripped dump satisfies the heif assertion without
        the binary importing anything (the 2026-08-28 otool self-match)."""
        dumps = {
            "dng_decoder_native.dll":
                "Dump of file C:/work/heif.dll-staging/dng_decoder_native.dll\n"
                "    DLL Name: KERNEL32.dll\n",
        }
        with _Cwd(self._tmp()):
            self._windows_dirs()
            with mock.patch.object(
                run_module, "run",
                side_effect=self._windows_fake_run(dumps=dumps, gate_results={}),
            ):
                rc, out, err = _run_captured(verify_artifact.import_closure, "windows")
        self.assertEqual(rc, 1)
        self.assertIn("ASSERT dep heif.dll RC=1", out)
        self.assertIn("does not import heif.dll", err)

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
