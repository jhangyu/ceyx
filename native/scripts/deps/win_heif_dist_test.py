"""Tests for win_heif_dist.py -- all runnable on macOS/Linux (spec §8.1).

Nothing here invokes cmake, clang-cl or dumpbin: the parts that CAN only be
proven on a Windows runner (that the dist links, that heif.dll really exports
heif_decode_image) are CI's job and are asserted there by the module itself.
What is proven here is the logic that would otherwise only be exercised on
Windows and would therefore be debugged one 20-minute CI round at a time:
pin-string derivation, the stamp fast path, layout resolution, and the
decode-only assertion wiring.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

# Imported through the PACKAGE (`deps.`), never flat: a flat `import
# win_heif_dist` sends its dependencies down their direct-execution fallback
# branch, where deps/render.py's own relative import has no parent package and
# raises. Going through the package also keeps exception identity single --
# `deps.win_pe.PeInspectionError` raised inside the module is the same class
# object this test catches, which a flat second copy would silently break.
from deps import manifest, win_heif_dist, win_pe  # noqa: E402


def _dist_with(files: dict, root: Path) -> Path:
    for relative, content in files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
    return root


class TestWantPins(unittest.TestCase):
    def setUp(self) -> None:
        self.loaded = manifest.load()

    def test_pins_are_derived_from_the_manifest_not_restated(self) -> None:
        pins = win_heif_dist.want_pins(self.loaded, "x86_64")
        components = self.loaded["manifest"]["component"]
        self.assertIn(f"libheif={components['libheif']['version']}:", pins)
        self.assertIn(f"libde265={components['libde265']['version']}:", pins)
        self.assertTrue(pins.endswith("platform=windows-x86_64"))

    def test_pins_use_the_windows_de265_hash(self) -> None:
        """libde265 resolves to vcpkg on Unix but to a SHA-256-pinned tarball
        on Windows; the stamp must carry the Windows hash, not the Unix
        'vcpkg' marker."""
        pins = win_heif_dist.want_pins(self.loaded, "x86_64")
        windows_sha = self.loaded["manifest"]["component"]["libde265"]["source"]["windows"]["sha256"]
        self.assertIn(windows_sha, pins)
        self.assertNotIn("vcpkg", pins)

    def test_format_matches_the_shell_script_byte_for_byte(self) -> None:
        """Compatibility with a .pins written by build_heif_dist_windows.sh is
        what makes the shell->Python switchover a no-op for an already-built
        tree instead of a forced rebuild."""
        components = self.loaded["manifest"]["component"]
        expected = (
            f"libheif={components['libheif']['version']}:"
            f"{components['libheif']['source']['default']['sha256']} "
            f"libde265={components['libde265']['version']}:"
            f"{components['libde265']['source']['windows']['sha256']} "
            f"platform=windows-x86_64"
        )
        self.assertEqual(win_heif_dist.want_pins(self.loaded, "x86_64"), expected)


class TestStampFastPath(unittest.TestCase):
    def setUp(self) -> None:
        self.loaded = manifest.load()

    def test_current_when_pins_match_and_both_dlls_exist(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with(
                {
                    ".pins": win_heif_dist.want_pins(self.loaded, "x86_64"),
                    "bin/heif.dll": "x",
                    "bin/libde265.dll": "x",
                },
                Path(tmp),
            )
            self.assertTrue(win_heif_dist.stamp_is_current(dist, self.loaded, "x86_64"))

    def test_not_current_when_a_dll_was_deleted(self) -> None:
        """A stamp can outlive a deleted DLL; the fast path must not then
        declare a dist that ships nothing to be ready."""
        with TemporaryDirectory() as tmp:
            dist = _dist_with(
                {".pins": win_heif_dist.want_pins(self.loaded, "x86_64"), "bin/heif.dll": "x"},
                Path(tmp),
            )
            self.assertFalse(win_heif_dist.stamp_is_current(dist, self.loaded, "x86_64"))

    def test_not_current_when_pins_differ(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with(
                {".pins": "libheif=0.0.1:deadbeef", "bin/heif.dll": "x", "bin/libde265.dll": "x"},
                Path(tmp),
            )
            self.assertFalse(win_heif_dist.stamp_is_current(dist, self.loaded, "x86_64"))

    def test_not_current_when_no_stamp_at_all(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with({"bin/heif.dll": "x", "bin/libde265.dll": "x"}, Path(tmp))
            self.assertFalse(win_heif_dist.stamp_is_current(dist, self.loaded, "x86_64"))

    def test_accepts_the_alternate_de265_dll_spelling(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with(
                {
                    ".pins": win_heif_dist.want_pins(self.loaded, "x86_64"),
                    "bin/heif.dll": "x",
                    "bin/de265.dll": "x",
                },
                Path(tmp),
            )
            self.assertTrue(win_heif_dist.stamp_is_current(dist, self.loaded, "x86_64"))


class TestLayoutAssertion(unittest.TestCase):
    COMPLETE = {
        "bin/heif.dll": "x",
        "bin/libde265.dll": "x",
        "lib/heif.lib": "x",
        "lib/de265.lib": "x",
        "include/libheif/heif.h": "x",
        "include/libde265/de265.h": "x",
    }

    def test_green_and_returns_the_resolved_dll(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with(dict(self.COMPLETE), Path(tmp))
            self.assertEqual(win_heif_dist.assert_layout(dist), "bin/libde265.dll")

    def test_red_names_the_missing_file(self) -> None:
        with TemporaryDirectory() as tmp:
            files = dict(self.COMPLETE)
            del files["lib/heif.lib"]
            dist = _dist_with(files, Path(tmp))
            with self.assertRaises(win_heif_dist.WindowsHeifError) as ctx:
                win_heif_dist.assert_layout(dist)
            self.assertIn("lib/heif.lib", str(ctx.exception))

    def test_import_library_asymmetry_is_preserved(self) -> None:
        """Upstream ships bin/libde265.dll alongside lib/de265.lib. The DLL is
        NOT renamed to match: heif.dll's import table names "libde265.dll", so
        a tidied copy would never be loaded."""
        with TemporaryDirectory() as tmp:
            dist = _dist_with(dict(self.COMPLETE), Path(tmp))
            self.assertEqual(win_heif_dist.assert_layout(dist), "bin/libde265.dll")
            self.assertEqual(win_heif_dist.resolve_de265_import_library(dist), "lib/de265.lib")

    def test_missing_de265_dll_is_reported_with_a_listing(self) -> None:
        with TemporaryDirectory() as tmp:
            files = dict(self.COMPLETE)
            del files["bin/libde265.dll"]
            dist = _dist_with(files, Path(tmp))
            with self.assertRaises(win_pe.PeInspectionError) as ctx:
                win_heif_dist.assert_layout(dist)
            self.assertIn("heif.dll", str(ctx.exception))


class TestDecodeOnlyAssertionSet(unittest.TestCase):
    """The Windows dist is decode-only, so its assertion set must differ from
    heif.py's Unix one -- which requires kvazaar and aom ENCODER symbols this
    dist intentionally does not contain."""

    def test_does_not_require_encoder_symbols(self) -> None:
        source = Path(win_heif_dist.__file__).read_text(encoding="utf-8")
        required = win_heif_dist._REQUIRED_HEIF_SYMBOL
        self.assertEqual(required, "heif_decode_image")
        for encoder_symbol in ("kvz_api_get", "aom_codec_av1_cx", "aom_codec_av1_dx"):
            self.assertNotIn(f'"{encoder_symbol}"', source)

    def test_gpl_contamination_scan_targets_x265(self) -> None:
        self.assertEqual(win_heif_dist._FORBIDDEN_HEIF_SYMBOL, "x265")

    def test_hevc_dependency_is_asserted(self) -> None:
        self.assertEqual(win_heif_dist._REQUIRED_HEIF_DEPENDENCY, "de265")


if __name__ == "__main__":
    unittest.main()
