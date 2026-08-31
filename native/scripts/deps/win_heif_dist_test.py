"""Tests for win_heif_dist.py -- all runnable on macOS/Linux (spec §8.1).

Nothing here invokes cmake, clang-cl or dumpbin: the parts that CAN only be
proven on a Windows runner (that the dist links, that heif.dll really exports
heif_decode_image) are CI's job and are asserted there by the module itself.
What is proven here is the logic that would otherwise only be exercised on
Windows and would therefore be debugged one 20-minute CI round at a time:
pin-string derivation, the stamp fast path, layout resolution, and the
decode-only assertion wiring.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

# Imported through the PACKAGE (`deps.`), never flat: a flat `import
# win_heif_dist` sends its dependencies down their direct-execution fallback
# branch, where deps/render.py's own relative import has no parent package and
# raises. Going through the package also keeps exception identity single --
# `deps.win_pe.PeInspectionError` raised inside the module is the same class
# object this test catches, which a flat second copy would silently break.
from deps import manifest, win_heif_dist, win_pe  # noqa: E402


def _dist_with(files: dict, root: Path) -> Path:
    for relative, content in files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
    return root


class TestWantPins(unittest.TestCase):
    def setUp(self) -> None:
        self.loaded = manifest.load()

    def test_pins_are_derived_from_the_manifest_not_restated(self) -> None:
        pins = win_heif_dist.want_pins(self.loaded, "x86_64")
        components = self.loaded["manifest"]["component"]
        self.assertIn(f"libheif={components['libheif']['version']}:", pins)
        self.assertIn(f"libde265={components['libde265']['version']}:", pins)
        self.assertTrue(pins.endswith("platform=windows-x86_64"))

    def test_pins_use_the_windows_de265_hash(self) -> None:
        """libde265 resolves to vcpkg on Unix but to a SHA-256-pinned tarball
        on Windows; the stamp must carry the Windows hash, not the Unix
        'vcpkg' marker."""
        pins = win_heif_dist.want_pins(self.loaded, "x86_64")
        windows_sha = self.loaded["manifest"]["component"]["libde265"]["source"]["windows"]["sha256"]
        self.assertIn(windows_sha, pins)
        self.assertNotIn("vcpkg", pins)

    def test_format_matches_the_four_component_shape(self) -> None:
        """Format changed 2026-08-31 when kvazaar and aom joined the dist
        (spec-windows-codec-full-green.md). The change is deliberately
        stamp-invalidating -- superseding rather than pinning the OLD
        two-component byte-for-byte shape, so an existing decode-only tree's
        stamp cannot be mistaken for current."""
        components = self.loaded["manifest"]["component"]
        expected = (
            f"libheif={components['libheif']['version']}:"
            f"{components['libheif']['source']['default']['sha256']} "
            f"libde265={components['libde265']['version']}:"
            f"{components['libde265']['source']['windows']['sha256']} "
            f"kvazaar={components['kvazaar']['version']}:"
            f"{win_heif_dist._source_pin(components['kvazaar'], 'windows')} "
            f"aom={components['aom']['version']}:"
            f"{win_heif_dist._source_pin(components['aom'], 'windows')} "
            f"platform=windows-x86_64"
        )
        self.assertEqual(win_heif_dist.want_pins(self.loaded, "x86_64"), expected)


class TestStampFastPath(unittest.TestCase):
    def setUp(self) -> None:
        self.loaded = manifest.load()

    def test_current_when_pins_match_and_both_dlls_exist(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with(
                {
                    ".pins": win_heif_dist.want_pins(self.loaded, "x86_64"),
                    "bin/heif.dll": "x",
                    "bin/libde265.dll": "x",
                },
                Path(tmp),
            )
            self.assertTrue(win_heif_dist.stamp_is_current(dist, self.loaded, "x86_64"))

    def test_not_current_when_a_dll_was_deleted(self) -> None:
        """A stamp can outlive a deleted DLL; the fast path must not then
        declare a dist that ships nothing to be ready."""
        with TemporaryDirectory() as tmp:
            dist = _dist_with(
                {".pins": win_heif_dist.want_pins(self.loaded, "x86_64"), "bin/heif.dll": "x"},
                Path(tmp),
            )
            self.assertFalse(win_heif_dist.stamp_is_current(dist, self.loaded, "x86_64"))

    def test_not_current_when_pins_differ(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with(
                {".pins": "libheif=0.0.1:deadbeef", "bin/heif.dll": "x", "bin/libde265.dll": "x"},
                Path(tmp),
            )
            self.assertFalse(win_heif_dist.stamp_is_current(dist, self.loaded, "x86_64"))

    def test_not_current_when_no_stamp_at_all(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with({"bin/heif.dll": "x", "bin/libde265.dll": "x"}, Path(tmp))
            self.assertFalse(win_heif_dist.stamp_is_current(dist, self.loaded, "x86_64"))

    def test_accepts_the_alternate_de265_dll_spelling(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with(
                {
                    ".pins": win_heif_dist.want_pins(self.loaded, "x86_64"),
                    "bin/heif.dll": "x",
                    "bin/de265.dll": "x",
                },
                Path(tmp),
            )
            self.assertTrue(win_heif_dist.stamp_is_current(dist, self.loaded, "x86_64"))


class TestLayoutAssertion(unittest.TestCase):
    COMPLETE = {
        "bin/heif.dll": "x",
        "bin/libde265.dll": "x",
        "lib/heif.lib": "x",
        "lib/de265.lib": "x",
        "include/libheif/heif.h": "x",
        "include/libde265/de265.h": "x",
    }

    def test_green_and_returns_the_resolved_dll(self) -> None:
        with TemporaryDirectory() as tmp:
            dist = _dist_with(dict(self.COMPLETE), Path(tmp))
            self.assertEqual(win_heif_dist.assert_layout(dist), "bin/libde265.dll")

    def test_red_names_the_missing_file(self) -> None:
        with TemporaryDirectory() as tmp:
            files = dict(self.COMPLETE)
            del files["lib/heif.lib"]
            dist = _dist_with(files, Path(tmp))
            with self.assertRaises(win_heif_dist.WindowsHeifError) as ctx:
                win_heif_dist.assert_layout(dist)
            self.assertIn("lib/heif.lib", str(ctx.exception))

    def test_import_library_asymmetry_is_preserved(self) -> None:
        """Upstream ships bin/libde265.dll alongside lib/de265.lib. The DLL is
        NOT renamed to match: heif.dll's import table names "libde265.dll", so
        a tidied copy would never be loaded."""
        with TemporaryDirectory() as tmp:
            dist = _dist_with(dict(self.COMPLETE), Path(tmp))
            self.assertEqual(win_heif_dist.assert_layout(dist), "bin/libde265.dll")
            self.assertEqual(win_heif_dist.resolve_de265_import_library(dist), "lib/de265.lib")

    def test_missing_de265_dll_is_reported_with_a_listing(self) -> None:
        with TemporaryDirectory() as tmp:
            files = dict(self.COMPLETE)
            del files["bin/libde265.dll"]
            dist = _dist_with(files, Path(tmp))
            with self.assertRaises(win_pe.PeInspectionError) as ctx:
                win_heif_dist.assert_layout(dist)
            self.assertIn("heif.dll", str(ctx.exception))


class TestDecodeOnlyAssertionSet(unittest.TestCase):
    """The Windows dist is now encode+decode capable (un-parked 2026-08-31,
    spec-windows-codec-full-green.md), but its capability assertions must
    still never be shaped as an export-table grep for the ENCODER symbols
    (R1): with WITH_REDUCED_VISIBILITY=ON a perfectly correct build exports
    neither ``kvz_api_get`` nor ``aom_codec_av1_*``. The kvazaar/aom checks
    that DO exist (below, TestEncoderCapabilityAssertions) are against the
    static archives on disk and the import table, never the export table --
    this class's name is kept ("DecodeOnly...") because it still documents
    what the *export*-table assertion set may never grow to cover."""

    def test_does_not_require_encoder_symbols(self) -> None:
        source = Path(win_heif_dist.__file__).read_text(encoding="utf-8")
        required = win_heif_dist._REQUIRED_HEIF_SYMBOL
        self.assertEqual(required, "heif_decode_image")
        for encoder_symbol in ("kvz_api_get", "aom_codec_av1_cx", "aom_codec_av1_dx"):
            self.assertNotIn(f'"{encoder_symbol}"', source)

    def test_gpl_contamination_scan_targets_x265(self) -> None:
        self.assertEqual(win_heif_dist._FORBIDDEN_HEIF_SYMBOL, "x265")

    def test_hevc_dependency_is_asserted(self) -> None:
        self.assertEqual(win_heif_dist._REQUIRED_HEIF_DEPENDENCY, "de265")


def _make_minimal_dist(tmp_path: Path) -> Path:
    """The existing decode-only layout (heif.dll + libde265.dll + headers),
    with NEITHER encoder static archive present."""
    return _dist_with(dict(TestLayoutAssertion.COMPLETE), tmp_path)


def _make_full_dist(tmp_path: Path) -> Path:
    """The minimal dist plus both encoder static archives -- a fully
    capability-complete Windows HEIF dist as far as inputs-on-disk go."""
    dist = _make_minimal_dist(tmp_path)
    (dist / "lib" / "kvazaar.lib").write_bytes(b"!<arch>\n")
    (dist / "lib" / "aom.lib").write_bytes(b"!<arch>\n")
    return dist


def test_source_pin_renders_the_version_template() -> None:
    """kvazaar's [component.kvazaar.source.windows] tag is the literal string
    ``"v{version}"`` -- a template, not a value. _source_pin must substitute
    {version} before returning; a stamp containing the raw placeholder would
    be IDENTICAL for every kvazaar release, so a version bump would never
    invalidate .pins and the rebuild it exists to force would be skipped."""
    loaded = manifest.load()
    kvazaar = loaded["manifest"]["component"]["kvazaar"]
    pin = win_heif_dist._source_pin(kvazaar, "windows")
    assert "{version}" not in pin
    assert pin == f"v{kvazaar['version']}"


def test_want_pins_never_contains_the_raw_template_placeholder() -> None:
    """End-to-end check on the assembled stamp, not just the helper: a
    template leak anywhere in want_pins() defeats the stamp's entire purpose
    (see test_source_pin_renders_the_version_template)."""
    loaded = manifest.load()
    pins = win_heif_dist.want_pins(loaded)
    assert "{version}" not in pins


def test_want_pins_includes_kvazaar_and_aom() -> None:
    """The stamp must cover every component whose bytes land in the dist.

    A stamp omitting kvazaar/aom would report an OLD decode-only dist as
    'already at the pinned versions' and skip the rebuild entirely.
    """
    loaded = manifest.load()
    pins = win_heif_dist.want_pins(loaded)
    assert "kvazaar=" in pins
    assert "aom=" in pins
    assert "libheif=" in pins and "libde265=" in pins


def test_build_installs_dependencies_before_libheif(monkeypatch, tmp_path) -> None:
    """libheif's find modules resolve against the INSTALLED tree, so all three
    dependencies must be on disk first. Order is asserted, not assumed."""
    calls = []
    for name in ("build_libde265", "build_kvazaar", "build_aom", "build_libheif"):
        monkeypatch.setattr(win_heif_dist, name,
                             lambda *a, _n=name, **k: calls.append(_n))
    monkeypatch.setattr(win_heif_dist, "assert_layout", lambda d: "bin/libde265.dll")
    monkeypatch.setattr(win_heif_dist, "assert_capabilities", lambda d, x: None)
    monkeypatch.setattr(win_heif_dist, "vendor_licences", lambda *a, **k: None)
    monkeypatch.setattr(win_heif_dist, "stamp_is_current", lambda *a, **k: False)
    monkeypatch.setattr(win_heif_dist, "want_pins", lambda *a, **k: "pins")
    win_heif_dist.build(manifest.load(), tmp_path)
    assert calls == ["build_libde265", "build_kvazaar", "build_aom", "build_libheif"]


def _patch_pe_reads(monkeypatch, *, deps_text: str) -> None:
    """R1: neither patched reader is asked about the encoder symbols -- the
    encoder checks in assert_capabilities look at static archives on disk and
    at ``deps_text`` (the import table), never at the export table.

    Also stubs `assert_architecture`: these tests write placeholder byte
    content for the DLLs (not real PE binaries), and this suite already
    documents (module docstring) that it never invokes real Windows tooling
    -- the architecture check is orthogonal to what is under test here.
    """
    monkeypatch.setattr(win_heif_dist.win_pe, "read_exports",
                         lambda dll, out: "    3    2 0002B230 heif_decode_image")
    monkeypatch.setattr(win_heif_dist.win_pe, "read_dependents",
                         lambda dll, out: deps_text)
    monkeypatch.setattr(win_heif_dist, "assert_architecture", lambda dist, de265_dll: None)


def test_assert_capabilities_rejects_missing_kvazaar_archive(monkeypatch, tmp_path) -> None:
    """A dist whose libheif configured with WITH_KVAZAAR=ON but whose kvazaar
    archive is absent installs happily and then encodes nothing."""
    dist = _make_minimal_dist(tmp_path)
    (dist / "lib" / "aom.lib").write_bytes(b"!<arch>\n")
    _patch_pe_reads(monkeypatch, deps_text="libde265.dll\nKERNEL32.dll")
    with pytest.raises(win_heif_dist.WindowsHeifError) as exc:
        win_heif_dist.assert_capabilities(dist, "bin/libde265.dll")
    assert "kvazaar" in str(exc.value)


def test_assert_capabilities_rejects_missing_aom_archive(monkeypatch, tmp_path) -> None:
    """Symmetric case for aom: present kvazaar, absent aom."""
    dist = _make_minimal_dist(tmp_path)
    (dist / "lib" / "kvazaar.lib").write_bytes(b"!<arch>\n")
    _patch_pe_reads(monkeypatch, deps_text="libde265.dll\nKERNEL32.dll")
    with pytest.raises(win_heif_dist.WindowsHeifError) as exc:
        win_heif_dist.assert_capabilities(dist, "bin/libde265.dll")
    assert "aom" in str(exc.value)


def test_assert_capabilities_rejects_dynamic_aom(monkeypatch, tmp_path) -> None:
    """aom must be MERGED statically. An aom.dll import means find_package
    picked an import library: runs on the build machine, breaks on a user's."""
    dist = _make_full_dist(tmp_path)
    _patch_pe_reads(monkeypatch, deps_text="libde265.dll\naom.dll\nKERNEL32.dll")
    with pytest.raises(win_heif_dist.WindowsHeifError) as exc:
        win_heif_dist.assert_capabilities(dist, "bin/libde265.dll")
    assert "aom.dll" in str(exc.value)


