"""Unix HEIF assembly tests -- Plan D3 round 4, the port of
``native/scripts/fetch_heif_deps.sh``.

The point of this file is that EVERY assertion the shell script made is
demonstrated RED here, against the real manifest where the data comes from
it. A ported check that silently never fires would be indistinguishable
from a working one on a green CI run -- which is exactly the class of
defect the original script's comments spend most of their length warning
about.

Mocking boundary: ``deps.heif.run`` and ``deps.execute.run`` only. No
global monkeypatching of ``os.environ`` or ``subprocess`` (forbidden route:
it produces a pytest INTERNALERROR in this suite); the one environment
variable this module reads, ``CEYX_VCPKG_PREFIX``, is set through pytest's
own ``monkeypatch.setenv`` fixture, which is scoped and undone per test.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path
from unittest import mock

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from deps import heif  # noqa: E402
from deps import manifest as manifest_mod  # noqa: E402

REAL = manifest_mod.load()


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _fake_runner(outputs: dict[str, str]):
    """Return a run() stand-in that maps argv[0] to canned stdout and records
    every call. Always returns an argv LIST in the CompletedProcess so a
    caller that stringified its command would be visible here."""
    calls: list[list[str]] = []

    def _run(argv, **kwargs):
        calls.append(list(argv))
        return subprocess.CompletedProcess(list(argv), 0, outputs.get(argv[0], ""), "")

    _run.calls = calls  # type: ignore[attr-defined]
    return _run


_GOOD_SYMBOLS = (
    "0000000000001000 T _heif_decode_image\n"
    "0000000000002000 T _heif_context_get_encoder_for_format\n"
    "0000000000003000 T _kvz_api_get\n"
    "0000000000004000 T _aom_codec_av1_cx\n"
    "0000000000005000 T _aom_codec_av1_dx\n"
)
_GOOD_DEPS = "\t@rpath/libde265.0.dylib (compatibility version 1.0.0)\n"


# ---------------------------------------------------------------------------
# Stamp: byte-compatible with the shell script's WANT_PINS
# ---------------------------------------------------------------------------
def test_want_pins_matches_the_shell_scripts_format_and_reads_the_manifest() -> None:
    pins = heif.want_pins(REAL, "arm64")
    heif_sha = REAL["manifest"]["component"]["libheif"]["source"]["default"]["sha256"]
    kvz_sha = REAL["manifest"]["component"]["kvazaar"]["source"]["default"]["sha256"]
    assert pins == (
        f"libheif=1.23.2:{heif_sha} "
        f"libde265=1.1.1:vcpkg "
        f"kvazaar=2.3.1:{kvz_sha} "
        f"aom=3.15.0:vcpkg "
        f"arch=arm64"
    )


def test_stamp_carries_the_arch_so_a_cross_dist_is_not_mistaken_for_current() -> None:
    assert heif.want_pins(REAL, "arm64") != heif.want_pins(REAL, "x86_64")


def test_stamp_carries_kvazaar_and_aom_so_a_decode_only_dist_cannot_match() -> None:
    """The highest-probability silent failure the shell script names: a
    pre-expansion dist matching on libheif/libde265 alone."""
    pins = heif.want_pins(REAL, "arm64")
    assert "kvazaar=" in pins and "aom=" in pins


def test_stamp_is_not_current_when_pins_match_but_the_dylib_is_gone(tmp_path: Path) -> None:
    (tmp_path / ".pins").write_text(heif.want_pins(REAL, "arm64"), encoding="utf-8")
    assert heif.stamp_is_current(tmp_path, REAL, "arm64", "macos") is False


def test_stamp_is_current_only_when_both_pins_and_artefacts_are_present(tmp_path: Path) -> None:
    (tmp_path / ".pins").write_text(heif.want_pins(REAL, "arm64"), encoding="utf-8")
    (tmp_path / "lib").mkdir()
    (tmp_path / "lib" / "libheif.1.dylib").write_bytes(b"")
    (tmp_path / "lib" / "libde265.0.dylib").write_bytes(b"")
    assert heif.stamp_is_current(tmp_path, REAL, "arm64", "macos") is True


def test_stale_pins_are_not_current(tmp_path: Path) -> None:
    (tmp_path / ".pins").write_text("libheif=1.0.0:old arch=arm64", encoding="utf-8")
    (tmp_path / "lib").mkdir()
    (tmp_path / "lib" / "libheif.1.dylib").write_bytes(b"")
    (tmp_path / "lib" / "libde265.0.dylib").write_bytes(b"")
    assert heif.stamp_is_current(tmp_path, REAL, "arm64", "macos") is False


# ---------------------------------------------------------------------------
# Capability assertions -- each demonstrated RED
# ---------------------------------------------------------------------------
def test_all_capability_assertions_pass_on_a_good_symbol_table() -> None:
    heif.check_symbols(_GOOD_SYMBOLS, _GOOD_DEPS)  # must not raise


@pytest.mark.parametrize(
    "missing",
    [
        "heif_decode_image",
        "heif_context_get_encoder_for_format",
        "kvz_api_get",
        "aom_codec_av1_cx",
        "aom_codec_av1_dx",
    ],
)
def test_each_required_symbol_is_independently_red(missing: str) -> None:
    """aom_codec_av1_cx and _dx are checked separately because
    WITH_AOM_ENCODER and WITH_AOM_DECODER are INDEPENDENT flags -- one can
    silently be off while the other is on."""
    symbols = "".join(line for line in _GOOD_SYMBOLS.splitlines(True) if missing not in line)
    with pytest.raises(heif.HeifError) as exc:
        heif.check_symbols(symbols, _GOOD_DEPS)
    assert "FAILED" in str(exc.value)


def test_x265_symbols_are_red_gpl_contamination() -> None:
    with pytest.raises(heif.HeifError) as exc:
        heif.check_symbols(_GOOD_SYMBOLS + "0000 T _x265_encoder_open\n", _GOOD_DEPS)
    assert "GPL-2.0" in str(exc.value)


def test_missing_libde265_dependency_is_red() -> None:
    with pytest.raises(heif.HeifError) as exc:
        heif.check_symbols(_GOOD_SYMBOLS, "\t/usr/lib/libSystem.B.dylib\n")
    assert "HEVC" in str(exc.value)


def test_each_passing_assertion_prints_its_own_evidence_line(capsys) -> None:
    """A silently-passing gate and a gate that never ran produce identical
    logs (lesson 2026-08-25). The CI verdict for this step is read at the
    level of individual assertion lines, so each one must announce itself on
    SUCCESS too -- not only shout on failure."""
    heif.check_symbols(_GOOD_SYMBOLS, _GOOD_DEPS)
    out = capsys.readouterr().out
    for symbol in (
        "heif_decode_image",
        "heif_context_get_encoder_for_format",
        "kvz_api_get",
        "aom_codec_av1_cx",
        "aom_codec_av1_dx",
    ):
        assert f"ASSERT ok      present in libheif: {symbol}" in out
    assert "ASSERT absent  correctly not in libheif: x265_encoder" in out
    assert "ASSERT ok      libheif records a libde265 runtime dependency" in out


def test_arch_assertion_prints_evidence_on_success(capsys) -> None:
    with mock.patch.object(heif, "run", _fake_runner({"lipo": "arm64\n"})):
        heif.assert_arch("macos", "arm64", [Path("/dist/lib/libheif.1.dylib")])
    assert "ASSERT ok      libheif.1.dylib archs 'arm64' include arm64" in capsys.readouterr().out

    with mock.patch.object(heif, "run", _fake_runner({"file": "x: ELF 64-bit LSB\n"})):
        heif.assert_arch("linux", "x86_64", [Path("/dist/lib/libheif.so.1")])
    assert "ASSERT ok      libheif.so.1 is a 64-bit ELF" in capsys.readouterr().out


def test_symbol_capture_is_an_argv_list_never_a_pipeline() -> None:
    runner = _fake_runner({"nm": _GOOD_SYMBOLS, "otool": _GOOD_DEPS})
    with mock.patch.object(heif, "run", runner):
        heif.assert_libheif_capabilities("macos", Path("/dist/lib/libheif.1.dylib"))
    for argv in runner.calls:  # type: ignore[attr-defined]
        assert isinstance(argv, list)
        assert not any("|" in element or "grep" in element for element in argv)
    assert runner.calls[0][0] == "nm"  # type: ignore[attr-defined]
    assert runner.calls[0][1] == "-gU"  # type: ignore[attr-defined]


def test_linux_uses_nm_D_and_ldd() -> None:
    runner = _fake_runner({"nm": _GOOD_SYMBOLS, "ldd": "libde265.so.0 => /dist/lib\n"})
    with mock.patch.object(heif, "run", runner):
        heif.assert_libheif_capabilities("linux", Path("/dist/lib/libheif.so.1"))
    assert runner.calls[0][:2] == ["nm", "-D"]  # type: ignore[attr-defined]
    assert runner.calls[1][0] == "ldd"  # type: ignore[attr-defined]


# ---------------------------------------------------------------------------
# Arch assertions
# ---------------------------------------------------------------------------
def test_macos_arch_assertion_is_red_on_the_wrong_architecture() -> None:
    with mock.patch.object(heif, "run", _fake_runner({"lipo": "x86_64\n"})):
        with pytest.raises(heif.HeifError) as exc:
            heif.assert_arch("macos", "arm64", [Path("/dist/lib/libheif.1.dylib")])
    assert "arm64" in str(exc.value)


def test_macos_arch_assertion_is_green_on_the_right_architecture() -> None:
    with mock.patch.object(heif, "run", _fake_runner({"lipo": "arm64\n"})):
        heif.assert_arch("macos", "arm64", [Path("/dist/lib/libheif.1.dylib")])


def test_macos_arch_match_is_whole_word_not_substring() -> None:
    """`grep -w` in the original: 'arm64e' must not satisfy a request for
    'arm64'."""
    with mock.patch.object(heif, "run", _fake_runner({"lipo": "arm64e\n"})):
        with pytest.raises(heif.HeifError):
            heif.assert_arch("macos", "arm64", [Path("/dist/lib/libheif.1.dylib")])


def test_linux_arch_assertion_requires_a_64_bit_elf() -> None:
    with mock.patch.object(heif, "run", _fake_runner({"file": "x.so: ELF 32-bit LSB\n"})):
        with pytest.raises(heif.HeifError) as exc:
            heif.assert_arch("linux", "x86_64", [Path("/dist/lib/libheif.so.1")])
    assert "64-bit ELF" in str(exc.value)

    with mock.patch.object(heif, "run", _fake_runner({"file": "x.so: ELF 64-bit LSB shared object\n"})):
        heif.assert_arch("linux", "x86_64", [Path("/dist/lib/libheif.so.1")])


# ---------------------------------------------------------------------------
# vcpkg copy-out: libde265
# ---------------------------------------------------------------------------
def _seed_vcpkg_prefix(tmp_path: Path, *, de265_name: str = "libde265.0.dylib") -> Path:
    prefix = tmp_path / "vcpkg-installed" / "arm64-osx-heif"
    (prefix / "lib").mkdir(parents=True)
    (prefix / "include" / "libde265").mkdir(parents=True)
    (prefix / "include" / "aom").mkdir(parents=True)
    (prefix / "share" / "aom").mkdir(parents=True)
    (prefix / "lib" / de265_name).write_bytes(b"MACHO")
    (prefix / "lib" / "libaom.a").write_bytes(b"!<arch>")
    (prefix / "include" / "libde265" / "de265.h").write_text("/* de265 */")
    (prefix / "include" / "aom" / "aom_encoder.h").write_text("/* enc */")
    (prefix / "include" / "aom" / "aom_decoder.h").write_text("/* dec */")
    (prefix / "share" / "aom" / "copyright").write_text("BSD-2 + PATENTS")
    return prefix


def test_libde265_hard_fails_when_the_vcpkg_prefix_is_unset(tmp_path, monkeypatch) -> None:
    """There is deliberately no fallback to a source build: a silent
    fallback would hide exactly the wiring this migration exists to prove."""
    monkeypatch.delenv("CEYX_VCPKG_PREFIX", raising=False)
    with pytest.raises(heif.HeifError) as exc:
        heif.build_libde265(REAL, "macos", tmp_path / "dist")
    assert "CEYX_VCPKG_PREFIX" in str(exc.value)


def test_libde265_hard_fails_when_no_shared_library_is_in_the_prefix(tmp_path, monkeypatch) -> None:
    """A STATIC libde265 here is an LGPL-3 4(d)(1) breach, not a packaging
    detail."""
    prefix = _seed_vcpkg_prefix(tmp_path)
    (prefix / "lib" / "libde265.0.dylib").unlink()
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(prefix))
    with pytest.raises(heif.HeifError) as exc:
        heif.build_libde265(REAL, "macos", tmp_path / "dist")
    assert "LGPL-3 4(d)(1)" in str(exc.value)


def test_libde265_copy_renames_rewrites_and_re_reads_the_install_name(tmp_path, monkeypatch) -> None:
    """Round-3 failure trace: a verbatim copy of vcpkg's
    libde265.0.2.1.dylib records an install name that does not match the
    staged libde265.0.dylib spelling, and the runtime load failure is
    invisible on build machines. Rename + rewrite + RE-READ, in that order."""
    prefix = _seed_vcpkg_prefix(tmp_path, de265_name="libde265.dylib")
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(prefix))
    dist = tmp_path / "dist"
    runner = _fake_runner({"otool": "/path/to/lib:\n@rpath/libde265.0.dylib\n"})
    with mock.patch.object(heif, "run", runner):
        heif.build_libde265(REAL, "macos", dist)

    # Renamed to the versioned spelling heif.cmake stages, with the
    # unversioned name as the symlink libheif is pointed at.
    assert (dist / "lib" / "libde265.0.dylib").is_file()
    assert (dist / "lib" / "libde265.dylib").is_symlink()
    assert (dist / "include" / "libde265" / "de265.h").is_file()

    argvs = runner.calls  # type: ignore[attr-defined]
    assert argvs[0][:3] == ["install_name_tool", "-id", "@rpath/libde265.0.dylib"]
    # Re-read AFTER the rewrite: setting a value and the artefact carrying
    # it are different facts.
    assert argvs[1][:2] == ["otool", "-D"]


def test_libde265_is_red_when_the_rewritten_install_name_does_not_stick(tmp_path, monkeypatch) -> None:
    prefix = _seed_vcpkg_prefix(tmp_path)
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(prefix))
    bad = "/path:\n/Users/runner/temp/vcpkg-installed/arm64-osx-heif/lib/libde265.0.2.1.dylib\n"
    with mock.patch.object(heif, "run", _fake_runner({"otool": bad})):
        with pytest.raises(heif.HeifError) as exc:
            heif.build_libde265(REAL, "macos", tmp_path / "dist")
    assert "@rpath/libde265.0.dylib" in str(exc.value)


def test_linux_soname_assertion_is_red_on_the_wrong_soname(tmp_path, monkeypatch) -> None:
    prefix = tmp_path / "prefix"
    (prefix / "lib").mkdir(parents=True)
    (prefix / "include" / "libde265").mkdir(parents=True)
    (prefix / "lib" / "libde265.so.0").write_bytes(b"\x7fELF")
    (prefix / "include" / "libde265" / "de265.h").write_text("/* h */")
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(prefix))
    with mock.patch.object(heif, "run", _fake_runner({"readelf": " 0x000e (SONAME) Library soname: [libde265.so.9]\n"})), \
            mock.patch.object(heif.shutil, "which", return_value="/usr/bin/readelf"):
        with pytest.raises(heif.HeifError) as exc:
            heif.build_libde265(REAL, "linux", tmp_path / "dist")
    assert "SONAME" in str(exc.value)


def test_linux_missing_readelf_is_a_failure_not_a_skip(tmp_path, monkeypatch) -> None:
    """A check that passes when its instrument is absent is worse than no
    check."""
    prefix = tmp_path / "prefix"
    (prefix / "lib").mkdir(parents=True)
    (prefix / "include" / "libde265").mkdir(parents=True)
    (prefix / "lib" / "libde265.so.0").write_bytes(b"\x7fELF")
    (prefix / "include" / "libde265" / "de265.h").write_text("/* h */")
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(prefix))
    with mock.patch.object(heif.shutil, "which", return_value=None):
        with pytest.raises(heif.HeifError) as exc:
            heif.build_libde265(REAL, "linux", tmp_path / "dist")
    assert "readelf" in str(exc.value)


# ---------------------------------------------------------------------------
# vcpkg copy-out: aom
# ---------------------------------------------------------------------------
def test_aom_copy_installs_the_static_archive_and_both_headers(tmp_path, monkeypatch) -> None:
    prefix = _seed_vcpkg_prefix(tmp_path)
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(prefix))
    dist = tmp_path / "dist"
    heif.build_aom(REAL, dist)
    assert (dist / "lib" / "libaom.a").is_file()
    assert (dist / "include" / "aom" / "aom_encoder.h").is_file()
    assert (dist / "include" / "aom" / "aom_decoder.h").is_file()


def test_aom_is_red_when_the_static_archive_is_absent(tmp_path, monkeypatch) -> None:
    prefix = _seed_vcpkg_prefix(tmp_path)
    (prefix / "lib" / "libaom.a").unlink()
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(prefix))
    with pytest.raises(heif.HeifError) as exc:
        heif.build_aom(REAL, tmp_path / "dist")
    assert "STATIC archive" in str(exc.value)


def test_aom_is_red_when_only_one_of_the_two_headers_is_present(tmp_path, monkeypatch) -> None:
    """libheif's encoder and decoder plugins include them separately."""
    prefix = _seed_vcpkg_prefix(tmp_path)
    (prefix / "include" / "aom" / "aom_decoder.h").unlink()
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(prefix))
    with pytest.raises(heif.HeifError) as exc:
        heif.build_aom(REAL, tmp_path / "dist")
    assert "aom_decoder.h" in str(exc.value)


