"""A-T2: android dist acquisition, ELF assertion helpers, and the libwebp
expectation table.

None of these tests need an NDK or a built dist: the NDK is faked as a
directory layout (the tools are never executed on the paths under test), and
the ELF/symbol assertions read captured dump FILES, which is exactly the
capture-then-grep shape the real checks use.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

_SCRIPTS = Path(__file__).resolve().parents[2]  # native/scripts
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from deps import android_dist  # noqa: E402
from deps import assertions as assertions_mod  # noqa: E402
from deps import execute as execute_mod  # noqa: E402
from deps import manifest as manifest_mod  # noqa: E402
from deps import render as render_mod  # noqa: E402


def _load():
    return manifest_mod.load()


# ---------------------------------------------------------------------------
# acquisition


def test_android_acquires_libwebp_from_the_same_upstream_release_as_the_registry() -> None:
    loaded = _load()
    default = loaded["manifest"]["component"]["libwebp"]["source"]["default"]
    android = execute_mod.resolve_source(loaded, "libwebp", "android")
    assert android["kind"] == "tarball"
    # Byte-identical to the historical pin, not re-derived: this is what makes
    # the android dist provably the same upstream 1.6.0 as the desktop one.
    assert android["url"] == default["historical_url"]
    assert android["sha256"] == default["historical_sha256"]


def test_desktop_libwebp_still_comes_from_the_registry() -> None:
    loaded = _load()
    for platform in ("macos", "linux", "windows"):
        assert execute_mod.resolve_source(loaded, "libwebp", platform)["kind"] == "registry"


def test_android_render_enables_the_mux_writer_and_keeps_the_other_tools_off() -> None:
    loaded = _load()
    argv = render_mod.render(loaded, "libwebp", "android", "arm64-v8a", ndk="/n")
    assert "-DWEBP_BUILD_WEBPMUX=ON" in argv
    for off in ("WEBP_BUILD_CWEBP", "WEBP_BUILD_DWEBP", "WEBP_BUILD_ANIM_UTILS",
                "WEBP_BUILD_GIF2WEBP", "WEBP_BUILD_IMG2WEBP", "WEBP_BUILD_VWEBP",
                "WEBP_BUILD_WEBPINFO", "WEBP_BUILD_EXTRAS"):
        assert f"-D{off}=OFF" in argv
    assert "-DBUILD_SHARED_LIBS=OFF" in argv
    assert "-DCMAKE_POSITION_INDEPENDENT_CODE=ON" in argv
    # the mux ON must not leak to the platforms whose dists are already built
    assert "-DWEBP_BUILD_WEBPMUX=OFF" in render_mod.render(loaded, "libwebp", "linux", "x86_64")


# ---------------------------------------------------------------------------
# ELF helpers (capture then grep)


def test_symbols_present_is_green_when_every_symbol_is_in_the_dump(tmp_path: Path) -> None:
    dump = tmp_path / "syms.txt"
    dump.write_text("0000 T WebPEncodeRGBA\n0000 T WebPDecodeRGBA\n", encoding="utf-8")
    assertions_mod.assert_symbols_present(dump, ["WebPEncodeRGBA", "WebPDecodeRGBA"], label="X")


def test_symbols_present_is_red_and_names_the_missing_symbol(tmp_path: Path) -> None:
    dump = tmp_path / "syms.txt"
    dump.write_text("0000 T WebPDecodeRGBA\n", encoding="utf-8")
    with pytest.raises(assertions_mod.AssertionFailed) as exc:
        assertions_mod.assert_symbols_present(dump, ["WebPEncodeRGBA"], label="A-SYMS")
    assert "WebPEncodeRGBA" in str(exc.value)


def test_elf_machine_green_and_red(tmp_path: Path) -> None:
    dump = tmp_path / "hdr.txt"
    dump.write_text("  Machine:  AArch64\n", encoding="utf-8")
    assertions_mod.assert_elf_machine(dump, "AArch64", label="A-ARCH")
    dump.write_text("  Machine:  Advanced Micro Devices X86-64\n", encoding="utf-8")
    with pytest.raises(assertions_mod.AssertionFailed):
        assertions_mod.assert_elf_machine(dump, "AArch64", label="A-ARCH")


def test_dir_non_empty_green_and_red(tmp_path: Path) -> None:
    d = tmp_path / "licences"
    d.mkdir()
    with pytest.raises(assertions_mod.AssertionFailed):
        assertions_mod.assert_dir_non_empty(d, label="A-LICENCE")
    (d / "COPYING").write_text("BSD-3", encoding="utf-8")
    assertions_mod.assert_dir_non_empty(d, label="A-LICENCE")


def test_capture_tool_output_writes_the_evidence_before_judging_it(tmp_path: Path) -> None:
    out = tmp_path / "evidence" / "echo.txt"
    assertions_mod.capture_tool_output([sys.executable, "-c", "print('WebPEncodeRGBA')"], out)
    assert "WebPEncodeRGBA" in out.read_text(encoding="utf-8")


def test_capture_tool_output_keeps_the_evidence_when_the_tool_fails(tmp_path: Path) -> None:
    out = tmp_path / "fail.txt"
    with pytest.raises(assertions_mod.AssertionFailed):
        assertions_mod.capture_tool_output(
            [sys.executable, "-c", "import sys; sys.stderr.write('boom'); sys.exit(3)"], out
        )
    assert "boom" in out.read_text(encoding="utf-8")  # evidence survives the failure


def _fake_ndk(tmp_path: Path) -> Path:
    ndk = tmp_path / "ndk"
    bindir = ndk / "toolchains" / "llvm" / "prebuilt" / "darwin-x86_64" / "bin"
    bindir.mkdir(parents=True)
    for tool in ("llvm-nm", "llvm-readelf"):
        (bindir / tool).write_text("", encoding="utf-8")
    return ndk


def test_ndk_tool_discovers_the_single_prebuilt_host_dir(tmp_path: Path) -> None:
    ndk = _fake_ndk(tmp_path)
    assert assertions_mod.ndk_tool(ndk, "llvm-nm").name == "llvm-nm"


def test_ndk_tool_rejects_a_directory_that_is_not_an_ndk(tmp_path: Path) -> None:
    with pytest.raises(assertions_mod.AssertionFailed):
        assertions_mod.ndk_tool(tmp_path, "llvm-nm")


# ---------------------------------------------------------------------------
# expectation table / dist shape


def test_libwebp_expectations_cover_the_four_required_archives_and_headers() -> None:
    spec = android_dist.EXPECTATIONS["libwebp"]
    assert set(spec["archives"]) == {
        "lib/libwebp.a", "lib/libsharpyuv.a", "lib/libwebpmux.a", "lib/libwebpdemux.a",
    }
    assert spec["headers"] == [
        "include/webp/encode.h", "include/webp/decode.h",
        "include/webp/mux.h", "include/webp/demux.h",
    ]
    # capability symbols, not filler
    assert "WebPEncodeRGBA" in spec["archives"]["lib/libwebp.a"]
    assert "WebPMuxCreateInternal" in spec["archives"]["lib/libwebpmux.a"]


def test_assert_dist_fails_loudly_when_an_archive_is_missing(tmp_path: Path) -> None:
    ndk = _fake_ndk(tmp_path)
    dist = tmp_path / "dist"
    dist.mkdir()
    with pytest.raises(android_dist.AndroidDistError) as exc:
        android_dist.assert_dist("libwebp", dist, ndk, "arm64-v8a")
    assert "libwebp.a" in str(exc.value)


def test_assert_dist_refuses_a_component_with_no_declared_expectations(tmp_path: Path) -> None:
    with pytest.raises(android_dist.AndroidDistError):
        android_dist.assert_dist("kvazaar", tmp_path, _fake_ndk(tmp_path), "arm64-v8a")


def test_vendor_licences_copies_the_manifest_patterns(tmp_path: Path) -> None:
    loaded = _load()
    stage = tmp_path / "stage" / "libwebp-1.6.0"
    stage.mkdir(parents=True)
    (stage / "COPYING").write_text("BSD-3-Clause", encoding="utf-8")
    dist = tmp_path / "dist"
    copied = android_dist.vendor_licences(loaded, "libwebp", dist, tmp_path / "stage")
    assert len(copied) == 1
    assert (dist / "share" / "licenses" / "libwebp" / "COPYING").is_file()


def test_vendor_licences_refuses_to_ship_a_dist_with_no_licence(tmp_path: Path) -> None:
    loaded = _load()
    stage = tmp_path / "stage" / "libwebp-1.6.0"
    stage.mkdir(parents=True)
    (stage / "README.md").write_text("no licence here", encoding="utf-8")
    with pytest.raises(android_dist.AndroidDistError):
        android_dist.vendor_licences(loaded, "libwebp", tmp_path / "dist", tmp_path / "stage")