def test_assert_capabilities_rejects_dynamic_kvazaar(monkeypatch, tmp_path) -> None:
    """Symmetric case for kvazaar."""
    dist = _make_full_dist(tmp_path)
    _patch_pe_reads(monkeypatch, deps_text="libde265.dll\nkvazaar.dll\nKERNEL32.dll")
    with pytest.raises(win_heif_dist.WindowsHeifError) as exc:
        win_heif_dist.assert_capabilities(dist, "bin/libde265.dll")
    assert "kvazaar.dll" in str(exc.value)


def test_assert_capabilities_accepts_full_static_dist(monkeypatch, tmp_path) -> None:
    """Positive control: both archives present, no dynamic aom/kvazaar import
    -- assert_capabilities must return cleanly (it must not vacuously reject
    everything)."""
    dist = _make_full_dist(tmp_path)
    _patch_pe_reads(monkeypatch, deps_text="libde265.dll\nKERNEL32.dll")
    win_heif_dist.assert_capabilities(dist, "bin/libde265.dll")


def test_resolve_kvazaar_library_accepts_the_lib_prefixed_spelling(tmp_path) -> None:
    """kvazaar's CMake target is named 'kvazaar' but clang-cl+Ninja installed
    it as libkvazaar.lib (CI run 33415312766: '-- Installing: .../lib/
    libkvazaar.lib'), while the manifest's KVAZAAR_LIBRARY hint states the
    nominal 'lib/kvazaar.lib'. resolve_kvazaar_library must resolve the
    ACTUALLY installed spelling by presence, same as libde265's import lib."""
    (tmp_path / "lib").mkdir()
    (tmp_path / "lib" / "libkvazaar.lib").write_bytes(b"!<arch>\n")
    assert win_heif_dist.resolve_kvazaar_library(tmp_path) == "lib/libkvazaar.lib"