# ---------------------------------------------------------------------------
# Licence vendoring
# ---------------------------------------------------------------------------
def test_licence_glob_is_case_insensitive_and_depth_one(tmp_path: Path) -> None:
    src = tmp_path / "src"
    (src / "sub").mkdir(parents=True)
    (src / "COPYING").write_text("gpl-ish")
    (src / "license.md").write_text("bsd")
    (src / "PATENTS").write_text("aom patents")
    (src / "README.md").write_text("not a licence")
    (src / "sub" / "LICENSE").write_text("must not be picked up: depth 1 only")
    dest = tmp_path / "dest"
    dest.mkdir()
    copied = heif.copy_licence_files(src, dest, ["COPYING*", "LICENSE*", "PATENTS*"])
    names = sorted(p.name for p in copied)
    assert names == ["COPYING", "PATENTS", "license.md"]
    assert not (dest / "README.md").exists()


def test_aom_licence_is_red_when_the_vcpkg_copyright_is_absent(tmp_path, monkeypatch) -> None:
    """PATENTS matters specifically (K6): the Alliance for Open Media Patent
    License 1.0 is a SEPARATE grant on top of BSD-2."""
    prefix = _seed_vcpkg_prefix(tmp_path)
    (prefix / "share" / "aom" / "copyright").unlink()
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(prefix))
    with pytest.raises(heif.HeifError) as exc:
        heif.vendor_licences(REAL, tmp_path / "dist", tmp_path / "stage")
    assert "attribution duty" in str(exc.value)


