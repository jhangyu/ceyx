"""Unit tests for native/scripts/ci/dist_build.py (WI-29, push 8b)."""

from __future__ import annotations

import io
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import dist_build
from .. import run as run_module


def _fake_run_result(returncode=0, stdout="", stderr=""):
    return run_module.RunResult(argv=[], returncode=returncode, stdout=stdout, stderr=stderr)


def _run_captured(func, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = func(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


class DistBuildTests(unittest.TestCase):
    def test_invokes_build_deps_with_component_platform_arch_dist(self):
        captured = {}

        def fake_run(argv, cwd=None, env=None):
            captured["argv"] = argv
            return _fake_run_result(returncode=0, stdout="built\n")

        with mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, err = _run_captured(
                dist_build.dist_build,
                component="heif-stack", platform="windows", arch="x86_64",
                dist="native/third_party/heif-dist-windows", rc_marker="HEIF_DIST_WINDOWS_RC",
            )
        self.assertEqual(rc, 0)
        argv = captured["argv"]
        self.assertIn("build_deps.py", argv[1])
        self.assertEqual(argv[2], "build")
        self.assertEqual(argv[3], "heif-stack")
        self.assertIn("--platform", argv)
        self.assertEqual(argv[argv.index("--platform") + 1], "windows")
        self.assertIn("--arch", argv)
        self.assertEqual(argv[argv.index("--arch") + 1], "x86_64")
        self.assertIn("--dist", argv)
        self.assertEqual(argv[argv.index("--dist") + 1], "native/third_party/heif-dist-windows")
        self.assertNotIn("--android-ndk", argv)
        self.assertIn("HEIF_DIST_WINDOWS_RC=0", out)

    def test_android_ndk_appended_only_when_given(self):
        captured = {}

        def fake_run(argv, cwd=None, env=None):
            captured["argv"] = argv
            return _fake_run_result(returncode=0)

        with mock.patch.object(run_module, "run", side_effect=fake_run):
            _run_captured(
                dist_build.dist_build,
                component="libjxl", platform="android", arch="arm64-v8a",
                dist="d", rc_marker="T", android_ndk="/opt/ndk",
            )
        argv = captured["argv"]
        self.assertIn("--android-ndk", argv)
        self.assertEqual(argv[argv.index("--android-ndk") + 1], "/opt/ndk")

    # ---- red-then-green: non-zero child rc propagates, marker still prints ----

    def test_nonzero_child_rc_propagates_and_marker_still_prints(self):
        def fake_run(argv, cwd=None, env=None):
            return _fake_run_result(returncode=7, stdout="", stderr="boom")

        with mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, out, err = _run_captured(
                dist_build.dist_build,
                component="webp-stack", platform="windows", arch="x86_64",
                dist="d", rc_marker="WEBP_DIST_WINDOWS_RC",
            )
        self.assertEqual(rc, 7)
        self.assertIn("WEBP_DIST_WINDOWS_RC=7", out)
        self.assertIn("boom", out)

    def test_rc_read_from_result_object_not_a_shell_variable(self):
        # RC= grep AC: this module never shells out to capture $? -- the rc
        # comes straight off RunResult.returncode. Assert that field, not a
        # string parse of stdout.
        def fake_run(argv, cwd=None, env=None):
            return _fake_run_result(returncode=3)

        with mock.patch.object(run_module, "run", side_effect=fake_run):
            rc, _, _ = _run_captured(
                dist_build.dist_build,
                component="c", platform="windows", arch="x86_64",
                dist="d", rc_marker="T",
            )
        self.assertEqual(rc, 3)


class DistListTests(unittest.TestCase):
    def setUp(self):
        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)
        self.tmp = Path(self._tmpdir.name)

    def test_missing_directory_exits_nonzero(self):
        rc, out, err = _run_captured(dist_build.dist_list, str(self.tmp / "does-not-exist"))
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")

    # ---- red-then-green: empty tree exits non-zero rather than printing nothing ----

    def test_empty_tree_exits_nonzero_rather_than_silently_printing_nothing(self):
        empty = self.tmp / "empty-dist"
        empty.mkdir()
        rc, out, err = _run_captured(dist_build.dist_list, str(empty))
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")

    def test_ordering_byte_identical_to_find_pipe_sort_including_dotfile(self):
        dist = self.tmp / "dist"
        (dist / "sub").mkdir(parents=True)
        (dist / "b.txt").write_text("b")
        (dist / "a.txt").write_text("a")
        (dist / ".pins").write_text("pin")
        (dist / "sub" / "c.txt").write_text("c")

        expected = _find_pipe_sort(dist)

        rc, out, err = _run_captured(dist_build.dist_list, str(dist))
        self.assertEqual(rc, 0)
        actual = out.splitlines()
        self.assertEqual(actual, expected)
        self.assertIn(str(dist / ".pins"), actual)


def _find_pipe_sort(root: Path) -> list:
    """Reference oracle: what `find <root> -type f | sort` actually produces
    (byte/codepoint sort, same as the shell's C-locale `sort`), used only to
    assert dist_list's ordering against it -- not a reimplementation the
    module depends on."""
    paths = [str(p) for p in root.rglob("*") if p.is_file()]
    return sorted(paths)


if __name__ == "__main__":
    unittest.main()