def test_resolve_kvazaar_library_prefers_the_nominal_spelling_when_both_exist(tmp_path) -> None:
    (tmp_path / "lib").mkdir()
    (tmp_path / "lib" / "kvazaar.lib").write_bytes(b"!<arch>\n")
    (tmp_path / "lib" / "libkvazaar.lib").write_bytes(b"!<arch>\n")
    assert win_heif_dist.resolve_kvazaar_library(tmp_path) == "lib/kvazaar.lib"


def test_build_libheif_overrides_both_libde265_and_kvazaar_library(monkeypatch, tmp_path) -> None:
    """A later -D on the cmake command line wins for the same cache variable;
    both LIBDE265_LIBRARY and KVAZAAR_LIBRARY must be resolved by presence
    and passed as overrides, not left to the manifest's nominal spelling."""
    (tmp_path / "lib").mkdir()
    (tmp_path / "lib" / "de265.lib").write_bytes(b"x")
    (tmp_path / "lib" / "libkvazaar.lib").write_bytes(b"!<arch>\n")

    captured = {}

    def _fake_build_component(loaded, name, platform, arch, dist, stage, *, extra_args=None):
        captured["extra_args"] = extra_args

    monkeypatch.setattr(win_heif_dist.execute_mod, "build_component", _fake_build_component)
    win_heif_dist.build_libheif(manifest.load(), "x86_64", tmp_path, tmp_path / "stage")

    joined = " ".join(captured["extra_args"])
    assert f"-DLIBDE265_LIBRARY={tmp_path / 'lib' / 'de265.lib'}" in joined
    assert f"-DKVAZAAR_LIBRARY={tmp_path / 'lib' / 'libkvazaar.lib'}" in joined


