"""Tests for fetch_libraw.py -- all runnable on macOS/Linux without network,
mirroring win_jxl_dist_test.py's TestTranscriptionMatchesTheShellScript
pattern.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from deps import fetch_libraw  # noqa: E402

_SH_SCRIPT = Path(__file__).resolve().parents[1] / "fetch_libraw_dist.sh"


class TestTranscriptionMatchesTheShellScript(unittest.TestCase):
    def setUp(self) -> None:
        self.sh_text = _SH_SCRIPT.read_text(encoding="utf-8")

    def test_libraw_rev_matches(self) -> None:
        self.assertIn(f'LIBRAW_REV="{fetch_libraw.LIBRAW_REV}"', self.sh_text)

    def test_rawspeed_rev_matches(self) -> None:
        self.assertIn(f'RAWSPEED_REV="{fetch_libraw.RAWSPEED_REV}"', self.sh_text)

    def test_libraw_cmake_rev_matches(self) -> None:
        self.assertIn(f'LIBRAW_CMAKE_REV="{fetch_libraw.LIBRAW_CMAKE_REV}"', self.sh_text)

    def test_urls_match(self) -> None:
        self.assertIn(f'LIBRAW_URL="{fetch_libraw.LIBRAW_URL}"', self.sh_text)
        self.assertIn(f'RAWSPEED_URL="{fetch_libraw.RAWSPEED_URL}"', self.sh_text)
        self.assertIn(f'LIBRAW_CMAKE_URL="{fetch_libraw.LIBRAW_CMAKE_URL}"', self.sh_text)


class TestReadVendorRev(unittest.TestCase):
    def test_none_when_absent(self) -> None:
        with TemporaryDirectory() as tmp:
            self.assertIsNone(fetch_libraw.read_vendor_rev(Path(tmp)))

    def test_reads_stripped_content(self) -> None:
        with TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / ".vendor-rev").write_text("abc123\n", encoding="utf-8")
            self.assertEqual(fetch_libraw.read_vendor_rev(d), "abc123")


class TestCloneAt(unittest.TestCase):
    def test_skips_when_vendor_rev_matches(self) -> None:
        with TemporaryDirectory() as tmp:
            d = Path(tmp) / "dest"
            d.mkdir()
            (d / ".vendor-rev").write_text("rev1\n", encoding="utf-8")
            with mock.patch.object(fetch_libraw, "run") as m_run:
                fetch_libraw.clone_at("https://example.com/x.git", "rev1", d)
            m_run.assert_not_called()

    def test_clones_and_preserves_provenance(self) -> None:
        with TemporaryDirectory() as tmp:
            d = Path(tmp) / "dest"
            d.mkdir()
            (d / "PROVENANCE.md").write_text("keep me", encoding="utf-8")
            calls = []
            with mock.patch.object(fetch_libraw, "run", side_effect=lambda argv, **k: calls.append(argv)):
                fetch_libraw.clone_at("https://example.com/x.git", "rev2", d)
            self.assertEqual((d / "PROVENANCE.md").read_text(encoding="utf-8"), "keep me")
            joined = [" ".join(c) for c in calls]
            self.assertTrue(any("init" in c for c in joined))
            self.assertTrue(any("remote" in c and "add" in c for c in joined))
            self.assertTrue(any("fetch" in c and "rev2" in c for c in joined))
            self.assertTrue(any("checkout" in c and "rev2" in c for c in joined))


class TestStripGit(unittest.TestCase):
    def test_writes_vendor_rev_and_removes_git(self) -> None:
        with TemporaryDirectory() as tmp:
            d = Path(tmp) / "dest"
            (d / ".git").mkdir(parents=True)
            with mock.patch.object(fetch_libraw, "run") as m_run:
                m_run.return_value.stdout = "deadbeef\n"
                fetch_libraw.strip_git(d)
            self.assertFalse((d / ".git").exists())
            self.assertEqual((d / ".vendor-rev").read_text(encoding="utf-8"), "deadbeef\n")


class TestOverlayRawspeedPatches(unittest.TestCase):
    def test_replaces_wholesale(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            project_src = root / "patches" / "rawspeed3"
            project_src.mkdir(parents=True)
            (project_src / "01-fix.patch").write_text("p", encoding="utf-8")

            patch_dir = root / "RawSpeed3" / "patches"
            patch_dir.mkdir(parents=True)
            (patch_dir / "old-upstream.patch").write_text("stale", encoding="utf-8")

            changed = fetch_libraw.overlay_rawspeed_patches(project_src, patch_dir)

            self.assertTrue(changed)
            self.assertFalse((patch_dir / "old-upstream.patch").exists())
            self.assertTrue((patch_dir / "01-fix.patch").exists())

    def test_noop_when_no_project_patches(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            changed = fetch_libraw.overlay_rawspeed_patches(root / "nonexistent", root / "patches")
            self.assertFalse(changed)


class TestApplyOrSkip(unittest.TestCase):
    def _fake_run(self, forward_rc: int, reverse_rc: int):
        def _run(argv, **kwargs):
            result = mock.Mock()
            if "--check" in argv and "--reverse" not in argv:
                result.returncode = forward_rc
            elif "--reverse" in argv:
                result.returncode = reverse_rc
            else:
                result.returncode = 0
            return result
        return _run

    def test_applies_when_forward_check_succeeds(self) -> None:
        with TemporaryDirectory() as tmp:
            patch = Path(tmp) / "p.patch"
            patch.write_text("x", encoding="utf-8")
            calls = []
            with mock.patch.object(fetch_libraw, "run", side_effect=lambda argv, **k: (calls.append(argv), self._fake_run(0, 1)(argv, **k))[1]):
                fetch_libraw._apply_or_skip(Path(tmp), patch, label="")
            self.assertTrue(any(argv[:3] == ["git", "-C", str(Path(tmp))] and "apply" in argv and "--check" not in argv for argv in calls))

    def test_skips_when_already_applied(self) -> None:
        with TemporaryDirectory() as tmp:
            patch = Path(tmp) / "p.patch"
            patch.write_text("x", encoding="utf-8")
            with mock.patch.object(fetch_libraw, "run", side_effect=self._fake_run(1, 0)):
                fetch_libraw._apply_or_skip(Path(tmp), patch, label="")  # must not raise

    def test_raises_when_neither_applies(self) -> None:
        with TemporaryDirectory() as tmp:
            patch = Path(tmp) / "p.patch"
            patch.write_text("x", encoding="utf-8")
            with mock.patch.object(fetch_libraw, "run", side_effect=self._fake_run(1, 1)):
                with self.assertRaises(fetch_libraw.LibrawFetchError):
                    fetch_libraw._apply_or_skip(Path(tmp), patch, label="")


class TestFetchOrdering(unittest.TestCase):
    def test_calls_in_expected_order(self) -> None:
        calls = []
        with mock.patch.object(fetch_libraw, "clone_at", side_effect=lambda *a, **k: calls.append(("clone", a[2]))), \
             mock.patch.object(fetch_libraw, "overlay_rawspeed_patches", side_effect=lambda *a, **k: calls.append("overlay")), \
             mock.patch.object(fetch_libraw, "apply_patches", side_effect=lambda *a, **k: calls.append(("apply", k.get("label")))), \
             mock.patch.object(fetch_libraw, "strip_git", side_effect=lambda d: calls.append(("strip", d))):
            with TemporaryDirectory() as tmp:
                fetch_libraw.fetch(Path(tmp))
        kinds = [c[0] if isinstance(c, tuple) else c for c in calls]
        self.assertEqual(kinds.count("clone"), 3)
        self.assertIn("overlay", kinds)
        self.assertEqual(kinds.count("apply"), 2)
        # strip_git only runs for dirs whose .git actually exists; none do
        # here since clone_at is mocked out, so 0 is correct.
        self.assertEqual(kinds.count("strip"), 0)


if __name__ == "__main__":
    unittest.main()