def test_licence_vendoring_is_red_when_a_source_tree_has_no_licence_file(tmp_path, monkeypatch) -> None:
    prefix = _seed_vcpkg_prefix(tmp_path)
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(prefix))
    stage = tmp_path / "stage"
    # Present but empty of licence files -> must fail rather than ship
    # an empty share/licenses/<name>/ directory.
    (stage / "libheif-1.23.2").mkdir(parents=True)
    with pytest.raises(heif.HeifError) as exc:
        heif.vendor_licences(REAL, tmp_path / "dist", stage)
    assert "no licence file found for libheif" in str(exc.value)


def test_licence_vendoring_succeeds_and_writes_every_component(tmp_path, monkeypatch) -> None:
    prefix = _seed_vcpkg_prefix(tmp_path)
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(prefix))
    stage = tmp_path / "stage"
    dist = tmp_path / "dist"
    for name, version in (("libheif", "1.23.2"), ("libde265", "1.1.1"), ("kvazaar", "2.3.1")):
        d = stage / f"{name}-{version}"
        d.mkdir(parents=True)
        (d / "COPYING").write_text(f"{name} licence")
    heif.vendor_licences(REAL, dist, stage)
    for name in ("libheif", "libde265", "kvazaar"):
        assert (dist / "share" / "licenses" / name / "COPYING").is_file()
    assert (dist / "share" / "licenses" / "aom" / "copyright").read_text() == "BSD-2 + PATENTS"


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------
def test_windows_is_rejected_with_a_pointer_to_the_right_script() -> None:
    with pytest.raises(heif.HeifError) as exc:
        heif.build(REAL, "windows", "x86_64", Path("/dist"))
    # A-T4: the remediation pointer names the module that OWNS the Windows dist
    # today. It used to name build_heif_dist_windows.sh, which no longer exists
    # in the tree (that recipe moved into deps/win_heif_dist.py), so the old
    # assertion pinned a dead path -- a remediation message naming a file that
    # is not there is the documented-but-false claim this repo has been bitten
    # by before, and the fix is the message, not the check.
    assert "win_heif_dist" in str(exc.value)