def test_assert_capabilities_accepts_the_lib_prefixed_kvazaar_spelling(monkeypatch, tmp_path) -> None:
    """Positive control for the CI-observed spelling: assert_capabilities must
    not vacuously reject a dist whose kvazaar archive is libkvazaar.lib."""
    dist = _make_minimal_dist(tmp_path)
    (dist / "lib" / "libkvazaar.lib").write_bytes(b"!<arch>\n")
    (dist / "lib" / "aom.lib").write_bytes(b"!<arch>\n")
    _patch_pe_reads(monkeypatch, deps_text="libde265.dll\nKERNEL32.dll")
    win_heif_dist.assert_capabilities(dist, "bin/libde265.dll")


def test_build_aom_copies_static_archive_and_headers_from_vcpkg_prefix(monkeypatch, tmp_path) -> None:
    """[component.aom]'s resolved source kind is 'registry' on every platform
    (D1-a adds no source.windows override), and execute.acquire() raises
    unconditionally for kind=='registry'. build_aom must NOT route through
    execute_mod.build_component -- it must copy lib/aom.lib + include/aom out
    of CEYX_VCPKG_PREFIX, mirroring deps/heif.py's build_aom."""
    vcpkg_prefix = tmp_path / "vcpkg-installed" / "x64-windows-heif"
    (vcpkg_prefix / "lib").mkdir(parents=True)
    (vcpkg_prefix / "lib" / "aom.lib").write_bytes(b"!<arch>\n")
    (vcpkg_prefix / "include" / "aom").mkdir(parents=True)
    (vcpkg_prefix / "include" / "aom" / "aom.h").write_text("x", encoding="utf-8")
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(vcpkg_prefix))

    dist = tmp_path / "dist"
    dist.mkdir()
    win_heif_dist.build_aom(manifest.load(), "x86_64", dist, tmp_path / "stage")

    assert (dist / "lib" / "aom.lib").is_file()
    assert (dist / "include" / "aom" / "aom.h").is_file()


