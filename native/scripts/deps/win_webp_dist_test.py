"""Tests for win_webp_dist.py -- all runnable on macOS/Linux (spec §8.1).

Round 3: the carrier was proven green on a real Windows CI leg (run
https://github.com/jhangyu/ceyx/actions/runs/33422633434, head
522e913b4b6178f1c128bf3b48058e88603ad36c, self-captured
``WEBP_DIST_WINDOWS_RC=0``) and the produced dist was byte-compared against
the committed native/third_party/libwebp-dist-windows tree (headers,
pkgconfig, cmake, licence and PROVENANCE.md are digest-identical; the five
``.lib`` binaries differ -- non-reproducible compiler output across build
runs, not a logic regression -- see docs/logs for the full finding).
``build_libwebp_dist_windows.sh`` was then deleted; its transcription test
is frozen below to literals, same pattern as win_jxl_dist_test.py's
``TestFrozenTranscriptionMatchesTheDeletedShellScript``.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from deps import win_pe, win_webp_dist  # noqa: E402


def _dist_with(files: dict, root: Path) -> Path:
    for relative, content in files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
    return root


class TestFrozenTranscriptionMatchesTheDeletedShellScript(unittest.TestCase):
    """build_libwebp_dist_windows.sh was DELETED in this commit set (round 3
    task #12 closeout) once the carrier-built dist was proven green on a
    real Windows CI leg (run 33422633434, head
    522e913b4b6178f1c128bf3b48058e88603ad36c).

    These values are FROZEN LITERALS, copied (not retyped) from the last
    revision of ``build_libwebp_dist_windows.sh`` at commit
    d125df8e830b9c7e98541b34fff42ab694abde2b (``git show
    d125df8e830b9c7e98541b34fff42ab694abde2b:native/scripts/build_libwebp_dist_windows.sh``
    recovers the full original text). A value changing here without a
    corresponding intentional edit to ``win_webp_dist.py`` is exactly as
    much a red flag as a live-diff mismatch against the .sh would have been.
    """

    def test_version_matches(self) -> None:
        self.assertEqual(win_webp_dist.WEBP_VERSION, "1.6.0")

    def test_sha256_matches(self) -> None:
        self.assertEqual(
            win_webp_dist.WEBP_SHA256,
            "e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564",
        )

    def test_url_pattern_matches(self) -> None:
        self.assertEqual(
            win_webp_dist.WEBP_URL,
            "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz",
        )

    def test_required_libs_match(self) -> None:
        self.assertEqual(
            win_webp_dist.REQUIRED_LIBS,
            ("libwebp.lib", "libwebpmux.lib", "libwebpdemux.lib", "libsharpyuv.lib"),
        )

    def test_required_headers_match(self) -> None:
        self.assertEqual(
            win_webp_dist.REQUIRED_HEADERS,
            ("include/webp/encode.h", "include/webp/decode.h", "include/webp/mux.h"),
        )

    def test_encoder_symbols_match(self) -> None:
        self.assertEqual(
            win_webp_dist.REQUIRED_ENCODER_SYMBOLS,
            ("WebPEncodeRGBA", "WebPEncodeLosslessRGBA"),
        )

    def test_mux_symbols_match(self) -> None:
        self.assertEqual(win_webp_dist.REQUIRED_MUX_SYMBOLS, ("WebPMuxSetChunk", "WebPMuxAssemble"))

    def test_cmake_flags_match(self) -> None:
        self.assertEqual(
            win_webp_dist._CMAKE_ARGS_BASE,
            (
                "-DCMAKE_BUILD_TYPE=Release",
                "-DCMAKE_C_COMPILER=clang-cl",
                "-DCMAKE_CXX_COMPILER=clang-cl",
                "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
                "-DBUILD_SHARED_LIBS=OFF",
                "-DWEBP_BUILD_ANIM_UTILS=OFF",
                "-DWEBP_BUILD_CWEBP=OFF",
                "-DWEBP_BUILD_DWEBP=OFF",
                "-DWEBP_BUILD_GIF2WEBP=OFF",
                "-DWEBP_BUILD_IMG2WEBP=OFF",
                "-DWEBP_BUILD_VWEBP=OFF",
                "-DWEBP_BUILD_WEBPINFO=OFF",
                "-DWEBP_BUILD_WEBPMUX=OFF",
                "-DWEBP_BUILD_EXTRAS=OFF",
            ),
        )

    def test_multithreaded_runtime_not_dynamic(self) -> None:
        self.assertIn("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", win_webp_dist._CMAKE_ARGS_BASE)


class TestCmakeConfigureArgs(unittest.TestCase):
    def test_install_prefix_is_the_dist_dir(self) -> None:
        dist = Path("/tmp/webp-dist")
        argv = win_webp_dist.cmake_configure_args(dist)
        # str(dist), not a POSIX literal: the CI Windows leg runs this suite
        # on native Windows Python, where Path("/tmp/...") renders as \tmp\...
        self.assertIn(f"-DCMAKE_INSTALL_PREFIX={dist}", argv)

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
