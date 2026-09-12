"""Tests for publish_release.py's min_runtime_for_asset() (WI-14 step 14.4
ruling: the publish job copies DECLARED values into artifacts.lock)."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import publish_release as pr  # noqa: E402


def _item(component, platform, asset_name="asset.tar.gz", arch=None):
    item = {"component": component, "platform": platform, "asset_name": asset_name}
    if arch is not None:
        item["arch"] = arch
    return item


def test_decoder_asset_returns_declared_value():
    declared = {"windows": {"value": "6.0"}}
    value = pr.min_runtime_for_asset(_item("dng_decoder_native", "windows"), declared)
    assert value == "6.0"


def test_non_decoder_asset_returns_none_not_an_error():
    declared = {"windows": {"value": "6.0"}}
    value = pr.min_runtime_for_asset(_item("heif-dist", "windows"), declared)
    assert value is None


def test_decoder_asset_missing_platform_raises_loudly():
    """Never silently write a missing min_runtime as null -- raise instead."""
    declared = {"windows": {"value": "6.0"}}
    try:
        pr.min_runtime_for_asset(_item("dng_decoder_native", "linux"), declared)
        assert False, "expected ManifestError"
    except pr.ManifestError as exc:
        assert "linux" in str(exc)


def test_macos_arm64_and_x86_64_assets_resolve_to_their_own_arch_floors():
    """REPLACES test_macos_..._resolve_to_the_single_macos_key (2026-09-12).
    macos-arm64 and macos-x86_64 assets share item['platform'] == 'macos' but
    NOT the same floor -- they bundle different OpenMP runtimes (15.0 vs
    14.0). Resolving both to one value is what sent the x86_64 leg red in CI
    run 34697591379, so the two must now come out different."""
    declared = {"macos": {"arm64": {"value": "15.0"}, "x86_64": {"value": "14.0"}}}
    v1 = pr.min_runtime_for_asset(
        _item("dng_decoder_native", "macos",
              "dng_decoder_native-macos-arm64.tar.gz", arch="arm64"),
        declared,
    )
    v2 = pr.min_runtime_for_asset(
        _item("dng_decoder_native", "macos",
              "dng_decoder_native-macos-x86_64.tar.gz", arch="x86_64"),
        declared,
    )
    assert v1 == "15.0"
    assert v2 == "14.0"


def test_per_arch_platform_without_an_arch_raises_rather_than_guessing():
    """A decoder item for a per-arch platform that carries no arch must fail
    loudly -- picking either subtable would write a wrong floor into the lock."""
    declared = {"macos": {"arm64": {"value": "15.0"}, "x86_64": {"value": "14.0"}}}
    try:
        pr.min_runtime_for_asset(_item("dng_decoder_native", "macos"), declared)
        assert False, "expected ManifestError"
    except pr.ManifestError as exc:
        assert "PER-ARCH" in str(exc) or "per-arch" in str(exc)


def test_unknown_arch_for_per_arch_platform_raises():
    declared = {"macos": {"arm64": {"value": "15.0"}, "x86_64": {"value": "14.0"}}}
    try:
        pr.min_runtime_for_asset(
            _item("dng_decoder_native", "macos", arch="riscv64"), declared)
        assert False, "expected ManifestError"
    except pr.ManifestError as exc:
        assert "riscv64" in str(exc)


def test_real_declaration_file_loads_and_resolves_all_platforms():
    """Integration check against the REAL committed declaration -- this is the
    check that would have caught the macos regression had the arch been wired
    through, so it now exercises both macOS legs by their real arch tags."""
    declared = pr.load_min_runtime_expected()
    for platform in ("windows", "linux", "android"):
        value = pr.min_runtime_for_asset(_item("dng_decoder_native", platform), declared)
        assert value is not None
    assert pr.min_runtime_for_asset(
        _item("dng_decoder_native", "macos", arch="arm64"), declared) == "15.0"
    assert pr.min_runtime_for_asset(
        _item("dng_decoder_native", "macos", arch="x86_64"), declared) == "14.0"