def test_build_aom_fails_loud_when_vcpkg_prefix_unset(monkeypatch, tmp_path) -> None:
    monkeypatch.delenv("CEYX_VCPKG_PREFIX", raising=False)
    dist = tmp_path / "dist"
    dist.mkdir()
    with pytest.raises(win_heif_dist.WindowsHeifError) as exc:
        win_heif_dist.build_aom(manifest.load(), "x86_64", dist, tmp_path / "stage")
    assert "CEYX_VCPKG_PREFIX" in str(exc.value)


def test_vendor_licences_copies_aom_copyright_and_component_licences(monkeypatch, tmp_path) -> None:
    vcpkg_prefix = tmp_path / "vcpkg-installed" / "x64-windows-heif"
    (vcpkg_prefix / "share" / "aom").mkdir(parents=True)
    (vcpkg_prefix / "share" / "aom" / "copyright").write_text("BSD-2 + PATENTS", encoding="utf-8")
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(vcpkg_prefix))

    loaded = manifest.load()
    stage = tmp_path / "stage"
    for name in ("libheif", "libde265", "kvazaar"):
        version = loaded["manifest"]["component"][name]["version"]
        src = stage / f"{name}-{version}"
        src.mkdir(parents=True)
        (src / "COPYING").write_text(f"{name} licence text", encoding="utf-8")
    monkeypatch.setattr(win_heif_dist.execute_mod, "acquire", lambda *a, **k: (_ for _ in ()).throw(
        AssertionError("acquire() must not be called when the source tree already exists")))

    dist = tmp_path / "dist"
    dist.mkdir()
    win_heif_dist.vendor_licences(loaded, dist, stage)

    assert (dist / "share" / "licenses" / "aom" / "copyright").read_text(encoding="utf-8") == "BSD-2 + PATENTS"
    for name in ("libheif", "libde265", "kvazaar"):
        assert (dist / "share" / "licenses" / name / "COPYING").is_file()


