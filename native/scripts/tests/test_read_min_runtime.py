"""Tests for read_min_runtime.py (WI-14 step 14.1, S-F1)."""
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import read_min_runtime as rmr  # noqa: E402


def _macho64_with_build_version(minos_major, minos_minor, minos_patch=0):
    minos = (minos_major << 16) | (minos_minor << 8) | minos_patch
    # LC_BUILD_VERSION: cmd, cmdsize, platform, minos, sdk, ntools
    lc = struct.pack("<IIIIII", rmr.LC_BUILD_VERSION, 24, 1, minos, 0, 0)
    ncmds = 1
    sizeofcmds = len(lc)
    header = struct.pack("<IiiIIIII", rmr.MH_MAGIC_64, 0x0100000C, 0, 2, ncmds, sizeofcmds, 0, 0)
    return header + lc


def _macho64_with_version_min_macosx(major, minor, patch=0):
    version = (major << 16) | (minor << 8) | patch
    lc = struct.pack("<IIII", rmr.LC_VERSION_MIN_MACOSX, 16, version, 0)
    ncmds = 1
    sizeofcmds = len(lc)
    header = struct.pack("<IiiIIIII", rmr.MH_MAGIC_64, 0x0100000C, 0, 2, ncmds, sizeofcmds, 0, 0)
    return header + lc


def _pe_with_subsystem_version(major, minor, plus=False):
    magic = rmr.PE32_PLUS_MAGIC if plus else rmr.PE32_MAGIC
    opt_header = bytearray(112)
    struct.pack_into("<H", opt_header, 0, magic)
    struct.pack_into("<H", opt_header, 48, major)
    struct.pack_into("<H", opt_header, 50, minor)
    file_header = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, len(opt_header), 0)
    pe_sig = b"PE\x00\x00"
    dos_stub = bytearray(64)
    struct.pack_into("<I", dos_stub, 0x3C, 64)
    return _build_pe(dos_stub, pe_sig, file_header, bytes(opt_header))


def _build_pe(dos_stub, pe_sig, file_header, opt_header):
    data = bytearray(dos_stub)
    data[0:2] = b"MZ"
    data += pe_sig + file_header + opt_header
    return bytes(data)


def test_macos_build_version_read(tmp_path):
    p = tmp_path / "libfoo.dylib"
    p.write_bytes(_macho64_with_build_version(15, 0))
    value, breakdown = rmr.macos_min_runtime([p])
    assert value == "15.0"
    assert "LC_BUILD_VERSION" in breakdown[0]


def test_macos_version_min_macosx_fallback(tmp_path):
    p = tmp_path / "libold.dylib"
    p.write_bytes(_macho64_with_version_min_macosx(10, 13))
    value, breakdown = rmr.macos_min_runtime([p])
    assert value == "10.13"
    assert "LC_VERSION_MIN_MACOSX" in breakdown[0]


def test_macos_group_maximum(tmp_path):
    p1 = tmp_path / "decoder.dylib"
    p1.write_bytes(_macho64_with_build_version(12, 0))
    p2 = tmp_path / "libomp.dylib"
    p2.write_bytes(_macho64_with_build_version(15, 0))
    value, breakdown = rmr.macos_min_runtime([p1, p2])
    assert value == "15.0"
    assert len(breakdown) == 2


def test_macos_malformed_file_raises(tmp_path):
    p = tmp_path / "garbage.dylib"
    p.write_bytes(b"not a mach-o file at all")
    try:
        rmr.macos_min_runtime([p])
        assert False, "expected ValueError"
    except ValueError:
        pass


def test_windows_pe32_subsystem_version(tmp_path):
    p = tmp_path / "decoder.dll"
    p.write_bytes(_pe_with_subsystem_version(10, 0, plus=False))
    value, provenance = rmr.windows_min_runtime(p)
    assert value == "10.0"
    assert provenance == "measured"


def test_windows_pe32_plus_subsystem_version(tmp_path):
    p = tmp_path / "decoder.dll"
    p.write_bytes(_pe_with_subsystem_version(10, 0, plus=True))
    value, provenance = rmr.windows_min_runtime(p)
    assert value == "10.0"


def test_windows_linker_default_flagged(tmp_path):
    p = tmp_path / "decoder.dll"
    p.write_bytes(_pe_with_subsystem_version(6, 0, plus=True))
    value, provenance = rmr.windows_min_runtime(p)
    assert value == "6.0"
    assert provenance == "linker-default"


def test_windows_malformed_file_raises(tmp_path):
    p = tmp_path / "notpe.dll"
    p.write_bytes(b"nope")
    try:
        rmr.windows_min_runtime(p)
        assert False, "expected ValueError"
    except ValueError:
        pass


LINUX_DUMP = """
Symbol table '.dynsym' contains 42 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
     3: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND memcpy@GLIBC_2.38 (5)
     4: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND __cxa_throw@CXXABI_1.3.13 (6)
     5: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND _ZSt9terminatev@GLIBCXX_3.4.32 (7)
     6: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND malloc@GLIBC_2.35 (5)
"""


def test_linux_dump_max_glibc(tmp_path):
    p = tmp_path / "dynsyms.txt"
    p.write_text(LINUX_DUMP)
    value, breakdown = rmr.linux_min_runtime(p)
    assert value == "2.38"
    assert any("GLIBCXX_3.4.32" in b for b in breakdown)
    assert any("CXXABI_1.3.13" in b for b in breakdown)


def test_linux_empty_dump_raises(tmp_path):
    p = tmp_path / "dynsyms.txt"
    p.write_text("nothing interesting here\n")
    try:
        rmr.linux_min_runtime(p)
        assert False, "expected ValueError"
    except ValueError:
        pass


def test_android_minsdk_read(tmp_path):
    p = tmp_path / "build.gradle"
    p.write_text("""
android {
    defaultConfig {
        minSdk 21
        targetSdk 34
    }
}
""")
    value = rmr.android_min_runtime(p)
    assert value == "21"


def test_android_minsdkversion_variant(tmp_path):
    p = tmp_path / "build.gradle"
    p.write_text("minSdkVersion 24\n")
    value = rmr.android_min_runtime(p)
    assert value == "24"


def test_android_missing_declaration_raises(tmp_path):
    p = tmp_path / "build.gradle"
    p.write_text("android {}\n")
    try:
        rmr.android_min_runtime(p)
        assert False, "expected ValueError"
    except ValueError:
        pass


def test_cli_writes_out_file(tmp_path, monkeypatch, capsys):
    p = tmp_path / "libfoo.dylib"
    p.write_bytes(_macho64_with_build_version(15, 0))
    out = tmp_path / "min_runtime.txt"
    monkeypatch.setattr(sys, "argv", [
        "read_min_runtime.py", "--artifact", str(p), "--platform", "macos", "--out", str(out),
    ])
    rc = rmr.main()
    assert rc == 0
    text = out.read_text()
    assert "MIN_RUNTIME_macos=15.0" in text
    stdout = capsys.readouterr().out
    assert "READ_MIN_RUNTIME_RC=0" in stdout


def test_cli_malformed_file_nonzero_exit_no_empty_value(tmp_path, monkeypatch, capsys):
    p = tmp_path / "garbage.dylib"
    p.write_bytes(b"not a mach-o file")
    out = tmp_path / "min_runtime.txt"
    monkeypatch.setattr(sys, "argv", [
        "read_min_runtime.py", "--artifact", str(p), "--platform", "macos", "--out", str(out),
    ])
    rc = rmr.main()
    assert rc != 0
    assert not out.exists()
    err = capsys.readouterr().err
    assert "READ_MIN_RUNTIME_RC=1" in err
