"""Refactor T4 (2026-10-02): marker/notice surface of verify_artifact's two
dumpbin->LLVM fallback callers, pinned for primary-ok / primary-fail-
fallback-ok / both-fail. Must pass unchanged before AND after the
deps.win_pe.run_with_fallback extraction (the windows leg's markerdiff
contract: verify_artifact.py:390-396)."""
from __future__ import annotations

import io
import os
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from .. import run as run_module
from .. import verify_artifact


def _fake(primary_rc, fallback_rc):
    def fake(argv, cwd=None, env=None):
        tool = str(argv[0])
        if tool == "dumpbin":
            return run_module.RunResult(argv=list(argv), returncode=primary_rc, stdout="P\n", stderr="")
        if tool in ("llvm-objdump", "llvm-nm"):
            return run_module.RunResult(argv=list(argv), returncode=fallback_rc, stdout="F\n", stderr="")
        return run_module.RunResult(argv=list(argv), returncode=0, stdout="", stderr="")
    return fake


class PeFallbackSurfaceTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        old_cwd = os.getcwd()
        os.chdir(tmp.name)
        self.addCleanup(os.chdir, old_cwd)

    def _dump(self, primary_rc, fallback_rc, prefix=""):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(run_module, "run", side_effect=_fake(primary_rc, fallback_rc)), \
                redirect_stdout(out), redirect_stderr(err):
            rc = verify_artifact._pe_dump_imports(
                "x.dll", "d/x.txt", primary_marker="PRIMARY_RC", fallback_marker="FALLBACK_RC",
                subject=" of x.dll", prefix=prefix)
        return rc, out.getvalue().splitlines(), Path("d/x.txt").read_text(encoding="utf-8")

    def _exports(self, primary_rc, fallback_rc):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(run_module, "run", side_effect=_fake(primary_rc, fallback_rc)), \
                mock.patch.object(verify_artifact, "_artifact_path", return_value="x.dll"), \
                mock.patch.object(verify_artifact._assert_exports_script, "run", return_value=0), \
                redirect_stdout(out), redirect_stderr(err):
            rc = verify_artifact.assert_exports("windows", "x86_64")
        return rc, out.getvalue().splitlines(), err.getvalue()

    def test_dump_primary_ok(self):
        self.assertEqual(self._dump(0, 0), (0, ["PRIMARY_RC=0"], "P\n"))

    def test_dump_fallback_ok(self):
        rc, lines, text = self._dump(1, 0)
        self.assertEqual(rc, 0)
        self.assertEqual(lines, [
            "PRIMARY_RC=1",
            "::notice::dumpbin -dependents of x.dll failed (rc=1); falling back to llvm-objdump.",
            "FALLBACK_RC=0",
        ])
        self.assertEqual(text, "F\n")

    def test_dump_both_fail(self):
        rc, lines, text = self._dump(1, 2)
        self.assertEqual(rc, 2)
        self.assertEqual((lines[0], lines[-1]), ("PRIMARY_RC=1", "FALLBACK_RC=2"))
        self.assertEqual(text, "F\n")

    def test_dump_prefix_notice_is_plain_not_a_marker(self):
        _rc, lines, _text = self._dump(1, 0, prefix="  [walk] ")
        self.assertEqual(lines, [
            "PRIMARY_RC=1",
            "  [walk] notice: dumpbin -dependents of x.dll failed (rc=1); falling back to llvm-objdump.",
            "FALLBACK_RC=0",
        ])

    def test_exports_primary_ok_emits_one_marker(self):
        _rc, lines, _err = self._exports(0, 0)
        self.assertIn("DUMPBIN_RC=0", lines)
        self.assertFalse(any(line.startswith("LLVM_NM_RC=") for line in lines))

    def test_exports_fallback_ok_marker_sequence(self):
        _rc, lines, _err = self._exports(1, 0)
        i = lines.index("DUMPBIN_RC=1")
        self.assertEqual(lines[i:i + 3], [
            "DUMPBIN_RC=1",
            "::notice::dumpbin unavailable or failed (rc=1); falling back to llvm-nm.",
            "LLVM_NM_RC=0",
        ])

    def test_exports_both_fail_is_red_with_dump_echo(self):
        rc, lines, err = self._exports(1, 2)
        self.assertEqual(rc, 1)
        i = lines.index("DUMPBIN_RC=1")
        self.assertEqual(lines[i + 2], "LLVM_NM_RC=2")
        self.assertIn("could not read the export table of x.dll with either dumpbin or llvm-nm (rc=2)", err)
        self.assertEqual(Path("dll_exports.txt").read_text(), "F\n")


if __name__ == "__main__":
    unittest.main()
