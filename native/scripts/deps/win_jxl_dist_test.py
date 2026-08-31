"""Tests for win_jxl_dist.py -- all runnable on macOS/Linux (spec §8.1).

Nothing here invokes git, cmake, clang-cl or llvm-nm: the parts that CAN
only be proven on a Windows runner (that the dist actually links, that
jxl.lib really contains the encode/decode entry points) are CI's job and
are asserted there by the module itself. What is proven here is the logic
that would otherwise only be debugged one 90-minute CI round at a time:
pin-string derivation, the stamp fast path, the static-lib layout
assertion, the frozen transcription fidelity against the now-deleted
``build_libjxl_dist_windows.sh`` (see
``TestFrozenTranscriptionMatchesTheDeletedShellScript``), and the cmake
argv shape.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

# Imported through the PACKAGE (`deps.`), never flat -- same rationale as
# win_heif_dist_test.py: a flat import sends relative-import dependencies
# down a fallback branch, and exception identity must stay single.
from deps import win_jxl_dist, win_pe  # noqa: E402


def _dist_with(files: dict, root: Path) -> Path:
    for relative, content in files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
    return root


class TestFrozenTranscriptionMatchesTheDeletedShellScript(unittest.TestCase):
    """build_libjxl_dist_windows.sh was DELETED in the same commit set as
    this freeze (round 2 task #6 closeout, 2026-09-01 contract item 10 /
    ENTRY-POINT RULE): the carrier-built dist was proven green on a real
    Windows CI leg (run https://github.com/jhangyu/ceyx/actions/runs/33421023420,
    head c1d771d) and the .sh had zero remaining consumers once
    jxl_dist_windows.yml was rewired to call ``build_deps.py build
    jxl-stack``. Same pattern as ``test_fetch_libjxl.py``'s
    ``TestFrozenPinsMatchTheDeletedShellScript`` for the macOS/Linux
    sibling.

    These values are FROZEN LITERALS, copied (not retyped) from the last
    revision of ``build_libjxl_dist_windows.sh`` at commit
    d125df8e830b9c7e98541b34fff42ab694abde2b (``git show
    d125df8e830b9c7e98541b34fff42ab694abde2b:native/scripts/build_libjxl_dist_windows.sh``
    recovers the full original text). A value changing here without a
    corresponding intentional edit to ``win_jxl_dist.py`` is exactly as much
    a red flag as a live-diff mismatch against the .sh would have been.
    """

    def test_jxl_tag_matches(self) -> None:
        self.assertEqual(win_jxl_dist.JXL_TAG, "v0.12.0")

    def test_repo_url_matches(self) -> None:
        self.assertEqual(win_jxl_dist.JXL_REPO_URL, "https://github.com/libjxl/libjxl.git")

    def test_required_libs_match(self) -> None:
        self.assertEqual(
            win_jxl_dist.REQUIRED_LIBS,
            (
                "jxl.lib",
                "jxl_threads.lib",
                "jxl_cms.lib",
                "hwy.lib",
                "brotlicommon.lib",
                "brotlidec.lib",
                "brotlienc.lib",
            ),
        )

    def test_required_symbols_match(self) -> None:
        self.assertEqual(
            win_jxl_dist.REQUIRED_SYMBOLS,
            ("JxlEncoderProcessOutput", "JxlDecoderProcessInput", "JxlEncoderAddBox"),
        )

    def test_needed_submodules_match(self) -> None:
        self.assertEqual(
            win_jxl_dist.JXL_NEEDED_SUBMODULES,
            ("third_party/brotli", "third_party/highway", "third_party/skcms"),
        )

    def test_cmake_flags_match(self) -> None:
        self.assertEqual(
            win_jxl_dist._CMAKE_ARGS_BASE,
            (
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCMAKE_C_COMPILER=clang-cl",
                "-DCMAKE_CXX_COMPILER=clang-cl",
                "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
                "-DBUILD_SHARED_LIBS=OFF",
                "-DBUILD_TESTING=OFF",
                "-DJPEGXL_ENABLE_TOOLS=OFF",
                "-DJPEGXL_ENABLE_BENCHMARK=OFF",
                "-DJPEGXL_ENABLE_EXAMPLES=OFF",
                "-DJPEGXL_ENABLE_FUZZERS=OFF",
                "-DJPEGXL_ENABLE_DOXYGEN=OFF",
                "-DJPEGXL_ENABLE_MANPAGES=OFF",
                "-DJPEGXL_ENABLE_SJPEG=OFF",
                "-DJPEGXL_ENABLE_OPENEXR=OFF",
                "-DJPEGXL_ENABLE_SKCMS=ON",
                "-DJPEGXL_ENABLE_JNI=OFF",
                "-DJPEGXL_FORCE_SYSTEM_BROTLI=OFF",
                "-DJPEGXL_FORCE_SYSTEM_HWY=OFF",
            ),
        )

    def test_skcms_is_on_not_off(self) -> None:
        """R7 / the module's own KNOWN RISK paragraph: this flag must never
        be flipped OFF to make a build pass."""
        self.assertIn("-DJPEGXL_ENABLE_SKCMS=ON", win_jxl_dist._CMAKE_ARGS_BASE)


class TestCmakeConfigureArgs(unittest.TestCase):
    def test_install_prefix_is_the_dist_dir(self) -> None:
        dist = Path("/tmp/jxl-dist")
        argv = win_jxl_dist.cmake_configure_args(dist)
        # str(dist), not a POSIX literal: the CI Windows leg runs this suite
        # on native Windows Python, where Path("/tmp/...") renders as \tmp\...
        self.assertIn(f"-DCMAKE_INSTALL_PREFIX={dist}", argv)

    def test_no_shared_libs(self) -> None:
        argv = win_jxl_dist.cmake_configure_args(Path("/tmp/jxl-dist"))
        self.assertIn("-DBUILD_SHARED_LIBS=OFF", argv)

    def test_static_msvc_runtime(self) -> None:
        argv = win_jxl_dist.cmake_configure_args(Path("/tmp/jxl-dist"))
        self.assertIn("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", argv)


class TestWantPins(unittest.TestCase):
    def test_format_matches_the_shell_script_byte_for_byte(self) -> None:
        """Compatibility with a .pins written by
        build_libjxl_dist_windows.sh is what makes the shell->Python
        switchover a no-op for an already-built tree instead of a forced
        rebuild."""
        pins = win_jxl_dist.compute_want_pins("deadbeef", "-1234 third_party/highway", arch="x86_64")
        expected = "tag=v0.12.0 commit=deadbeef platform=windows-x86_64\n-1234 third_party/highway"
        self.assertEqual(pins, expected)

    def test_want_pins_reads_git_state_via_run(self) -> None:
        with mock.patch.object(win_jxl_dist, "git_rev_parse_head", return_value="cafef00d"), \
             mock.patch.object(win_jxl_dist, "git_submodule_status", return_value="status-line"):
            pins = win_jxl_dist.want_pins(Path("/nonexistent"), arch="x86_64")
        self.assertEqual(pins, "tag=v0.12.0 commit=cafef00d platform=windows-x86_64\nstatus-line")


class TestStampFastPath(unittest.TestCase):
    WANT = "tag=v0.12.0 commit=deadbeef platform=windows-x86_64\nstatus"

    def test_current_when_pins_match_and_all_libs_exist(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {".pins": self.WANT}
            for lib in win_jxl_dist.REQUIRED_LIBS:
                files[f"lib/{lib}"] = "x"
            dist = _dist_with(files, Path(tmp))
            self.assertTrue(win_jxl_dist.stamp_is_current(dist, self.WANT))

    def test_not_current_when_a_lib_was_deleted(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {".pins": self.WANT}
            for lib in win_jxl_dist.REQUIRED_LIBS[:-1]:
                files[f"lib/{lib}"] = "x"
            dist = _dist_with(files, Path(tmp))
            self.assertFalse(win_jxl_dist.stamp_is_current(dist, self.WANT))

    def test_not_current_when_pins_differ(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {".pins": "tag=v0.0.0 commit=old platform=windows-x86_64\n"}
            for lib in win_jxl_dist.REQUIRED_LIBS:
                files[f"lib/{lib}"] = "x"
            dist = _dist_with(files, Path(tmp))
            self.assertFalse(win_jxl_dist.stamp_is_current(dist, self.WANT))

    def test_not_current_when_no_stamp_at_all(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {}
            for lib in win_jxl_dist.REQUIRED_LIBS:
                files[f"lib/{lib}"] = "x"
            dist = _dist_with(files, Path(tmp))
            self.assertFalse(win_jxl_dist.stamp_is_current(dist, self.WANT))


class TestAssertStaticLibs(unittest.TestCase):
    def test_green_when_all_libs_and_header_present(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {"include/jxl/encode.h": "x"}
            for lib in win_jxl_dist.REQUIRED_LIBS:
                files[f"lib/{lib}"] = "x"
            dist = _dist_with(files, Path(tmp))
            win_jxl_dist.assert_static_libs(dist)  # must not raise

    def test_red_names_the_missing_lib(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {"include/jxl/encode.h": "x"}
            for lib in win_jxl_dist.REQUIRED_LIBS[:-1]:
                files[f"lib/{lib}"] = "x"
            dist = _dist_with(files, Path(tmp))
            with self.assertRaises(win_jxl_dist.WindowsJxlError) as ctx:
                win_jxl_dist.assert_static_libs(dist)
            self.assertIn(win_jxl_dist.REQUIRED_LIBS[-1], str(ctx.exception))

    def test_red_when_header_missing(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {}
            for lib in win_jxl_dist.REQUIRED_LIBS:
                files[f"lib/{lib}"] = "x"
            dist = _dist_with(files, Path(tmp))
            with self.assertRaises(win_jxl_dist.WindowsJxlError) as ctx:
                win_jxl_dist.assert_static_libs(dist)
            self.assertIn("encode.h", str(ctx.exception))


class TestAssertSymbols(unittest.TestCase):
    def test_green_when_all_symbols_present(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = Path(tmp)
            (dist / "lib").mkdir(parents=True)
            (dist / "lib" / "jxl.lib").write_bytes(b"!<arch>\n")
            nm_text = "\n".join(f"0000000 T {sym}" for sym in win_jxl_dist.REQUIRED_SYMBOLS)
            with mock.patch.object(win_jxl_dist, "_read_nm_symbols", return_value=nm_text):
                win_jxl_dist.assert_symbols(dist)  # must not raise

    def test_red_when_a_symbol_is_missing(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = Path(tmp)
            (dist / "lib").mkdir(parents=True)
            (dist / "lib" / "jxl.lib").write_bytes(b"!<arch>\n")
            nm_text = "0000000 T JxlEncoderProcessOutput\n0000000 T JxlDecoderProcessInput\n"
            with mock.patch.object(win_jxl_dist, "_read_nm_symbols", return_value=nm_text):
                with self.assertRaises(win_pe.PeAssertionFailed) as ctx:
                    win_jxl_dist.assert_symbols(dist)
            self.assertIn("JxlEncoderAddBox", str(ctx.exception))
            self.assertIn("skcms", str(ctx.exception).lower())


class TestVendorLicenses(unittest.TestCase):
    def test_copies_found_licence_files(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "src"
            dist = root / "dist"
            (src / "third_party" / "highway").mkdir(parents=True)
            (src / "third_party" / "brotli").mkdir(parents=True)
            (src / "LICENSE").write_text("jxl licence", encoding="utf-8")
            (src / "third_party" / "highway" / "LICENSE").write_text("hwy licence", encoding="utf-8")
            (src / "third_party" / "brotli" / "COPYING").write_text("brotli licence", encoding="utf-8")

            win_jxl_dist.vendor_licenses(src, dist)

            self.assertTrue((dist / "share" / "licenses" / "libjxl" / "LICENSE").is_file())
            self.assertTrue((dist / "share" / "licenses" / "highway" / "LICENSE").is_file())
            self.assertTrue((dist / "share" / "licenses" / "brotli" / "COPYING").is_file())

    def test_red_when_a_licence_is_missing(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "src"
            dist = root / "dist"
            (src / "third_party" / "highway").mkdir(parents=True)
            (src / "third_party" / "brotli").mkdir(parents=True)
            (src / "LICENSE").write_text("jxl licence", encoding="utf-8")
            (src / "third_party" / "brotli" / "COPYING").write_text("brotli licence", encoding="utf-8")
            # highway licence deliberately absent

            with self.assertRaises(win_jxl_dist.WindowsJxlError) as ctx:
                win_jxl_dist.vendor_licenses(src, dist)
            self.assertIn("highway", str(ctx.exception))


class TestBuildOrdering(unittest.TestCase):
    """build() must clone, compute pins, skip on a current stamp, and
    otherwise configure/install/assert/vendor/stamp in that order."""

    def test_skips_build_when_stamp_current(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = Path(tmp)
            with mock.patch.object(win_jxl_dist, "clone_source", return_value=Path("/src")) as m_clone, \
                 mock.patch.object(win_jxl_dist, "want_pins", return_value="pins") as m_want, \
                 mock.patch.object(win_jxl_dist, "stamp_is_current", return_value=True), \
                 mock.patch.object(win_jxl_dist, "configure_build_install") as m_configure, \
                 mock.patch.object(win_jxl_dist, "assert_static_libs") as m_assert_libs, \
                 mock.patch.object(win_jxl_dist, "assert_symbols") as m_assert_symbols, \
                 mock.patch.object(win_jxl_dist, "vendor_licenses") as m_vendor:
                result = win_jxl_dist.build(dist)
            self.assertEqual(result, dist)
            m_clone.assert_called_once()
            m_want.assert_called_once()
            m_configure.assert_not_called()
            m_assert_libs.assert_not_called()
            m_assert_symbols.assert_not_called()
            m_vendor.assert_not_called()

    def test_full_build_runs_every_stage_in_order(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = Path(tmp)
            calls = []
            with mock.patch.object(win_jxl_dist, "clone_source", return_value=Path("/src")), \
                 mock.patch.object(win_jxl_dist, "want_pins", return_value="pins"), \
                 mock.patch.object(win_jxl_dist, "stamp_is_current", return_value=False), \
                 mock.patch.object(
                     win_jxl_dist, "configure_build_install",
                     side_effect=lambda *a, **k: calls.append("configure_build_install"),
                 ), \
                 mock.patch.object(
                     win_jxl_dist, "assert_static_libs", side_effect=lambda *a, **k: calls.append("assert_static_libs")
                 ), \
                 mock.patch.object(
                     win_jxl_dist, "assert_symbols", side_effect=lambda *a, **k: calls.append("assert_symbols")
                 ), \
                 mock.patch.object(
                     win_jxl_dist, "vendor_licenses", side_effect=lambda *a, **k: calls.append("vendor_licenses")
                 ):
                win_jxl_dist.build(dist)
            self.assertEqual(
                calls,
                ["configure_build_install", "assert_static_libs", "assert_symbols", "vendor_licenses"],
            )
            self.assertEqual((dist / ".pins").read_text(encoding="utf-8"), "pins")


if __name__ == "__main__":
    unittest.main()