def test_unknown_stage_is_rejected() -> None:
    with pytest.raises(heif.HeifError) as exc:
        heif.build(REAL, "macos", "arm64", Path("/dist"), stage_arg="nonesuch")
    assert "unknown stage" in str(exc.value)


def test_current_stamp_short_circuits_the_whole_build(tmp_path: Path) -> None:
    (tmp_path / ".pins").write_text(heif.want_pins(REAL, "arm64"), encoding="utf-8")
    (tmp_path / "lib").mkdir()
    (tmp_path / "lib" / "libheif.1.dylib").write_bytes(b"")
    (tmp_path / "lib" / "libde265.0.dylib").write_bytes(b"")
    # No run() mock installed: if any stage executed, it would try to shell
    # out to a real tool and fail.
    assert heif.build(REAL, "macos", "arm64", tmp_path) == 0


def test_default_dist_suffixes_only_the_cross_architecture_path() -> None:
    native = Path("/repo/native")
    host = heif.host_arch()
    other = "x86_64" if host == "arm64" else "arm64"
    assert heif.default_dist(native, host) == native / "third_party" / "heif-dist"
    assert heif.default_dist(native, other) == native / "third_party" / f"heif-dist-{other}"


def test_libheif_stage_refuses_to_run_before_its_three_dependencies(tmp_path: Path) -> None:
    with pytest.raises(heif.HeifError) as exc:
        heif.build_libheif(REAL, "macos", "arm64", tmp_path, tmp_path / "stage")
    assert "build libde265/kvazaar/aom first" in str(exc.value)


