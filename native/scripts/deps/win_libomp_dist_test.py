"""Tests for win_libomp_dist.py -- all runnable on macOS/Linux (no Windows
toolchain): pins, argv, stamp fast path, layout/symbol/import-lib/CRT
assertions with the PE instruments mocked, and stage ordering."""
from __future__ import annotations

import io
import sys
import tarfile
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from deps import win_libomp_dist as w, win_pe  # noqa: E402


def _dist_with(files: dict, root: Path) -> Path:
    for relative, content in files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
    return root


class TestPins(unittest.TestCase):
    def test_version_tracks_the_runner_clang(self) -> None:
        # CI log: "The CXX compiler identification is Clang 22.1.8".
        self.assertEqual(w.LLVM_VERSION, "22.1.8")
        self.assertEqual(w.LLVM_TAG, "llvmorg-22.1.8")

    def test_sha256_is_the_release_digest(self) -> None:
        self.assertEqual(w.LLVM_SHA256, "922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888")

    def test_url(self) -> None:
        self.assertEqual(
            w.LLVM_URL,
            "https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/llvm-project-22.1.8.src.tar.xz",
        )

    def test_want_pins_names_dll_and_crt(self) -> None:
        self.assertEqual(
            w.compute_want_pins(arch="arm64"),
            f"libomp=llvmorg-22.1.8:{w.LLVM_SHA256} platform=windows-arm64 dll=libomp140.aarch64.dll crt=MT",
        )


class TestDllName(unittest.TestCase):
    def test_matches_what_the_decoder_imports(self) -> None:
        self.assertEqual(w.dll_name("x86_64"), "libomp140.x86_64.dll")
        self.assertEqual(w.dll_name("arm64"), "libomp140.aarch64.dll")

    def test_unknown_arch_fails_loudly(self) -> None:
        with self.assertRaises(w.WindowsLibompError):
            w.dll_name("arm64-v8a")


class TestCmakeConfigureArgs(unittest.TestCase):
    def test_x86_64(self) -> None:
        dist = Path("/tmp/omp-dist")
        argv = w.cmake_configure_args(dist, arch="x86_64")
        self.assertIn(f"-DCMAKE_INSTALL_PREFIX={dist}", argv)
        self.assertIn("-DOPENMP_MSVC_NAME_SCHEME=ON", argv)
        self.assertIn("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", argv)
        self.assertIn("-DLIBOMP_ARCH=x86_64", argv)
        # x64 assembles z_Windows_NT-586_asm.asm with the real ml64.
        self.assertFalse(any(a.startswith("-DCMAKE_ASM_MASM_COMPILER") for a in argv))

    def test_arm64(self) -> None:
        argv = w.cmake_configure_args(Path("/tmp/omp-dist"), arch="arm64")
        self.assertIn("-DLIBOMP_ARCH=aarch64", argv)
        self.assertIn("-DCMAKE_ASM_MASM_COMPILER=llvm-ml", argv)


