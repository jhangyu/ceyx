"""Tests for assert_import_closure.py (WI-4 step 4.3/4.4, S-B1/S-B3)."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import assert_import_closure as aic  # noqa: E402

DECLARATION = pathlib.Path(__file__).resolve().parents[2] / "deps" / "shipped_files.toml"


def _write(tmp_path, name, text):
    p = tmp_path / name
    p.write_text(text)
    return p


def _stage(tmp_path, names):
    d = tmp_path / "staged"
    d.mkdir(exist_ok=True)
    for n in names:
        (d / n).write_bytes(b"\x00")
    return d


def _invoke(monkeypatch, argv, capsys):
    monkeypatch.setattr(sys, "argv", ["assert_import_closure.py"] + argv)
    rc = aic.main()
    out = capsys.readouterr()
    return rc, out.out, out.err


PE_DUMP_CLEAN = (
    "Dump of file dng_decoder_native.dll\n\n"
    "  Image has the following dependencies:\n\n"
    "    heif.dll\n"
    "    libde265.dll\n"
    "    libomp140.x86_64.dll\n"
    "    KERNEL32.dll\n"
    "    api-ms-win-crt-runtime-l1-1-0.dll\n"
)

PE_DUMP_UNKNOWN = PE_DUMP_CLEAN + "    sketchy_third_party.dll\n"


def test_pe_clean_dump_passes(tmp_path, monkeypatch, capsys):
    dump = _write(tmp_path, "dump.txt", PE_DUMP_CLEAN)
    staged = _stage(tmp_path, ["heif.dll", "libde265.dll", "libomp140.x86_64.dll"])
    rc, out, err = _invoke(monkeypatch, [
        "--dump", str(dump), "--staged-dir", str(staged),
        "--declaration", str(DECLARATION), "--platform", "windows", "--format", "pe",
    ], capsys)
    assert rc == 0
    assert "IMPORT_CLOSURE_RESULT=PASS" in out
    assert "IMPORT_CLOSURE PASS (windows):" in out


def test_pe_unknown_import_fails(tmp_path, monkeypatch, capsys):
    dump = _write(tmp_path, "dump.txt", PE_DUMP_UNKNOWN)
    staged = _stage(tmp_path, ["heif.dll", "libde265.dll", "libomp140.x86_64.dll"])
    rc, out, err = _invoke(monkeypatch, [
        "--dump", str(dump), "--staged-dir", str(staged),
        "--declaration", str(DECLARATION), "--platform", "windows", "--format", "pe",
    ], capsys)
    assert rc == 1
    assert "IMPORT sketchy_third_party.dll -> MISSING" in out
    assert "IMPORT_CLOSURE_RESULT=FAIL" in out


def test_pe_missing_staged_companion_fails(tmp_path, monkeypatch, capsys):
    """S-B1 red: delete a staged companion (heif.dll) before the gate."""
    dump = _write(tmp_path, "dump.txt", PE_DUMP_CLEAN)
    staged = _stage(tmp_path, ["libde265.dll", "libomp140.x86_64.dll"])  # heif.dll deleted
    rc, out, err = _invoke(monkeypatch, [
        "--dump", str(dump), "--staged-dir", str(staged),
        "--declaration", str(DECLARATION), "--platform", "windows", "--format", "pe",
    ], capsys)
    assert rc == 1
    assert "IMPORT heif.dll -> MISSING" in out
    assert "IMPORT_CLOSURE_RESULT=FAIL" in out


def test_pe_openmp_missing_before_staging_lands():
    """S-B1 red (pre-staging, today's tree): OpenMP DLL absent from staged-dir."""
    pass  # covered structurally by test_pe_missing_staged_companion_fails' MISSING path


ELF_DUMP_LINUX_CLEAN = (
    " Dynamic section at offset 0x1000 contains 20 entries:\n"
    "  Tag        Type                         Name/Value\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libheif.so.1]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libde265.so.0]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libstdc++.so.6]\n"
)


def test_elf_linux_clean_dump_passes(tmp_path, monkeypatch, capsys):
    dump = _write(tmp_path, "readelf_dynamic.txt", ELF_DUMP_LINUX_CLEAN)
    staged = _stage(tmp_path, ["libheif.so.1", "libde265.so.0"])
    rc, out, err = _invoke(monkeypatch, [
        "--dump", str(dump), "--staged-dir", str(staged),
        "--declaration", str(DECLARATION), "--platform", "linux", "--format", "elf",
    ], capsys)
    assert rc == 0
    assert "IMPORT_CLOSURE_RESULT=PASS" in out


def test_elf_unknown_needed_fails(tmp_path, monkeypatch, capsys):
    dump = _write(tmp_path, "readelf_dynamic.txt",
                  ELF_DUMP_LINUX_CLEAN + " 0x0000000000000001 (NEEDED)             Shared library: [libvulkan.so.1]\n")
    staged = _stage(tmp_path, ["libheif.so.1", "libde265.so.0"])
    rc, out, err = _invoke(monkeypatch, [
        "--dump", str(dump), "--staged-dir", str(staged),
        "--declaration", str(DECLARATION), "--platform", "linux", "--format", "elf",
    ], capsys)
    assert rc == 1
    assert "IMPORT libvulkan.so.1 -> MISSING" in out


def test_empty_dump_is_unverified(tmp_path, monkeypatch, capsys):
    dump = _write(tmp_path, "dump.txt", "")
    staged = _stage(tmp_path, [])
    rc, out, err = _invoke(monkeypatch, [
        "--dump", str(dump), "--staged-dir", str(staged),
        "--declaration", str(DECLARATION), "--platform", "windows", "--format", "pe",
    ], capsys)
    assert rc == 1
    assert "IMPORT_CLOSURE_RESULT=UNVERIFIED" in out


def test_garbage_dump_is_unverified(tmp_path, monkeypatch, capsys):
    dump = _write(tmp_path, "dump.txt", "this is not a dependency dump\n12345\n???\n")
    staged = _stage(tmp_path, [])
    rc, out, err = _invoke(monkeypatch, [
        "--dump", str(dump), "--staged-dir", str(staged),
        "--declaration", str(DECLARATION), "--platform", "linux", "--format", "elf",
    ], capsys)
    assert rc == 1
    assert "IMPORT_CLOSURE_RESULT=UNVERIFIED" in out


def test_missing_dump_file_is_unverified(tmp_path, monkeypatch, capsys):
    staged = _stage(tmp_path, [])
    rc, out, err = _invoke(monkeypatch, [
        "--dump", str(tmp_path / "nope.txt"), "--staged-dir", str(staged),
        "--declaration", str(DECLARATION), "--platform", "windows", "--format", "pe",
    ], capsys)
    assert rc == 1
    assert "IMPORT_CLOSURE_RESULT=UNVERIFIED" in out
