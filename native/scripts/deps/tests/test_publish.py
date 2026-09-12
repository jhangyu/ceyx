"""Minimal regression check for publish_release_assets()'s "Latest" flag.

A real (non-prerelease) release must explicitly pass --latest to `gh
release create` -- see native/scripts/deps/publish.py:publish_release_assets
for why implicit default behaviour is not trustworthy here (two consecutive
production releases published without the "Latest" label). This test does
not invoke `gh`; it captures the argv publish_release_assets would run.
"""
from __future__ import annotations

from deps import publish
from deps.publish import build_artifacts_lock


def _fake_run(recorded):
    def run(argv, **_kwargs):
        recorded.append(list(argv))

        class _Result:
            returncode = 1  # "gh release view" -> not found -> create path

        return _Result()

    return run


def test_real_release_gets_explicit_latest_flag(tmp_path, monkeypatch):
    asset = tmp_path / "asset.tar.gz"
    asset.write_bytes(b"x")
    recorded: list[list[str]] = []
    monkeypatch.setattr(publish, "run", _fake_run(recorded))

    publish.publish_release_assets(
        "v9.9.9", [asset], repo="jhangyu/ceyx", prerelease=False
    )

    create_argv = recorded[1]
    assert "--latest" in create_argv
    assert "--latest=false" not in create_argv
    assert "--prerelease" not in create_argv


def test_prerelease_does_not_get_latest_flag(tmp_path, monkeypatch):
    asset = tmp_path / "asset.tar.gz"
    asset.write_bytes(b"x")
    recorded: list[list[str]] = []
    monkeypatch.setattr(publish, "run", _fake_run(recorded))

    publish.publish_release_assets(
        "r5-test-x", [asset], repo="jhangyu/ceyx", prerelease=True
    )

    create_argv = recorded[1]
    assert "--prerelease" in create_argv
    assert "--latest" not in create_argv


def test_build_artifacts_lock_embeds_min_runtime_when_given(tmp_path):
    """WI-14 step 14.4: a min_runtime value passed in is embedded verbatim."""
    asset = tmp_path / "dng_decoder_native-windows-x86_64.tar.gz"
    asset.write_bytes(b"x")
    lock = build_artifacts_lock([asset], {asset.name: "6.0"})
    assert lock["assets"][asset.name]["min_runtime"] == "6.0"


def test_build_artifacts_lock_omits_min_runtime_key_when_not_given(tmp_path):
    """A dist asset with no map entry gets no min_runtime key at all --
    never a null placeholder."""
    asset = tmp_path / "heif-dist-windows-x86_64.tar.gz"
    asset.write_bytes(b"x")
    lock = build_artifacts_lock([asset], {})
    assert "min_runtime" not in lock["assets"][asset.name]