class TestStampFastPath(unittest.TestCase):
    def test_current_when_pins_match_and_outputs_exist(self) -> None:
        want = w.compute_want_pins(arch="x86_64")
        with TemporaryDirectory() as tmp:
            dist = _dist_with({".pins": want, "bin/libomp140.x86_64.dll": "x", "lib/libomp.lib": "x"}, Path(tmp))
            self.assertTrue(w.stamp_is_current(dist, want, arch="x86_64"))

    def test_not_current_when_dll_missing(self) -> None:
        want = w.compute_want_pins(arch="x86_64")
        with TemporaryDirectory() as tmp:
            dist = _dist_with({".pins": want, "lib/libomp.lib": "x"}, Path(tmp))
            self.assertFalse(w.stamp_is_current(dist, want, arch="x86_64"))

    def test_not_current_for_other_arch_pins(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with(
                {".pins": w.compute_want_pins(arch="x86_64"), "bin/libomp140.x86_64.dll": "x", "lib/libomp.lib": "x"},
                Path(tmp),
            )
            self.assertFalse(w.stamp_is_current(dist, w.compute_want_pins(arch="arm64"), arch="arm64"))


class TestExtractFilter(unittest.TestCase):
    def test_only_openmp_and_cmake_are_extracted(self) -> None:
        root = w._SRC_ROOT
        names = {
            f"{root}/openmp/CMakeLists.txt": b"project(openmp)\n",
            f"{root}/openmp/LICENSE.TXT": b"apache\n",
            f"{root}/cmake/Modules/X.cmake": b"\n",
            f"{root}/llvm/CMakeLists.txt": b"\n",
            f"{root}/openmp-not/x": b"\n",
        }
        with TemporaryDirectory() as tmp:
            stage = Path(tmp)
            buf = io.BytesIO()
            with tarfile.open(fileobj=buf, mode="w:xz") as tf:
                for name, data in names.items():
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    tf.addfile(info, io.BytesIO(data))
            (stage / f"llvm-project-{w.LLVM_VERSION}.src.tar.xz").write_bytes(buf.getvalue())
            with mock.patch.object(w, "verify_sha256"):
                src = w.fetch_source(stage)
            self.assertEqual(src, stage / root / "openmp")
            self.assertTrue((src / "LICENSE.TXT").is_file())
            self.assertTrue((stage / root / "cmake" / "Modules" / "X.cmake").is_file())
            self.assertFalse((stage / root / "llvm").exists())
            self.assertFalse((stage / root / "openmp-not").exists())


class TestAssertLayout(unittest.TestCase):
    _FILES = ("bin/libomp140.aarch64.dll", "lib/libomp.lib", "include/omp.h")

    def test_green_when_all_present(self) -> None:
        with TemporaryDirectory() as tmp:
            w.assert_layout(_dist_with({f: "x" for f in self._FILES}, Path(tmp)), arch="arm64")

    def test_red_names_missing_file(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with({f: "x" for f in self._FILES[1:]}, Path(tmp))
            with self.assertRaises(w.WindowsLibompError) as ctx:
                w.assert_layout(dist, arch="arm64")
            self.assertIn("libomp140.aarch64.dll", str(ctx.exception))


class TestAssertImportLib(unittest.TestCase):
    def test_green_when_members_name_the_dll(self) -> None:
        with TemporaryDirectory() as tmp:
            with mock.patch.object(w, "_read_import_lib_members", return_value="libomp140.x86_64.dll\n" * 3):
                w.assert_import_lib_targets_dll(Path(tmp), arch="x86_64")

    def test_red_when_import_lib_names_another_dll(self) -> None:
        # The upstream default (no MSVC name scheme) would import libomp.dll.
        with TemporaryDirectory() as tmp:
            with mock.patch.object(w, "_read_import_lib_members", return_value="libomp.dll\n"):
                with self.assertRaises(win_pe.PeAssertionFailed):
                    w.assert_import_lib_targets_dll(Path(tmp), arch="x86_64")


class TestAssertExports(unittest.TestCase):
    def test_green(self) -> None:
        text = "\n".join(f"0001000 T {s}" for s in w.REQUIRED_EXPORTS)
        with TemporaryDirectory() as tmp:
            with mock.patch.object(w.win_pe, "read_exports", return_value=text):
                w.assert_exports(Path(tmp), arch="x86_64")

    def test_red_when_fork_call_missing(self) -> None:
        text = "0001000 T omp_get_num_threads\n0001000 T __kmpc_for_static_init_4\n"
        with TemporaryDirectory() as tmp:
            with mock.patch.object(w.win_pe, "read_exports", return_value=text):
                with self.assertRaises(win_pe.PeAssertionFailed) as ctx:
                    w.assert_exports(Path(tmp), arch="x86_64")
            self.assertIn("__kmpc_fork_call", str(ctx.exception))


class TestAssertStaticCrt(unittest.TestCase):
    def test_green_without_vcruntime(self) -> None:
        with TemporaryDirectory() as tmp:
            with mock.patch.object(w.win_pe, "read_dependents", return_value="KERNEL32.dll\nPSAPI.DLL\n"):
                w.assert_static_crt(Path(tmp), arch="x86_64")

    def test_red_with_vcruntime(self) -> None:
        with TemporaryDirectory() as tmp:
            with mock.patch.object(w.win_pe, "read_dependents", return_value="KERNEL32.dll\nVCRUNTIME140.dll\n"):
                with self.assertRaises(win_pe.PeAssertionFailed):
                    w.assert_static_crt(Path(tmp), arch="x86_64")


class TestPruneAliases(unittest.TestCase):
    def test_removes_only_the_alias_copies(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with(
                {"bin/libiomp5md.dll": "x", "lib/libiomp5md.lib": "x", "bin/libomp140.x86_64.dll": "x"}, Path(tmp)
            )
            w.prune_aliases(dist)
            self.assertEqual(sorted(str(p.relative_to(dist)) for p in dist.rglob("*") if p.is_file()),
                             [str(Path("bin/libomp140.x86_64.dll"))])


class TestBuildOrdering(unittest.TestCase):
    _STAGES = (
        "configure_build_install", "prune_aliases", "assert_layout", "assert_import_lib_targets_dll",
        "assert_exports", "assert_static_crt", "assert_architecture", "vendor_license",
    )

    def test_skips_build_when_stamp_current(self) -> None:
        with TemporaryDirectory() as tmp:
            with mock.patch.object(w, "stamp_is_current", return_value=True), \
                 mock.patch.object(w, "fetch_source") as m_fetch:
                self.assertEqual(w.build(Path(tmp), arch="arm64"), Path(tmp))
            m_fetch.assert_not_called()

    def test_full_build_runs_every_stage_in_order_then_stamps(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = Path(tmp)
            calls = []
            patches = [mock.patch.object(w, "stamp_is_current", return_value=False),
                       mock.patch.object(w, "fetch_source", return_value=Path("/src"))]
            for name in self._STAGES:
                patches.append(mock.patch.object(w, name, side_effect=lambda *a, _n=name, **k: calls.append(_n)))
            for p in patches:
                p.start()
            try:
                w.build(dist, arch="arm64")
            finally:
                for p in patches:
                    p.stop()
            self.assertEqual(calls, list(self._STAGES))
            self.assertEqual((dist / ".pins").read_text(encoding="utf-8"), w.compute_want_pins(arch="arm64"))


if __name__ == "__main__":
    unittest.main()
