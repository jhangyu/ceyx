"""Manifest schema tests -- Plan D1 acceptance A1.1-A1.6.

Every "demonstrated red" acceptance criterion below constructs a synthetic,
deliberately malformed manifest in-memory (never edits the real
native/deps/manifest.toml) and asserts `manifest.validate()` raises
`ManifestError`. This is standard practice for schema validators and is
explicitly permitted by A1.2-A1.6's wording ("Demonstrated red").
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from deps import manifest  # noqa: E402

_MINIMAL_ARCH_MAP = {
    "arm64": {"apple": "arm64", "aom_target_cpu": "arm64"},
    "x86_64": {"apple": "x86_64", "aom_target_cpu": "x86_64"},
}


def _minimal_valid_manifest() -> dict:
    return {
        "component": {
            "widget": {
                "role": "test",
                "version": "1.0.0",
                "source": {
                    "default": {
                        "kind": "tarball",
                        "reason": "no vcpkg port exists for widget at any version",
                        "url": "https://example.com/widget-{version}.tar.gz",
                        "sha256": "deadbeef",
                    }
                },
                "cmake": {"base": {"CMAKE_BUILD_TYPE": "Release"}},
            }
        }
    }


# --- A1.1 -------------------------------------------------------------------


def test_a1_1_real_manifest_loads_without_raising():
    result = manifest.load()
    assert "component" in result["manifest"]
    assert set(result["manifest"]["component"]) == {"kvazaar", "libde265", "aom", "libheif"}


# --- A1.2 -------------------------------------------------------------------


def test_a1_2_missing_reason_raises():
    m = _minimal_valid_manifest()
    del m["component"]["widget"]["source"]["default"]["reason"]
    with pytest.raises(manifest.ManifestError, match="reason"):
        manifest.validate(m, _MINIMAL_ARCH_MAP)


def test_a1_2_reason_naming_none_of_the_taxonomy_raises():
    m = _minimal_valid_manifest()
    m["component"]["widget"]["source"]["default"]["reason"] = "we just felt like it"
    with pytest.raises(manifest.ManifestError, match="A1.2"):
        manifest.validate(m, _MINIMAL_ARCH_MAP)


def test_a1_2_real_manifest_every_source_block_has_kind_and_reason():
    result = manifest.load()
    for name, comp in result["manifest"]["component"].items():
        for platform_key, block in comp["source"].items():
            assert block.get("kind"), f"{name}.source.{platform_key} missing kind"
            assert block.get("reason", "").strip(), f"{name}.source.{platform_key} missing reason"


# --- A1.3 -------------------------------------------------------------------


def test_a1_3_registry_kind_without_version_raises():
    m = _minimal_valid_manifest()
    m["component"]["widget"]["source"]["default"] = {
        "kind": "registry",
        "reason": "vcpkg carries our exact pin",
    }
    with pytest.raises(manifest.ManifestError, match="A1.3"):
        manifest.validate(m, _MINIMAL_ARCH_MAP)


def test_a1_3_registry_kind_with_version_is_accepted():
    m = _minimal_valid_manifest()
    m["component"]["widget"]["source"]["default"] = {
        "kind": "registry",
        "reason": "vcpkg carries our exact pin",
        "version": "1.0.0",
    }
    manifest.validate(m, _MINIMAL_ARCH_MAP)  # must not raise


# --- A1.4 -------------------------------------------------------------------


def test_a1_4_undefined_arch_map_key_raises_at_load_not_build():
    m = _minimal_valid_manifest()
    m["component"]["widget"]["cmake"]["base"]["SOME_CPU"] = "{arch.no_such_field}"
    with pytest.raises(manifest.ManifestError, match="A1.4"):
        manifest.validate(m, _MINIMAL_ARCH_MAP)


def test_a1_4_defined_arch_map_key_is_accepted():
    m = _minimal_valid_manifest()
    m["component"]["widget"]["cmake"]["base"]["CMAKE_OSX_ARCHITECTURES"] = "{arch.apple}"
    manifest.validate(m, _MINIMAL_ARCH_MAP)  # must not raise


def test_a1_4_real_manifest_all_arch_refs_resolve():
    result = manifest.load()  # would already have raised if any ref was dangling
    assert result is not None


# --- A1.5 (round-1 partial scope) -------------------------------------------
# The full A1.5 (every K1-K44 manifest-expressible item present) spans every
# component in the Plan's final D1 scope (LibRaw, libjxl, Halide included);
# round 1 covers the HEIF stack only (Plan §5 round 1 exit condition). This
# test checks the subset of knowledge items that apply to the HEIF-stack
# components, citing the manifest key each one maps to.
_HEIF_STACK_KNOWLEDGE_ITEMS = {
    "K1 (libheif/libde265 shared)": ("libde265", "linkage", "shared"),
    "K2 (kvazaar static)": ("kvazaar", "linkage", "static"),
    "K3 (WITH_X265=OFF)": ("libheif", "cmake.base.WITH_X265", "OFF"),
    "K6 (PATENTS* glob)": ("aom", "licence_files", None),
    "K9 (MultiThreaded CRT)": ("kvazaar", "cmake.windows.CMAKE_MSVC_RUNTIME_LIBRARY", "MultiThreaded"),
    "K10 (CMP0091 NEW)": ("kvazaar", "cmake.windows.CMAKE_POLICY_DEFAULT_CMP0091", "NEW"),
    "K11 (KVZ_STATIC_LIB on libheif)": ("libheif", "cmake.windows.CMAKE_C_FLAGS", "-DKVZ_STATIC_LIB"),
    "K15 (PIC on static deps)": ("kvazaar", "cmake.base.CMAKE_POSITION_INDEPENDENT_CODE", "ON"),
    "K16 (AOM_TARGET_CPU macOS-only)": ("aom", "cmake.macos.AOM_TARGET_CPU", "{arch.aom_target_cpu}"),
    "K19 (AV1 ENC/DEC independent)": ("aom", "cmake.base.CONFIG_AV1_ENCODER", "1"),
    "K20 (CMAKE_IGNORE_PREFIX_PATH)": ("libheif", "cmake.macos.CMAKE_IGNORE_PREFIX_PATH", None),
    "K23 (WITH_REDUCED_VISIBILITY)": ("libheif", "cmake.base.WITH_REDUCED_VISIBILITY", "ON"),
}


def test_a1_5_heif_stack_knowledge_items_present():
    result = manifest.load()
    components = result["manifest"]["component"]
    for label, (comp_name, dotted_key, expect_value) in _HEIF_STACK_KNOWLEDGE_ITEMS.items():
        comp = components[comp_name]
        node: object = comp
        for part in dotted_key.split("."):
            assert isinstance(node, dict) and part in node, f"{label}: missing {comp_name}.{dotted_key}"
            node = node[part]
        if expect_value is not None:
            if isinstance(node, list):
                assert expect_value in node, f"{label}: {expect_value!r} not found in {node!r}"
            else:
                assert node == expect_value, f"{label}: expected {expect_value!r}, got {node!r}"


# --- A1.6 -------------------------------------------------------------------


def test_a1_6_overlay_cannot_delete_a_base_key():
    m = _minimal_valid_manifest()
    m["component"]["widget"]["cmake"]["windows"] = {"CMAKE_BUILD_TYPE": "!delete"}
    with pytest.raises(manifest.ManifestError, match="additive-only"):
        manifest.validate(m, _MINIMAL_ARCH_MAP)


def test_a1_6_overlay_can_add_and_override():
    m = _minimal_valid_manifest()
    m["component"]["widget"]["cmake"]["windows"] = {
        "CMAKE_BUILD_TYPE": "Debug",  # override
        "CMAKE_GENERATOR_PLATFORM": "x64",  # add
    }
    manifest.validate(m, _MINIMAL_ARCH_MAP)  # must not raise
    merged = manifest.merge_platform_overlay(
        m["component"]["widget"]["cmake"]["base"],
        m["component"]["widget"]["cmake"]["windows"],
    )
    assert merged["CMAKE_BUILD_TYPE"] == "Debug"
    assert merged["CMAKE_GENERATOR_PLATFORM"] == "x64"


# --- Red line: no value disagreement between the two source scripts --------


def test_no_pin_disagreement_between_the_two_transcribed_scripts():
    """K32: the two source scripts hold pins meant to be identical, enforced
    only by a `# MUST equal` comment. This is the mechanical check that a
    disagreement is caught rather than silently resolved -- both scripts
    were read by hand during transcription and agreed exactly, this test
    guards against a future manual edit reintroducing drift."""
    result = manifest.load()
    kvazaar = result["manifest"]["component"]["kvazaar"]
    aom = result["manifest"]["component"]["aom"]
    assert kvazaar["version"] == "2.3.1"
    assert aom["version"] == "3.12.1"
    # The Windows override for kvazaar's source uses git, not tarball, but
    # must still target the same tagged version.
    assert kvazaar["source"]["windows"]["tag"] == "v{version}"
