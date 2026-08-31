"""Tests for win_webp_dist.py -- all runnable on macOS/Linux (spec §8.1).

Mirrors win_jxl_dist_test.py's TestTranscriptionMatchesTheShellScript
pattern. This round's proof for the Windows carrier is these tests plus
webp_dist_windows.yml's rewire; a real Windows build run is round 3's job
(this host cannot build a Windows PE) -- see the module docstring.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from deps import win_pe, win_webp_dist  # noqa: E402

_SH_SCRIPT = Path(__file__).resolve().parents[1] / "build_libwebp_dist_windows.sh"


def _dist_with(files: dict, root: Path) -> Path:
    for relative, content in files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
    return root


class TestTranscriptionMatchesTheShellScript(unittest.TestCase):
    def setUp(self) -> None:
        self.sh_text = _SH_SCRIPT.read_text(encoding="utf-8")

    def test_version_matches(self) -> None:
        self.assertIn(f'WEBP_VERSION="{win_webp_dist.WEBP_VERSION}"', self.sh_text)

    def test_sha256_matches(self) -> None:
        self.assertIn(win_webp_dist.WEBP_SHA256, self.sh_text)

    def test_url_pattern_matches(self) -> None:
        self.assertIn("storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-", self.sh_text)

    def test_required_libs_match(self) -> None:
        for lib in win_webp_dist.REQUIRED_LIBS:
            self.assertIn(f"lib/{lib}", self.sh_text)

    def test_required_headers_match(self) -> None:
        for header in win_webp_dist.REQUIRED_HEADERS:
            self.assertIn(header, self.sh_text)

    def test_encoder_symbols_match(self) -> None:
        for symbol in win_webp_dist.REQUIRED_ENCODER_SYMBOLS:
            self.assertIn(symbol, self.sh_text)

    def test_mux_symbols_match(self) -> None:
        for symbol in win_webp_dist.REQUIRED_MUX_SYMBOLS:
            self.assertIn(symbol, self.sh_text)

    def test_cmake_flags_match(self) -> None:
        for flag in win_webp_dist._CMAKE_ARGS_BASE:
            self.assertIn(flag, self.sh_text)

    def test_multithreaded_runtime_not_dynamic(self) -> None:
        self.assertIn("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", win_webp_dist._CMAKE_ARGS_BASE)


class TestCmakeConfigureArgs(unittest.TestCase):
    def test_install_prefix_is_the_dist_dir(self) -> None:
        argv = win_webp_dist.cmake_configure_args(Path("/tmp/webp-dist"))
        self.assertIn("-DCMAKE_INSTALL_PREFIX=/tmp/webp-dist", argv)

    def test_no_shared_libs(self) -> None:
        argv = win_webp_dist.cmake_configure_args(Path("/tmp/webp-dist"))
        self.assertIn("-DBUILD_SHARED_LIBS=OFF", argv)


class TestWantPins(unittest.TestCase):
    def test_format_matches_the_shell_script_byte_for_byte(self) -> None:
        pins = win_webp_dist.compute_want_pins()
        expected = (
            f"libwebp={win_webp_dist.WEBP_VERSION}:{win_webp_dist.WEBP_SHA256} "
            "platform=windows-x86_64 archives=webp+mux+demux+sharpyuv"
        )
        self.assertEqual(pins, expected)


class TestStampFastPath(unittest.TestCase):
    def test_current_when_pins_match_and_libs_exist(self) -> None:
        want = win_webp_dist.compute_want_pins()
        with TemporaryDirectory() as tmp:
            dist = _dist_with(
                {".pins": want, "lib/libwebp.lib": "x", "lib/libwebpmux.lib": "x"}, Path(tmp)
            )
            self.assertTrue(win_webp_dist.stamp_is_current(dist, want))

    def test_not_current_when_a_lib_missing(self) -> None:
        want = win_webp_dist.compute_want_pins()
        with TemporaryDirectory() as tmp:
            dist = _dist_with({".pins": want, "lib/libwebp.lib": "x"}, Path(tmp))
            self.assertFalse(win_webp_dist.stamp_is_current(dist, want))

    def test_not_current_when_pins_differ(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with(
                {".pins": "libwebp=0.0.0:stale", "lib/libwebp.lib": "x", "lib/libwebpmux.lib": "x"},
                Path(tmp),
            )
            self.assertFalse(win_webp_dist.stamp_is_current(dist, win_webp_dist.compute_want_pins()))


class TestAssertLayout(unittest.TestCase):
    def test_green_when_all_present(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {f"lib/{lib}": "x" for lib in win_webp_dist.REQUIRED_LIBS}
            files.update({h: "x" for h in win_webp_dist.REQUIRED_HEADERS})
            dist = _dist_with(files, Path(tmp))
            win_webp_dist.assert_layout(dist)  # must not raise

    def test_red_names_missing_file(self) -> None:
        with TemporaryDirectory() as tmp:
            files = {f"lib/{lib}": "x" for lib in win_webp_dist.REQUIRED_LIBS[:-1]}
            files.update({h: "x" for h in win_webp_dist.REQUIRED_HEADERS})
            dist = _dist_with(files, Path(tmp))
            with self.assertRaises(win_webp_dist.WindowsWebpError) as ctx:
                win_webp_dist.assert_layout(dist)
            self.assertIn(win_webp_dist.REQUIRED_LIBS[-1], str(ctx.exception))


class TestAssertSymbols(unittest.TestCase):
    def test_green_when_all_symbols_present(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = Path(tmp)
            (dist / "lib").mkdir(parents=True)
            (dist / "lib" / "libwebp.lib").write_bytes(b"x")
            (dist / "lib" / "libwebpmux.lib").write_bytes(b"x")
            enc_text = "\n".join(f"0000000 T {s}" for s in win_webp_dist.REQUIRED_ENCODER_SYMBOLS)
            mux_text = "\n".join(f"0000000 T {s}" for s in win_webp_dist.REQUIRED_MUX_SYMBOLS)
            with mock.patch.object(win_webp_dist, "_read_nm_symbols", side_effect=[enc_text, mux_text]):
                win_webp_dist.assert_symbols(dist)  # must not raise

    def test_red_when_encoder_symbol_missing(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = Path(tmp)
            (dist / "lib").mkdir(parents=True)
            (dist / "lib" / "libwebp.lib").write_bytes(b"x")
            with mock.patch.object(win_webp_dist, "_read_nm_symbols", return_value="nothing here"):
                with self.assertRaises(win_pe.PeAssertionFailed) as ctx:
                    win_webp_dist.assert_symbols(dist)
            self.assertIn("WebPEncodeRGBA", str(ctx.exception))


class TestVendorLicense(unittest.TestCase):
    def test_copies_license(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "src"
            dist = root / "dist"
            src.mkdir()
            (src / "COPYING").write_text("bsd", encoding="utf-8")
            win_webp_dist.vendor_license(src, dist)
            self.assertEqual((dist / "share" / "licenses" / "libwebp" / "COPYING").read_text(encoding="utf-8"), "bsd")

    def test_red_when_missing(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "src"
            src.mkdir()
            with self.assertRaises(win_webp_dist.WindowsWebpError):
                win_webp_dist.vendor_license(src, root / "dist")


class TestBuildOrdering(unittest.TestCase):
    def test_skips_build_when_stamp_current(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = Path(tmp)
            with mock.patch.object(win_webp_dist, "stamp_is_current", return_value=True), \
                 mock.patch.object(win_webp_dist, "fetch_source") as m_fetch, \
                 mock.patch.object(win_webp_dist, "configure_build_install") as m_configure, \
                 mock.patch.object(win_webp_dist, "assert_layout") as m_layout, \
                 mock.patch.object(win_webp_dist, "assert_symbols") as m_symbols, \
                 mock.patch.object(win_webp_dist, "vendor_license") as m_license:
                result = win_webp_dist.build(dist)
            self.assertEqual(result, dist)
            m_fetch.assert_not_called()
            m_configure.assert_not_called()
            m_layout.assert_not_called()
            m_symbols.assert_not_called()
            m_license.assert_not_called()

    def test_full_build_runs_every_stage_in_order(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = Path(tmp)
            calls = []
            with mock.patch.object(win_webp_dist, "stamp_is_current", return_value=False), \
                 mock.patch.object(win_webp_dist, "fetch_source", return_value=Path("/src")), \
                 mock.patch.object(
                     win_webp_dist, "configure_build_install",
                     side_effect=lambda *a, **k: calls.append("configure"),
                 ), \
                 mock.patch.object(
                     win_webp_dist, "assert_layout", side_effect=lambda *a, **k: calls.append("layout")
                 ), \
                 mock.patch.object(
                     win_webp_dist, "assert_symbols", side_effect=lambda *a, **k: calls.append("symbols")
                 ), \
                 mock.patch.object(
                     win_webp_dist, "vendor_license", side_effect=lambda *a, **k: calls.append("license")
                 ):
                win_webp_dist.build(dist)
            self.assertEqual(calls, ["configure", "layout", "symbols", "license"])
            self.assertEqual((dist / ".pins").read_text(encoding="utf-8"), win_webp_dist.compute_want_pins())


if __name__ == "__main__":
    unittest.main()
