"""Tests for publish_release.py's min_runtime_for_asset() (WI-14 step 14.4
ruling: the publish job copies DECLARED values into artifacts.lock)."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import publish_release as pr  # noqa: E402


def _item(component, platform, asset_name="asset.tar.gz"):
    return {"component": component, "platform": platform, "asset_name": asset_name}


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


def test_macos_arm64_and_x86_64_assets_both_resolve_to_the_single_macos_key():
    """macos-arm64 and macos-x86_64 assets share item['platform'] == 'macos';
    both must resolve against the single [macos] declaration entry."""
    declared = {"macos": {"value": "15.0"}}
    v1 = pr.min_runtime_for_asset(
        _item("dng_decoder_native", "macos", "dng_decoder_native-macos-arm64.tar.gz"),
        declared,
    )
    v2 = pr.min_runtime_for_asset(
        _item("dng_decoder_native", "macos", "dng_decoder_native-macos-x86_64.tar.gz"),
        declared,
    )
    assert v1 == v2 == "15.0"


def test_real_declaration_file_loads_and_resolves_all_platforms():
    """Integration check against the real committed declaration."""
    declared = pr.load_min_runtime_expected()
    for platform in ("windows", "linux", "macos", "android"):
        value = pr.min_runtime_for_asset(_item("dng_decoder_native", platform), declared)
        assert value is not None