def test_assemble_refuses_to_run_before_the_artefacts_exist(tmp_path: Path) -> None:
    with pytest.raises(heif.HeifError) as exc:
        heif.assemble(REAL, "macos", "arm64", tmp_path, tmp_path / "stage")
    assert "run libde265/kvazaar/aom/libheif stages first" in str(exc.value)


# ---------------------------------------------------------------------------
# The manifest really is the single source of the build options (the whole
# point of the port): assert the rendered argv reproduces the flags the shell
# script passed by hand.
# ---------------------------------------------------------------------------
@pytest.mark.parametrize(
    "flag",
    [
        "-DWITH_LIBDE265=ON",
        "-DWITH_KVAZAAR=ON",
        "-DWITH_AOM_ENCODER=ON",
        "-DWITH_AOM_DECODER=ON",
        "-DWITH_X265=OFF",
        "-DENABLE_PLUGIN_LOADING=OFF",
        "-DWITH_LIBDE265_PLUGIN=OFF",
        "-DWITH_KVAZAAR_PLUGIN=OFF",
        "-DBUILD_SHARED_LIBS=ON",
        "-DCMAKE_INSTALL_NAME_DIR=@rpath",
    ],
)
def test_libheif_configure_argv_carries_the_shell_scripts_flags(flag: str) -> None:
    from deps import render as render_mod

    argv = render_mod.render(REAL, "libheif", "macos", "arm64", dist="/D")
    assert flag in argv