def test_vendor_licences_reacquires_a_source_tree_missing_after_a_resumed_run(monkeypatch, tmp_path) -> None:
    """A stage the fast path skipped (component already installed) never
    extracted a source tree; vendor_licences must re-fetch it rather than
    raising 'no licence file found' against an empty directory."""
    vcpkg_prefix = tmp_path / "vcpkg-installed" / "x64-windows-heif"
    (vcpkg_prefix / "share" / "aom").mkdir(parents=True)
    (vcpkg_prefix / "share" / "aom" / "copyright").write_text("BSD-2 + PATENTS", encoding="utf-8")
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(vcpkg_prefix))

    loaded = manifest.load()
    stage = tmp_path / "stage"

    def _fake_acquire(loaded_arg, name, platform, stage_arg):
        version = loaded_arg["manifest"]["component"][name]["version"]
        src = Path(stage_arg) / f"{name}-{version}"
        src.mkdir(parents=True, exist_ok=True)
        (src / "LICENSE").write_text(f"{name} refetched licence", encoding="utf-8")
        return src

    monkeypatch.setattr(win_heif_dist.execute_mod, "acquire", _fake_acquire)

    dist = tmp_path / "dist"
    dist.mkdir()
    win_heif_dist.vendor_licences(loaded, dist, stage)

    for name in ("libheif", "libde265", "kvazaar"):
        assert (dist / "share" / "licenses" / name / "LICENSE").is_file()


def test_vendor_licences_fails_loud_when_aom_copyright_absent(monkeypatch, tmp_path) -> None:
    vcpkg_prefix = tmp_path / "vcpkg-installed" / "x64-windows-heif"
    vcpkg_prefix.mkdir(parents=True)
    monkeypatch.setenv("CEYX_VCPKG_PREFIX", str(vcpkg_prefix))
    dist = tmp_path / "dist"
    dist.mkdir()
    with pytest.raises(win_heif_dist.WindowsHeifError) as exc:
        win_heif_dist.vendor_licences(manifest.load(), dist, tmp_path / "stage")
    assert "aom" in str(exc.value)


if __name__ == "__main__":
    unittest.main()
