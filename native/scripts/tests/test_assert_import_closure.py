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


# TRANSCRIBED VERBATIM from `readelf -d` on the RELEASED v0.1.23 linux asset
# (downloaded by tag, archive + member sha256 verified against
# scripts/ceyx_release_pin.json before reading; tmp/verify/linux/shipped_dtneeded.txt).
# The locally rebuilt .so produces the identical set, which is what establishes
# that libjpeg/libz/libgomp are the status quo rather than a new regression.
ELF_DUMP_LINUX_SHIPPED = (
    " Dynamic section at offset 0x1000 contains 30 entries:\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libjpeg.so.8]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libheif.so.1]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libm.so.6]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libz.so.1]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libgomp.so.1]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libstdc++.so.6]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libgcc_s.so.1]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [ld-linux-x86-64.so.2]\n"
)


def test_elf_linux_shipped_v0123_needed_set_passes(tmp_path, monkeypatch, capsys):
    """Regression for the WI-4 gate's third unmeasured platform: the real
    shipped Linux decoder imports libjpeg.so.8 / libz.so.1 / libgomp.so.1."""
    dump = _write(tmp_path, "readelf_dynamic.txt", ELF_DUMP_LINUX_SHIPPED)
    staged = _stage(tmp_path, ["libheif.so.1"])
    rc, out, err = _invoke(monkeypatch, [
        "--dump", str(dump), "--staged-dir", str(staged),
        "--declaration", str(DECLARATION), "--platform", "linux", "--format", "elf",
    ], capsys)
    assert rc == 0
    for name in ("libjpeg.so.8", "libz.so.1", "libgomp.so.1"):
        assert f"IMPORT {name} -> OS_ALLOWLIST" in out
    assert "IMPORT libheif.so.1 -> STAGED" in out
    assert "IMPORT_CLOSURE_RESULT=PASS" in out


def test_linux_additions_did_not_leak_to_other_platforms():
    """The three additions are Linux ELF sonames; they must not appear in the
    android or windows allowlists (android has its own libz.so spelling)."""
    for name in ("libjpeg.so.8", "libz.so.1", "libgomp.so.1"):
        assert name in aic.LINUX_OS_ALLOWLIST
        assert name not in aic.ANDROID_OS_ALLOWLIST
        assert name not in aic.WINDOWS_OS_ALLOWLIST


# TRANSCRIBED VERBATIM from the android/arm64-v8a leg of CI run 34697591379
# (tmp/verify/ci-fail-20260912-220022.log:3444-3451) -- the decoder's REAL
# DT_NEEDED set, in the order the gate printed it. libvulkan.so was the entry
# the allowlist was missing; it is hard-linked by native/cmake/ffi.cmake:104-110.
ELF_DUMP_ANDROID_CLEAN = (
    " Dynamic section at offset 0x1000 contains 20 entries:\n"
    "  Tag        Type                         Name/Value\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libvulkan.so]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [liblog.so]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libheif.so]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libde265.so]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libm.so]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libz.so]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libdl.so]\n"
    " 0x0000000000000001 (NEEDED)             Shared library: [libc.so]\n"
)


def test_elf_android_real_needed_set_passes(tmp_path, monkeypatch, capsys):
    """Regression for CI run 34697591379: the real Android decoder imports
    the NDK platform library libvulkan.so, which the allowlist omitted."""
    dump = _write(tmp_path, "readelf_dynamic.txt", ELF_DUMP_ANDROID_CLEAN)
    staged = _stage(tmp_path, ["libheif.so", "libde265.so"])
    rc, out, err = _invoke(monkeypatch, [
        "--dump", str(dump), "--staged-dir", str(staged),
        "--declaration", str(DECLARATION), "--platform", "android", "--format", "elf",
    ], capsys)
    assert rc == 0
    assert "IMPORT libvulkan.so -> OS_ALLOWLIST" in out
    assert "IMPORT_CLOSURE_RESULT=PASS" in out


def test_elf_android_unknown_import_still_fails(tmp_path, monkeypatch, capsys):
    """The libvulkan.so addition is a single measured NAME, not a relaxation:
    an unknown third-party .so on android must still fail the gate."""
    dump = _write(tmp_path, "readelf_dynamic.txt", ELF_DUMP_ANDROID_CLEAN +
                  " 0x0000000000000001 (NEEDED)             Shared library: [libsketchy.so]\n")
    staged = _stage(tmp_path, ["libheif.so", "libde265.so"])
    rc, out, err = _invoke(monkeypatch, [
        "--dump", str(dump), "--staged-dir", str(staged),
        "--declaration", str(DECLARATION), "--platform", "android", "--format", "elf",
    ], capsys)
    assert rc == 1
    assert "IMPORT libsketchy.so -> MISSING" in out
    assert "IMPORT_CLOSURE_RESULT=FAIL" in out


def test_libvulkan_allowlisted_for_android_only(tmp_path, monkeypatch, capsys):
    """Scope guard: libvulkan.so is an Android platform library. The Linux
    allowlist must NOT have gained it (linux links the loader differently --
    test_elf_unknown_needed_fails above covers libvulkan.so.1 there)."""
    assert "libvulkan.so" in aic.ANDROID_OS_ALLOWLIST
    assert "libvulkan.so" not in aic.LINUX_OS_ALLOWLIST
    assert "libvulkan.so" not in aic.WINDOWS_OS_ALLOWLIST


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


def test_pe_export_table_self_match_not_counted_as_import(tmp_path, monkeypatch, capsys):
    """U-10 real-fetch regression: objdump/llvm-objdump -p print the export
    table's "DLL name: <self>" line AFTER the import tables, using the same
    "DLL name:" shape the import parser matches -- caught against the real
    v0.1.23 fetched dng_decoder_native.dll, which self-matched as an
    unresolved import of itself before the Export Table stop was added."""
    dump = _write(tmp_path, "dump.txt", PE_DUMP_CLEAN + (
        "\nExport Table:\n"
        " DLL name: dng_decoder_native.dll\n"
        " Ordinal base: 1\n"
        "       1   0x1e40  ceyx_decode_into_buffer\n"
    ))
    staged = _stage(tmp_path, ["heif.dll", "libde265.dll", "libomp140.x86_64.dll"])
    rc, out, err = _invoke(monkeypatch, [
        "--dump", str(dump), "--staged-dir", str(staged),
        "--declaration", str(DECLARATION), "--platform", "windows", "--format", "pe",
    ], capsys)
    assert rc == 0
    assert "dng_decoder_native.dll" not in out
    assert "IMPORT_CLOSURE_RESULT=PASS" in out


def test_missing_dump_file_is_unverified(tmp_path, monkeypatch, capsys):
    staged = _stage(tmp_path, [])
    rc, out, err = _invoke(monkeypatch, [
        "--dump", str(tmp_path / "nope.txt"), "--staged-dir", str(staged),
        "--declaration", str(DECLARATION), "--platform", "windows", "--format", "pe",
    ], capsys)
    assert rc == 1
    assert "IMPORT_CLOSURE_RESULT=UNVERIFIED" in out
