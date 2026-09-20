"""Unit tests for native/scripts/deps/fetch.py (D3 round 1, A3.3).

Run with: python3 -m pytest native/scripts/deps/test_fetch.py -v
(or python3 native/scripts/deps/test_fetch.py to run standalone via unittest).
"""
from __future__ import annotations

import hashlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fetch as fetch_module  # noqa: E402


def _sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class _FakeResponse:
    """Minimal context-manager stand-in for urllib.request.urlopen()."""

    def __init__(self, payload: bytes) -> None:
        self._payload = payload

    def __enter__(self) -> "_FakeResponse":
        return self

    def __exit__(self, *exc_info) -> None:
        return None

    def read(self, size: int = -1) -> bytes:
        if size < 0 or size >= len(self._payload):
            data, self._payload = self._payload, b""
            return data
        data, self._payload = self._payload[:size], self._payload[size:]
        return data


class TestVerifySha256(unittest.TestCase):
    def test_matching_hash_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "good.bin"
            payload = b"the quick brown fox"
            path.write_bytes(payload)
            fetch_module.verify_sha256(path, _sha256_hex(payload))  # must not raise

    def test_mismatched_hash_raises_naming_file_and_both_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "corrupt.bin"
            payload = b"not what you expected"
            path.write_bytes(payload)
            wrong_hash = _sha256_hex(b"something else entirely")
            with self.assertRaises(fetch_module.FetchError) as ctx:
                fetch_module.verify_sha256(path, wrong_hash)
            message = str(ctx.exception)
            self.assertIn(str(path), message)
            self.assertIn(wrong_hash, message)
            self.assertIn(_sha256_hex(payload), message)


class TestFetchTarball(unittest.TestCase):
    def test_good_input_passes_and_file_is_retained(self) -> None:
        payload = b"a legitimate tarball's bytes"
        good_sha256 = _sha256_hex(payload)
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "download" / "component.tar.gz"
            with mock.patch.object(
                fetch_module.urllib.request, "urlopen", return_value=_FakeResponse(payload)
            ):
                result = fetch_module.fetch_tarball("https://example.invalid/component.tar.gz", good_sha256, dest)
            self.assertEqual(result, dest)
            self.assertTrue(dest.exists())
            self.assertEqual(dest.read_bytes(), payload)
            self.assertFalse(dest.with_name(dest.name + ".part").exists())

    def test_corrupt_input_fails_and_partial_file_is_removed(self) -> None:
        payload = b"bytes that got corrupted in transit"
        wrong_sha256 = _sha256_hex(b"the bytes we actually expected")
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "download" / "component.tar.gz"
            with mock.patch.object(
                fetch_module.urllib.request, "urlopen", return_value=_FakeResponse(payload)
            ):
                with self.assertRaises(fetch_module.FetchError) as ctx:
                    fetch_module.fetch_tarball("https://example.invalid/component.tar.gz", wrong_sha256, dest)
            message = str(ctx.exception)
            self.assertIn(str(dest), message)
            self.assertIn(wrong_sha256, message)
            self.assertIn(_sha256_hex(payload), message)
            # The corrupted file must not be left behind for a later run to
            # mistake for a validated artefact (A3.3).
            self.assertFalse(dest.exists())
            self.assertFalse(dest.with_name(dest.name + ".part").exists())

    def test_download_failure_removes_partial_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "download" / "component.tar.gz"
            with mock.patch.object(
                fetch_module.urllib.request,
                "urlopen",
                side_effect=fetch_module.urllib.error.URLError("connection refused"),
            ):
                with self.assertRaises(fetch_module.FetchError):
                    fetch_module.fetch_tarball("https://example.invalid/component.tar.gz", "deadbeef", dest)
            self.assertFalse(dest.exists())
            self.assertFalse(dest.with_name(dest.name + ".part").exists())


class TestWorktreeDistReuse(unittest.TestCase):
    """2026-09-20 parking-lot item 2: a fresh `git worktree add` should
    reuse the main tree's already-fetched gitignored dist dirs instead of
    re-fetching. Exercised against real throwaway git repos (main tree +
    linked worktree), no network."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.main_root = Path(self._tmp.name) / "main"
        self.main_root.mkdir()
        run = fetch_module.run
        run(["git", "init", "-q", str(self.main_root)])
        run(["git", "-C", str(self.main_root), "-c", "user.email=t@t", "-c", "user.name=t",
             "commit", "-q", "--allow-empty", "-m", "init"])
        self.worktree_root = Path(self._tmp.name) / "wt"
        run(["git", "-C", str(self.main_root), "worktree", "add", "-q", str(self.worktree_root), "-b", "wt-branch"])

    def test_not_a_worktree_returns_none(self) -> None:
        self.assertIsNone(fetch_module.find_worktree_main_tree_root(self.main_root))

    def test_linked_worktree_resolves_main_root(self) -> None:
        resolved = fetch_module.find_worktree_main_tree_root(self.worktree_root)
        self.assertEqual(resolved, self.main_root.resolve())

    def test_symlinks_to_main_tree_dist_when_present_and_valid(self) -> None:
        main_dist = self.main_root / "native" / "third_party" / "halide"
        main_dist.mkdir(parents=True)
        (main_dist / "marker").write_text("real dist\n", encoding="utf-8")

        wt_native = self.worktree_root / "native"
        wt_dest = wt_native / "third_party" / "halide"
        # mirrors the real repo, where native/third_party/ itself is tracked
        # (it holds other non-gitignored vendored dirs) even though the
        # individual halide subdirectory is gitignored/untracked.
        wt_dest.parent.mkdir(parents=True)
        reused = fetch_module.reuse_worktree_dist(
            wt_dest,
            is_valid=lambda p: (p / "marker").is_file(),
            this_native_dir=wt_native,
        )
        self.assertTrue(reused)
        self.assertTrue(wt_dest.is_symlink())
        self.assertEqual((wt_dest / "marker").read_text(encoding="utf-8"), "real dist\n")

    def test_does_not_reuse_when_main_tree_dist_missing(self) -> None:
        wt_native = self.worktree_root / "native"
        wt_dest = wt_native / "third_party" / "halide"
        reused = fetch_module.reuse_worktree_dist(
            wt_dest, is_valid=lambda p: (p / "marker").is_file(), this_native_dir=wt_native
        )
        self.assertFalse(reused)
        self.assertFalse(wt_dest.exists())

    def test_does_not_reuse_when_dest_already_exists(self) -> None:
        main_dist = self.main_root / "native" / "third_party" / "halide"
        main_dist.mkdir(parents=True)
        (main_dist / "marker").write_text("real dist\n", encoding="utf-8")

        wt_native = self.worktree_root / "native"
        wt_dest = wt_native / "third_party" / "halide"
        wt_dest.mkdir(parents=True)
        reused = fetch_module.reuse_worktree_dist(
            wt_dest, is_valid=lambda p: (p / "marker").is_file(), this_native_dir=wt_native
        )
        self.assertFalse(reused)
        self.assertFalse(wt_dest.is_symlink())


if __name__ == "__main__":
    unittest.main()
