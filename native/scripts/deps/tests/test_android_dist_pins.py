"""Regression coverage for the missing android `.pins` stamp: the generic
carrier path (execute.py's acquire()/build_component(), used by libjxl and
libwebp) had NO `.pins` write step at all -- unlike heif-stack's dedicated
android orchestration (deps/heif.py), which has always written one. Both
the committed libjxl and libwebp android dists shipped without a staleness
stamp as a result, silently defeating CI-T8's digest check.

New file (not an edit to test_android_dist.py/test_android_dist_libjxl.py)
since this concern -- the .pins stamp -- is orthogonal to any one
component's EXPECTATIONS row.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

_SCRIPTS = Path(__file__).resolve().parents[2]  # native/scripts
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from deps import android_dist  # noqa: E402
from deps import manifest as manifest_mod  # noqa: E402


def _load():
    return manifest_mod.load()


def _fake_ndk(tmp_path: Path, revision: str = "27.2.12479018") -> Path:
    ndk = tmp_path / "ndk"
    ndk.mkdir()
    (ndk / "source.properties").write_text(
        f"Pkg.Desc = Android NDK\nPkg.Revision = {revision}\n", encoding="utf-8"
    )
    return ndk


def test_pin_string_for_a_git_kind_component_names_the_tag(tmp_path: Path) -> None:
    """libjxl's android source is git-kind (component.libjxl.source.default,
    android has no override -- see manifest.toml)."""
    ndk = _fake_ndk(tmp_path)
    pin = android_dist.pin_string(_load(), "libjxl", "arm64-v8a", ndk)
    assert "libjxl=0.12.0:git:v0.12.0" in pin
    assert "arch=arm64-v8a" in pin
    assert "abi=arm64-v8a" in pin
    assert "ndk=27.2.12479018" in pin


def test_pin_string_for_a_tarball_kind_component_names_the_sha256(tmp_path: Path) -> None:
    """libwebp's android source is a tarball override
    (component.libwebp.source.android)."""
    ndk = _fake_ndk(tmp_path)
    pin = android_dist.pin_string(_load(), "libwebp", "arm64-v8a", ndk)
    loaded = _load()
    expected_sha = loaded["manifest"]["component"]["libwebp"]["source"]["android"]["sha256"]
    assert f"libwebp=1.6.0:{expected_sha}" in pin


def test_pin_string_format_matches_heif_stamp_shape(tmp_path: Path) -> None:
    """Same token shape as heif.android_pin_string() -- CI-T8 parses one
    format across every android dist, not two."""
    ndk = _fake_ndk(tmp_path)
    pin = android_dist.pin_string(_load(), "libjxl", "arm64-v8a", ndk)
    tokens = pin.split(" ")
    assert tokens[0].startswith("libjxl=")
    assert tokens[1] == "arch=arm64-v8a"
    assert tokens[2] == "abi=arm64-v8a"
    assert tokens[3].startswith("ndk=")


def test_pin_string_refuses_a_component_with_no_ndk_revision_available() -> None:
    """Red case: an NDK root missing source.properties must fail loudly,
    not silently omit the ndk= token (heif_mod.ndk_revision's own contract,
    reused rather than reimplemented)."""
    bad_ndk = Path("/nonexistent/ndk/root")
    with pytest.raises(Exception):  # heif.HeifError, imported lazily inside pin_string
        android_dist.pin_string(_load(), "libjxl", "arm64-v8a", bad_ndk)


def test_write_pins_writes_the_file_and_returns_its_path(tmp_path: Path) -> None:
    ndk = _fake_ndk(tmp_path)
    dist = tmp_path / "dist"
    dist.mkdir()
    path = android_dist.write_pins(dist, _load(), "libjxl", "arm64-v8a", ndk)
    assert path == dist / ".pins"
    assert path.is_file()
    assert "libjxl=0.12.0:git:v0.12.0" in path.read_text(encoding="utf-8")