def test_kvazaar_configure_argv_is_static_and_pic() -> None:
    from deps import render as render_mod

    argv = render_mod.render(REAL, "kvazaar", "linux", "x86_64", dist="/D")
    # Without PIC the link fails on Linux with a relocation error that names
    # libheif rather than the archive.
    assert "-DCMAKE_POSITION_INDEPENDENT_CODE=ON" in argv
    assert "-DBUILD_SHARED_LIBS=OFF" in argv


# ---------------------------------------------------------------------------
# A-T4: android (NDK cross-compile) leg.
#
# Every test below is a mechanical encoding of a failure this leg can produce
# SILENTLY -- a stamp that cannot see a missing component, a capability lost
# to a non-fatal cmake probe, a versioned SONAME that only fails on a device.
# ---------------------------------------------------------------------------
def _fake_ndk(tmp_path: Path) -> Path:
    """A directory shaped enough like an NDK for tool/revision resolution."""
    ndk = tmp_path / "ndk"
    binaries = ndk / "toolchains" / "llvm" / "prebuilt" / "darwin-x86_64" / "bin"
    binaries.mkdir(parents=True)
    for tool in ("llvm-nm", "llvm-readelf"):
        (binaries / tool).write_text("#!/bin/sh\n", encoding="utf-8")
    (ndk / "source.properties").write_text(
        "Pkg.Desc = Android NDK\nPkg.Revision = 27.2.12479018\n", encoding="utf-8"
    )
    return ndk


