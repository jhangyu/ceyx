"""Tests for fetch_halide.py -- all runnable on macOS/Linux without network.

Mirrors win_jxl_dist_test.py's TestTranscriptionMatchesTheShellScript
pattern: every pin transcribed from fetch_halide_v21_dist.sh is checked
mechanically against that file's source text, not eyeballed.
"""
from __future__ import annotations

import sys
import tarfile
import zipfile
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from deps import fetch_halide  # noqa: E402

_SH_SCRIPT = Path(__file__).resolve().parents[1] / "fetch_halide_v21_dist.sh"


class TestTranscriptionMatchesTheShellScript(unittest.TestCase):
    def setUp(self) -> None:
        self.sh_text = _SH_SCRIPT.read_text(encoding="utf-8")

    def test_commit_matches(self) -> None:
        self.assertIn(f'HALIDE_COMMIT="{fetch_halide.HALIDE_COMMIT}"', self.sh_text)

    def test_version_matches(self) -> None:
        self.assertIn(f"v{fetch_halide.HALIDE_VERSION}", self.sh_text)

    def test_platform_tags_match(self) -> None:
        for (_, ext) in fetch_halide._PLATFORM_TABLE.values():
            self.assertIn(ext.split(".")[0], self.sh_text)
        for platform_tag, _ in fetch_halide._PLATFORM_TABLE.values():
            self.assertIn(f'PLATFORM="{platform_tag}"', self.sh_text)


class TestNormaliseArch(unittest.TestCase):
    def test_macos_arm64(self) -> None:
        self.assertEqual(fetch_halide.normalise_arch("Darwin", "arm64"), "arm64")

    def test_macos_x86_64(self) -> None:
        self.assertEqual(fetch_halide.normalise_arch("Darwin", "x86_64"), "x86_64")

    def test_linux_aarch64(self) -> None:
        self.assertEqual(fetch_halide.normalise_arch("Linux", "aarch64"), "arm64")

    def test_windows_amd64(self) -> None:
        self.assertEqual(fetch_halide.normalise_arch("Windows", "AMD64"), "x86_64")

    def test_unsupported_raises(self) -> None:
        with self.assertRaises(fetch_halide.HalideFetchError):
            fetch_halide.normalise_arch("Darwin", "riscv64")


class TestResolveAsset(unittest.TestCase):
    def test_macos_arm64_asset(self) -> None:
        platform_tag, ext, asset = fetch_halide.resolve_asset("Darwin", "arm64")
        self.assertEqual(platform_tag, "arm-64-osx")
        self.assertEqual(ext, "tar.gz")
        self.assertEqual(
            asset,
            f"Halide-{fetch_halide.HALIDE_VERSION}-arm-64-osx-{fetch_halide.HALIDE_COMMIT}.tar.gz",
        )

    def test_windows_x86_64_uses_zip(self) -> None:
        _, ext, asset = fetch_halide.resolve_asset("Windows", "AMD64")
        self.assertEqual(ext, "zip")
        self.assertTrue(asset.endswith(".zip"))

    def test_unsupported_os_raises(self) -> None:
        with self.assertRaises(fetch_halide.HalideFetchError):
            fetch_halide.resolve_asset("Plan9", "x86_64")


class TestAlreadyPresent(unittest.TestCase):
    def test_false_when_empty(self) -> None:
        with TemporaryDirectory() as tmp:
            self.assertFalse(fetch_halide.already_present(Path(tmp)))

    def test_true_with_posix_archive(self) -> None:
        with TemporaryDirectory() as tmp:
            dest = Path(tmp)
            (dest / "lib").mkdir()
            (dest / "lib" / "libHalide.a").write_bytes(b"x")
            self.assertTrue(fetch_halide.already_present(dest))

    def test_true_with_windows_import_lib(self) -> None:
        with TemporaryDirectory() as tmp:
            dest = Path(tmp)
            (dest / "lib").mkdir()
            (dest / "lib" / "Halide.lib").write_bytes(b"x")
            self.assertTrue(fetch_halide.already_present(dest))


class TestExtractTarStrippingTop(unittest.TestCase):
    def test_strips_the_single_top_level_directory(self) -> None:
        with TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            src_root = tmp_path / "src"
            (src_root / "Halide-21.0.0-arm-64-osx" / "lib").mkdir(parents=True)
            (src_root / "Halide-21.0.0-arm-64-osx" / "lib" / "libHalide.a").write_bytes(b"x")
            (src_root / "Halide-21.0.0-arm-64-osx" / "VERSION").write_text("v", encoding="utf-8")

            archive_path = tmp_path / "halide.tar.gz"
            with tarfile.open(archive_path, "w:gz") as tf:
                tf.add(src_root / "Halide-21.0.0-arm-64-osx", arcname="Halide-21.0.0-arm-64-osx")

            dest = tmp_path / "dest"
            fetch_halide.extract_tar_stripping_top(archive_path, dest)

            self.assertTrue((dest / "lib" / "libHalide.a").is_file())
            self.assertFalse((dest / "Halide-21.0.0-arm-64-osx").exists())


class TestExtractZipStrippingTop(unittest.TestCase):
    def test_strips_the_single_top_level_directory(self) -> None:
        with TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            archive_path = tmp_path / "halide.zip"
            with zipfile.ZipFile(archive_path, "w") as zf:
                zf.writestr("Halide-21.0.0-x86-64-windows/lib/Halide.lib", "x")
                zf.writestr("Halide-21.0.0-x86-64-windows/VERSION", "v")

            dest = tmp_path / "dest"
            fetch_halide.extract_zip_stripping_top(archive_path, dest)

            self.assertTrue((dest / "lib" / "Halide.lib").is_file())
            self.assertFalse((dest / "Halide-21.0.0-x86-64-windows").exists())

    def test_multiple_top_level_entries_raises(self) -> None:
        with TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            archive_path = tmp_path / "bad.zip"
            with zipfile.ZipFile(archive_path, "w") as zf:
                zf.writestr("a/x.txt", "x")
                zf.writestr("b/y.txt", "y")
            with self.assertRaises(fetch_halide.HalideFetchError):
                fetch_halide.extract_zip_stripping_top(archive_path, tmp_path / "dest")


class TestFetchSkipsWhenPresent(unittest.TestCase):
    def test_fetch_is_a_noop_when_already_present(self) -> None:
        with TemporaryDirectory() as tmp:
            dest = Path(tmp) / "halide"
            (dest / "lib").mkdir(parents=True)
            (dest / "lib" / "libHalide.a").write_bytes(b"x")
            result = fetch_halide.fetch(dest)
            self.assertEqual(result, dest)


if __name__ == "__main__":
    unittest.main()
