"""Refactor T3 (2026-10-02): the four NDK-tool sites resolve through
deps.assertions.ndk_tool, the one NDK resolver.

Pins: (1) every site routes through its module's `ndk_tool` name;
(2) on a linux-x86_64 NDK the resolved path equals the old fixed join;
(3) a missing tool -> one `::error::<tool> not found at` line + RC 1,
before any tool is run."""
from __future__ import annotations

import io
import os
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import dt_needed, orientation, verify_artifact
from .. import run as run_module


class _Stop(Exception):
    """Raised by the fake runner after recording the first argv."""


def _captured(fn, *args, **kwargs):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = fn(*args, **kwargs)
    return rc, out.getvalue(), err.getvalue()


class NdkToolAdoptionTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        old_cwd = os.getcwd()
        os.chdir(self.base)
        self.addCleanup(os.chdir, old_cwd)
        self.ndk = self.base / "ndk"
        self.bin = self.ndk / "toolchains" / "llvm" / "prebuilt" / "linux-x86_64" / "bin"
        self.bin.mkdir(parents=True)
        for tool in ("llvm-readelf", "llvm-nm"):
            path = self.bin / tool
            path.write_text("#!/bin/sh\n")
            path.chmod(0o755)
        self.artifacts = self.base / "artifacts"
        (self.artifacts / "native").mkdir(parents=True)
        self.so = self.artifacts / "native" / "libdng_decoder_native.so"
        self.so.write_bytes(b"")
        self.runner_temp = self.base / "runner_temp"
        self.runner_temp.mkdir()

    def _old_join(self, tool):
        return os.path.join(str(self.ndk), "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin", tool)

    def _first_argv(self, fn, *args, **kwargs):
        seen = []

        def fake(argv, cwd=None, env=None):
            seen.append([str(a) for a in argv])
            raise _Stop

        with mock.patch.object(run_module, "run", side_effect=fake):
            with self.assertRaises(_Stop):
                _captured(fn, *args, **kwargs)
        return seen[0]

    def _sites(self):
        """(module, tool, callable running that site) for all four sites."""
        return (
            (dt_needed, "llvm-readelf", lambda: dt_needed.dt_needed(
                "android", str(self.artifacts), str(self.runner_temp), ndk_home=str(self.ndk),
                build_log=str(self.base / "android_build.log"))),
            (verify_artifact, "llvm-readelf", lambda: verify_artifact.import_closure(
                "android", artifact_dir=str(self.artifacts), ndk_home=str(self.ndk))),
            (verify_artifact, "llvm-nm", lambda: verify_artifact.assert_exports(
                "android", artifact_dir=str(self.artifacts), ndk_home=str(self.ndk))),
            (orientation, "llvm-nm", lambda: orientation._android_scan(
                str(self.so), "android_so_strings.txt", str(self.ndk))),
        )

    def test_linux_ndk_resolves_same_path_as_old_join(self):
        for _module, tool, call in self._sites():
            with self.subTest(tool=tool, call=call):
                argv = self._first_argv(call)
                self.assertEqual(argv[0], self._old_join(tool))

    def test_sites_route_through_ndk_tool(self):
        """A host dir other than linux-x86_64 (e.g. a local macOS NDK) is
        found only if the site asks ndk_tool instead of joining a path."""
        for module, tool, call in self._sites():
            with self.subTest(module=module.__name__, tool=tool):
                fake_path = Path("/fake-ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin") / tool
                with mock.patch.object(module, "ndk_tool", return_value=fake_path) as fake_tool:
                    argv = self._first_argv(call)
                fake_tool.assert_called_with(str(self.ndk), tool)
                self.assertEqual(argv[0], str(fake_path))

    def test_missing_tool_errors_before_running_anything(self):
        for tool in ("llvm-readelf", "llvm-nm"):
            (self.bin / tool).unlink()
        for _module, tool, call in self._sites():
            with self.subTest(tool=tool, call=call):
                with mock.patch.object(run_module, "run", side_effect=AssertionError("ran a tool")):
                    rc, _out, err = _captured(call)
                self.assertEqual(rc, 1)
                self.assertIn(f"::error::{tool} not found at {self.ndk}/toolchains/llvm/prebuilt/*/bin", err)

    def test_orientation_missing_llvm_nm_errors_before_running(self):
        (self.bin / "llvm-nm").unlink()
        with mock.patch.object(run_module, "run", side_effect=AssertionError("ran a tool")):
            rc, out, err = _captured(orientation._android_scan, str(self.so), "android_so_strings.txt", str(self.ndk))
        self.assertEqual(rc, 1)
        self.assertNotIn("NM_RC=", out)
        self.assertIn("::error::llvm-nm not found at", err)


if __name__ == "__main__":
    unittest.main()