def test_android_pin_string_names_every_component(tmp_path: Path) -> None:
    # Plan Step 4.3, the round's highest-risk defect made mechanical: a stamp
    # missing a component reports a stale decode-only dist as "already at the
    # pinned versions" and the encoders silently never appear.
    stamp = heif.android_pin_string(REAL, "arm64-v8a", str(_fake_ndk(tmp_path)))
    for token in ("libheif", "libde265", "kvazaar", "aom", "arm64-v8a", "27.2.12479018"):
        assert token in stamp


def test_android_pin_string_quotes_the_android_source_pins_not_the_desktop_ones(
    tmp_path: Path,
) -> None:
    # macOS/Linux take libde265 and aom from vcpkg; android builds them from
    # tarballs. A stamp carrying the desktop 'vcpkg' token would describe bytes
    # this dist does not contain.
    stamp = heif.android_pin_string(REAL, "arm64-v8a", str(_fake_ndk(tmp_path)))
    assert "vcpkg" not in stamp
    assert "fd48a927" in stamp  # libde265 1.1.1 tarball
    assert "git:v2.3.1" in stamp  # kvazaar clone, not tarball


def test_android_stamp_without_ndk_revision_is_a_failure(tmp_path: Path) -> None:
    ndk = _fake_ndk(tmp_path)
    (ndk / "source.properties").unlink()
    with pytest.raises(heif.HeifError) as exc:
        heif.android_pin_string(REAL, "arm64-v8a", str(ndk))
    assert "source.properties" in str(exc.value)


def test_android_build_refuses_to_run_without_an_ndk(tmp_path: Path) -> None:
    # Falling back to a host toolchain here would produce a dist whose NAME
    # promises arm64-v8a and whose BYTES are the host's.
    with pytest.raises(heif.HeifError) as exc:
        heif.build(REAL, "android", "arm64-v8a", tmp_path, ndk=None)
    assert "--android-ndk" in str(exc.value)


def test_ndk_tool_missing_instrument_is_a_failure_not_a_skip(tmp_path: Path) -> None:
    with pytest.raises(heif.HeifError) as exc:
        heif.ndk_tool(str(tmp_path / "not-an-ndk"), "llvm-nm")
    assert "llvm-nm" in str(exc.value)


def test_android_required_symbols_may_live_in_the_full_table(tmp_path: Path) -> None:
    # WITH_REDUCED_VISIBILITY=ON keeps the merged kvazaar/aom symbols out of
    # .dynsym. Checking only the dynamic table would measure VISIBILITY and
    # report a present capability as absent.
    dynamic = (
        "0000000000001000 T heif_decode_image\n"
        "0000000000002000 T heif_context_get_encoder_for_format\n"
    )
    full = (
        dynamic
        + "0000000000003000 t kvz_api_get\n"
        + "0000000000004000 t aom_codec_av1_cx\n"
        + "0000000000005000 t aom_codec_av1_dx\n"
    )
    heif.check_symbols(dynamic, "0x1 (NEEDED) [libde265.so]", full_symbols=full)
    with pytest.raises(heif.HeifError):
        heif.check_symbols(dynamic, "0x1 (NEEDED) [libde265.so]")


