"""Unit tests for native/scripts/check_jit_inert.py classify_body().

Stubs subprocess.run (not _run) so this file runs unchanged against the
code before and after the failed-tool fix. Manual only:
python3 -m pytest native/scripts/tests/test_check_jit_inert.py -q
"""
import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
CHECK_PATH = REPO_ROOT / "native" / "scripts" / "check_jit_inert.py"

spec = importlib.util.spec_from_file_location("check_jit_inert", CHECK_PATH)
cji = importlib.util.module_from_spec(spec)
sys.modules["check_jit_inert"] = cji
spec.loader.exec_module(cji)

SYM = "_halide_use_jit_module"
NM_OK = ("0000000000001000 T %s\n" % SYM).encode()
PARTIAL_DISASM = (
    "%s:\n"
    "0000000000001000\tstp\tx29, x30, [sp, #-0x10]!\n" % SYM
).encode()  # body cut off before `ret` -- what a dying otool can leave behind
INERT_DISASM = (
    "%s:\n"
    "0000000000001000\tret\n" % SYM
).encode()
NON_INERT_DISASM = (
    "%s:\n"
    "0000000000001000\tadrp\tx8, 0x2000\n"
    "0000000000001004\tldr\tx9, [x8]\n"
    "0000000000001008\tbl\t_something\n"
    "000000000000100c\tret\n" % SYM
).encode()


class _Proc:
    def __init__(self, stdout, returncode, stderr=b""):
        self.stdout = stdout
        self.returncode = returncode
        self.stderr = stderr


def _stub(monkeypatch, nm, otool):
    def fake_run(cmd, **_kwargs):
        if cmd[0] == "nm":
            return nm
        if cmd[0] == "otool":
            return otool
        raise AssertionError("unexpected command %r" % (cmd,))
    monkeypatch.setattr(cji.subprocess, "run", fake_run)


def test_otool_failure_is_absent_not_inert(monkeypatch):
    _stub(monkeypatch, _Proc(NM_OK, 0), _Proc(PARTIAL_DISASM, 1, b"otool: truncated"))
    state, detail = cji.classify_body("lib.dylib", SYM)
    assert state == "ABSENT", (state, detail)
    assert "otool_failed rc=1" in detail


def test_nm_failure_with_output_is_absent(monkeypatch):
    _stub(monkeypatch, _Proc(NM_OK, 1, b"nm: partial read"), _Proc(INERT_DISASM, 0))
    state, detail = cji.classify_body("lib.dylib", SYM)
    assert state == "ABSENT", (state, detail)
    assert "nm_rc=1" in detail


def test_healthy_tools_keep_inert(monkeypatch):
    _stub(monkeypatch, _Proc(NM_OK, 0), _Proc(INERT_DISASM, 0))
    assert cji.classify_body("lib.dylib", SYM)[0] == "INERT"


def test_healthy_nontrivial_is_non_inert(monkeypatch):
    _stub(monkeypatch, _Proc(NM_OK, 0), _Proc(NON_INERT_DISASM, 0))
    assert cji.classify_body("lib.dylib", SYM)[0] == "NON_INERT"
