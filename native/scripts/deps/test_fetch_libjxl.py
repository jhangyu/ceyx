"""Tests for fetch_libjxl.py -- all runnable on macOS/Linux without network
or a real libjxl checkout, mirroring win_jxl_dist_test.py's
TestTranscriptionMatchesTheShellScript pattern.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from deps import fetch_libjxl, win_jxl_dist  # noqa: E402


def _dist_with(files: dict, root: Path) -> Path:
    for relative, content in files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
    return root


class TestSharesPinsWithWindowsCarrier(unittest.TestCase):
    """The whole point of importing rather than re-declaring: these must be
    object-identical, not merely equal."""

    def test_tag_is_the_same_object(self) -> None:
        self.assertIs(fetch_libjxl.JXL_TAG, win_jxl_dist.JXL_TAG)

    def test_repo_url_is_the_same_object(self) -> None:
        self.assertIs(fetch_libjxl.JXL_REPO_URL, win_jxl_dist.JXL_REPO_URL)

    def test_submodules_is_the_same_object(self) -> None:
        self.assertIs(fetch_libjxl.JXL_NEEDED_SUBMODULES, win_jxl_dist.JXL_NEEDED_SUBMODULES)


class TestFrozenPinsMatchTheDeletedShellScript(unittest.TestCase):
    """fetch_libjxl_dist.sh was DELETED in the same commit set as this
    module (2026-09-01 contract item 11 / ENTRY-POINT RULE): it had zero
    remaining consumers once every workflow call site was rewired to
    ``build_deps.py fetch libjxl``, and windows_build.yml never called it in
    the first place (it has no Windows branch -- see this module's
    docstring). Unlike fetch_halide.py/fetch_libraw.py (whose shell
    originals are still executed by windows_build.yml this round and so
    still exist to diff against), there is no live shell script left here
    to mechanically diff against.

    These values are therefore FROZEN LITERALS, transcribed once at
    migration time from the last revision of fetch_libjxl_dist.sh (git
    history has the original text if a future audit needs to re-diff). A
    value changing here without a corresponding intentional edit is exactly
    as much a red flag as a live-diff mismatch would have been.
    """

    def test_required_libs(self) -> None:
        self.assertEqual(
            fetch_libjxl.REQUIRED_LIBS,
            (
                "libjxl.a",
                "libjxl_cms.a",
                "libjxl_threads.a",
                "libhwy.a",
                "libbrotlicommon.a",
                "libbrotlidec.a",
                "libbrotlienc.a",
            ),
        )

    def test_required_symbols(self) -> None:
        self.assertEqual(
            fetch_libjxl.REQUIRED_SYMBOLS,
            ("JxlEncoderProcessOutput", "JxlDecoderProcessInput", "JxlEncoderAddBox"),
        )

    def test_cmake_flags(self) -> None:
        self.assertEqual(
            fetch_libjxl._CMAKE_ARGS_BASE,
            (
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
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
        self.assertIn("-DJPEGXL_ENABLE_SKCMS=ON", fetch_libjxl._CMAKE_ARGS_BASE)

    def test_license_dirs(self) -> None:
        self.assertEqual(fetch_libjxl._LICENSE_DIRS, ("libjxl", "highway", "brotli", "skcms"))


class TestResolveArch(unittest.TestCase):
    def test_env_override_wins(self) -> None:
        with mock.patch.dict("os.environ", {"CEYX_JXL_ARCH": "arm64"}):
            self.assertEqual(fetch_libjxl.resolve_arch("x86_64"), "arm64")

    def test_falls_back_to_host_machine(self) -> None:
        with mock.patch.dict("os.environ", {}, clear=True):
            self.assertEqual(fetch_libjxl.resolve_arch("x86_64"), "x86_64")


class TestResolveDist(unittest.TestCase):
    def test_same_arch_uses_plain_dist_dir(self) -> None:
        native_dir = Path("/native")
        self.assertEqual(
            fetch_libjxl.resolve_dist(native_dir, "arm64", "arm64"),
            native_dir / "third_party" / "libjxl-dist",
        )

    def test_cross_arch_uses_suffixed_dist_dir(self) -> None:
        native_dir = Path("/native")
        self.assertEqual(
            fetch_libjxl.resolve_dist(native_dir, "x86_64", "arm64"),
            native_dir / "third_party" / "libjxl-dist-x86_64",
        )


class TestWantPins(unittest.TestCase):
    def test_format(self) -> None:
        pins = fetch_libjxl.compute_want_pins("deadbeef", "-1234 third_party/highway", arch="arm64")
        expected = f"tag={win_jxl_dist.JXL_TAG} commit=deadbeef arch=arm64\n-1234 third_party/highway"
        self.assertEqual(pins, expected)


class TestStampFastPath(unittest.TestCase):
    WANT = f"tag={win_jxl_dist.JXL_TAG} commit=deadbeef arch=arm64\nstatus"

    def test_current_when_pins_match_and_all_libs_exist(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {".pins": self.WANT}
            for lib in fetch_libjxl.REQUIRED_LIBS:
                files[f"lib/{lib}"] = "x"
            dist = _dist_with(files, Path(tmp))
            self.assertTrue(fetch_libjxl.stamp_is_current(dist, self.WANT))

    def test_not_current_when_a_lib_was_deleted(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {".pins": self.WANT}
            for lib in fetch_libjxl.REQUIRED_LIBS[:-1]:
                files[f"lib/{lib}"] = "x"
            dist = _dist_with(files, Path(tmp))
            self.assertFalse(fetch_libjxl.stamp_is_current(dist, self.WANT))

    def test_not_current_when_no_stamp(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {}
            for lib in fetch_libjxl.REQUIRED_LIBS:
                files[f"lib/{lib}"] = "x"
            dist = _dist_with(files, Path(tmp))
            self.assertFalse(fetch_libjxl.stamp_is_current(dist, self.WANT))


class TestAssertStaticLibs(unittest.TestCase):
    def test_green_when_all_present(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {}
            for lib in fetch_libjxl.REQUIRED_LIBS:
                files[f"lib/{lib}"] = "x"
            dist = _dist_with(files, Path(tmp))
            fetch_libjxl.assert_static_libs(dist)  # must not raise

    def test_red_names_missing_lib(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {}
            for lib in fetch_libjxl.REQUIRED_LIBS[:-1]:
                files[f"lib/{lib}"] = "x"
            dist = _dist_with(files, Path(tmp))
            with self.assertRaises(fetch_libjxl.JxlFetchError) as ctx:
                fetch_libjxl.assert_static_libs(dist)
            self.assertIn(fetch_libjxl.REQUIRED_LIBS[-1], str(ctx.exception))


class TestAssertSymbols(unittest.TestCase):
    def test_green_when_all_symbols_present(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = Path(tmp)
            (dist / "lib").mkdir(parents=True)
            (dist / "lib" / "libjxl.a").write_bytes(b"!<arch>\n")
            nm_text = "\n".join(f"0000000 T {sym}" for sym in fetch_libjxl.REQUIRED_SYMBOLS)
            with mock.patch.object(fetch_libjxl, "_read_nm_symbols", return_value=nm_text):
                fetch_libjxl.assert_symbols(dist)  # must not raise

    def test_red_when_a_symbol_missing(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = Path(tmp)
            (dist / "lib").mkdir(parents=True)
            (dist / "lib" / "libjxl.a").write_bytes(b"!<arch>\n")
            nm_text = "0000000 T JxlEncoderProcessOutput\n0000000 T JxlDecoderProcessInput\n"
            with mock.patch.object(fetch_libjxl, "_read_nm_symbols", return_value=nm_text):
                with self.assertRaises(fetch_libjxl.JxlFetchError) as ctx:
                    fetch_libjxl.assert_symbols(dist)
            self.assertIn("JxlEncoderAddBox", str(ctx.exception))


class TestVendorLicenses(unittest.TestCase):
    def test_copies_all_four(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "src"
            dist = root / "dist"
            for sub in ("third_party/highway", "third_party/brotli", "third_party/skcms"):
                (src / sub).mkdir(parents=True)
                (src / sub / "LICENSE").write_text("x", encoding="utf-8")
            (src / "LICENSE").write_text("x", encoding="utf-8")

            fetch_libjxl.vendor_licenses(src, dist)

            for name in fetch_libjxl._LICENSE_DIRS:
                self.assertTrue(any((dist / "share" / "licenses" / name).iterdir()))

    def test_red_when_missing(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "src"
            dist = root / "dist"
            (src / "third_party" / "highway").mkdir(parents=True)
            (src / "LICENSE").write_text("x", encoding="utf-8")
            with self.assertRaises(fetch_libjxl.JxlFetchError):
                fetch_libjxl.vendor_licenses(src, dist)


class TestBuildOrdering(unittest.TestCase):
    def test_skips_build_when_stamp_current(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = Path(tmp)
            with mock.patch.object(fetch_libjxl, "clone_source", return_value=Path("/src")), \
                 mock.patch.object(fetch_libjxl, "want_pins", return_value="pins"), \
                 mock.patch.object(fetch_libjxl, "stamp_is_current", return_value=True), \
                 mock.patch.object(fetch_libjxl, "configure_build_install") as m_configure, \
                 mock.patch.object(fetch_libjxl, "assert_static_libs") as m_libs, \
                 mock.patch.object(fetch_libjxl, "assert_symbols") as m_syms, \
                 mock.patch.object(fetch_libjxl, "strip_archives") as m_strip, \
                 mock.patch.object(fetch_libjxl, "vendor_licenses") as m_vendor:
                result = fetch_libjxl.build(dist, arch="arm64")
            self.assertEqual(result, dist)
            m_configure.assert_not_called()
            m_libs.assert_not_called()
            m_syms.assert_not_called()
            m_strip.assert_not_called()
            m_vendor.assert_not_called()

    def test_full_build_runs_every_stage_in_order(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = Path(tmp)
            calls = []
            with mock.patch.object(fetch_libjxl, "clone_source", return_value=Path("/src")), \
                 mock.patch.object(fetch_libjxl, "want_pins", return_value="pins"), \
                 mock.patch.object(fetch_libjxl, "stamp_is_current", return_value=False), \
                 mock.patch.object(fetch_libjxl, "configure_build_install",
                                    side_effect=lambda *a, **k: calls.append("configure")), \
                 mock.patch.object(fetch_libjxl, "assert_static_libs",
                                    side_effect=lambda *a, **k: calls.append("assert_libs")), \
                 mock.patch.object(fetch_libjxl, "assert_symbols",
                                    side_effect=lambda *a, **k: calls.append("assert_symbols")), \
                 mock.patch.object(fetch_libjxl, "strip_archives",
                                    side_effect=lambda *a, **k: calls.append("strip")), \
                 mock.patch.object(fetch_libjxl, "vendor_licenses",
                                    side_effect=lambda *a, **k: calls.append("vendor")):
                fetch_libjxl.build(dist, arch="arm64")
            self.assertEqual(calls, ["configure", "assert_libs", "assert_symbols", "strip", "vendor"])
            self.assertEqual((dist / ".pins").read_text(encoding="utf-8"), "pins")


if __name__ == "__main__":
    unittest.main()