def test_android_x265_absence_is_checked_against_the_full_table_too() -> None:
    # The forbidden check must see EVERY table: a GPL symbol hiding in the one
    # we did not read is exactly the contamination it exists to catch.
    dynamic = (
        "0000000000001000 T heif_decode_image\n"
        "0000000000002000 T heif_context_get_encoder_for_format\n"
    )
    full = (
        dynamic
        + "0000000000003000 t kvz_api_get\n"
        + "0000000000004000 t aom_codec_av1_cx\n"
        + "0000000000005000 t aom_codec_av1_dx\n"
        + "0000000000006000 t x265_encoder_open\n"
    )
    with pytest.raises(heif.HeifError) as exc:
        heif.check_symbols(dynamic, "0x1 (NEEDED) [libde265.so]", full_symbols=full)
    assert "GPL" in str(exc.value)


def test_android_soname_must_be_unversioned(tmp_path: Path) -> None:
    ndk = str(_fake_ndk(tmp_path))
    library = tmp_path / "libde265.so"
    library.write_bytes(b"")
    versioned = _fake_runner({heif.ndk_tool(ndk, "llvm-readelf"): " 0x0e (SONAME) Library soname: [libde265.so.0]\n"})
    with mock.patch.object(heif, "run", versioned):
        with pytest.raises(heif.HeifError) as exc:
            heif._assert_android_soname(library, "libde265.so", ndk)
    assert "libde265.so.0" in str(exc.value)

    plain = _fake_runner({heif.ndk_tool(ndk, "llvm-readelf"): " 0x0e (SONAME) Library soname: [libde265.so]\n"})
    with mock.patch.object(heif, "run", plain):
        heif._assert_android_soname(library, "libde265.so", ndk)


def test_android_arch_check_rejects_a_host_arch_object(tmp_path: Path) -> None:
    ndk = str(_fake_ndk(tmp_path))
    library = tmp_path / "libheif.so"
    library.write_bytes(b"")
    x86 = _fake_runner(
        {heif.ndk_tool(ndk, "llvm-readelf"): "  Class: ELF64\n  Machine: Advanced Micro Devices X86-64\n"}
    )
    with mock.patch.object(heif, "run", x86):
        with pytest.raises(heif.HeifError) as exc:
            heif.assert_arch("android", "arm64-v8a", [library], ndk=ndk)
    assert "AArch64" in str(exc.value)

    aarch64 = _fake_runner(
        {heif.ndk_tool(ndk, "llvm-readelf"): "  Class: ELF64\n  Machine: AArch64\n"}
    )
    with mock.patch.object(heif, "run", aarch64):
        heif.assert_arch("android", "arm64-v8a", [library], ndk=ndk)


def test_android_alignment_is_measured_and_recorded(tmp_path: Path) -> None:
    # F5/R4: nothing in this repo handled 16 KB pages before. The value is
    # MEASURED and written next to the binaries it describes, never asserted
    # from memory.
    ndk = str(_fake_ndk(tmp_path))
    library = tmp_path / "libheif.so"
    library.write_bytes(b"")
    segments = (
        "  Type   Offset   VirtAddr   PhysAddr   FileSiz  MemSiz   Flg Align\n"
        "  LOAD   0x000000 0x00000000 0x00000000 0x001000 0x001000 R   0x4000\n"
        "  LOAD   0x002000 0x00002000 0x00002000 0x001000 0x001000 R E 0x4000\n"
    )
    with mock.patch.object(heif, "run", _fake_runner({heif.ndk_tool(ndk, "llvm-readelf"): segments})):
        report = heif.measure_android_alignment(tmp_path, [library], ndk)
    text = report.read_text(encoding="utf-8")
    assert "libheif.so LOAD alignments: 0x4000 0x4000" in text


def test_android_dist_path_is_abi_suffixed() -> None:
    # publish_release.py parses the ABI spelling out of this directory name.
    assert heif.default_dist(Path("/n"), "arm64-v8a", "android").name == (
        "heif-dist-android-arm64-v8a"
    )


def test_aom_android_tarball_extracts_to_libaom_not_aom() -> None:
    # The archive's top-level directory is "libaom-<version>"; declaring it in
    # the manifest is what stops "did not extract to ..." three steps later.
    from deps import execute as execute_mod

    block = execute_mod.resolve_source(REAL, "aom", "android")
    version = execute_mod.component_version(REAL, "aom")
    assert execute_mod.source_dirname(block, "aom", version) == f"libaom-{version}"
    assert execute_mod.source_dirname({}, "kvazaar", "2.3.1") == "kvazaar-2.3.1"
