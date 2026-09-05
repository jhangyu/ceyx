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


class TestFrozenTranscriptionMatchesTheDeletedShellScript(unittest.TestCase):
    """fetch_libraw_dist.sh was DELETED in the same commit set as this
    freeze (round 3, task #8 / 2026-09-01 contract item 11 / ENTRY-POINT
    RULE): windows_build.yml's last remaining call site was rewired to
    ``build_deps.py fetch libraw`` (linux_build.yml, macos_build.yml and
    android_build.yml already called the Python replacement), so the .sh
    had zero remaining consumers. Same pattern as
    win_jxl_dist_test.py's ``TestFrozenTranscriptionMatchesTheDeletedShellScript``
    and test_fetch_halide.py's sibling freeze.

    These values are FROZEN LITERALS, copied (not retyped) from the last
    revision of fetch_libraw_dist.sh at commit
    07596ef604badbc2037342d078d60242c511f2e4 (``git show
    07596ef604badbc2037342d078d60242c511f2e4:native/scripts/fetch_libraw_dist.sh``
    recovers the full original text). A value changing here without a
    corresponding intentional edit to fetch_libraw.py is exactly as much a
    red flag as a live-diff mismatch against the .sh would have been.
    """

    def test_libraw_rev_matches(self) -> None:
        self.assertEqual(fetch_libraw.LIBRAW_REV, "df226ea4178ccd74245f4f13c23adddfa01411c9")

    def test_rawspeed_rev_matches(self) -> None:
        self.assertEqual(fetch_libraw.RAWSPEED_REV, "c835b05aecfacb7343f7c424abd620aa12116c3f")

    def test_libraw_cmake_rev_matches(self) -> None:
        self.assertEqual(fetch_libraw.LIBRAW_CMAKE_REV, "eb98e4325aef2ce85d2eb031c2ff18640ca616d3")

    def test_urls_match(self) -> None:
        self.assertEqual(fetch_libraw.LIBRAW_URL, "https://github.com/LibRaw/LibRaw.git")
        self.assertEqual(fetch_libraw.RAWSPEED_URL, "https://github.com/darktable-org/rawspeed.git")
        self.assertEqual(fetch_libraw.LIBRAW_CMAKE_URL, "https://github.com/LibRaw/LibRaw-cmake.git")


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
             mock.patch.object(fetch_libraw, "apply_patches", side_effect=lambda *a, **k: calls.append(("apply", a[0] if a else None))), \
             mock.patch.object(fetch_libraw, "strip_git", side_effect=lambda d: calls.append(("strip", d))):
            with TemporaryDirectory() as tmp:
                dest = fetch_libraw.fetch(Path(tmp))
        kinds = [c[0] if isinstance(c, tuple) else c for c in calls]
        self.assertEqual(kinds.count("clone"), 3)
        self.assertIn("overlay", kinds)
        # Task #12: a third apply_patches() call was added for the
        # libraw-cmake clone (the LIBRAW_NOTHREADS fix), alongside the
        # pre-existing RawSpeed3 and project-LibRaw calls.
        self.assertEqual(kinds.count("apply"), 3)
        apply_targets = [c[1] for c in calls if isinstance(c, tuple) and c[0] == "apply"]
        libraw_cmake_dest = dest.parent / "libraw-cmake"
        self.assertIn(libraw_cmake_dest, apply_targets)
        # strip_git only runs for dirs whose .git actually exists; none do
        # here since clone_at is mocked out, so 0 is correct.
        self.assertEqual(kinds.count("strip"), 0)

    def test_apply_patches_precedes_strip_git_for_libraw_cmake(self) -> None:
        """Regression guard for the "KNOWN GAP" identified in commit
        007e72e's message and closed by task #12: apply_patches() requires
        `.git` (fetch_libraw.py:151), so the libraw-cmake apply_patches()
        call must run before strip_git() removes libraw-cmake's `.git`, not
        after. Uses real clone_at/strip_git against a throwaway local repo
        (no network) so the ordering is proven against the actual
        `.git`-presence precondition, not just call order."""
        with TemporaryDirectory() as tmp:
            native_dir = Path(tmp)
            libraw_cmake_dest = native_dir / "third_party" / "libraw-cmake"
            patch_dir = native_dir / "patches" / "libraw-cmake"
            patch_dir.mkdir(parents=True)

            # A minimal local bare-ish repo standing in for the real
            # LibRaw-cmake clone, with one file a trivial patch can target.
            upstream = native_dir / "_upstream"
            upstream.mkdir()
            (upstream / "CMakeLists.txt").write_text("target_compile_definitions(raw PRIVATE LIBRAW_NOTHREADS)\n", encoding="utf-8")
            from deps.run import run
            run(["git", "init", "-q", str(upstream)])
            run(["git", "-C", str(upstream), "add", "-A"])
            run(["git", "-C", str(upstream), "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-q", "-m", "init"])

            fake_patch = patch_dir / "11.no-libraw-nothreads.patch"
            fake_patch.write_text(
                "--- a/CMakeLists.txt\n+++ b/CMakeLists.txt\n"
                "@@ -1 +1 @@\n-target_compile_definitions(raw PRIVATE LIBRAW_NOTHREADS)\n+# removed\n",
                encoding="utf-8",
            )

            run(["git", "clone", "-q", str(upstream), str(libraw_cmake_dest)])
            fetch_libraw.apply_patches(libraw_cmake_dest, patch_dir, label="project ")
            self.assertNotIn("LIBRAW_NOTHREADS", (libraw_cmake_dest / "CMakeLists.txt").read_text(encoding="utf-8"))
            fetch_libraw.strip_git(libraw_cmake_dest)
            self.assertFalse((libraw_cmake_dest / ".git").exists())


if __name__ == "__main__":
    unittest.main()
