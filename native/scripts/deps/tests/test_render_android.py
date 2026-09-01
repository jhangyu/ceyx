"""A-T1: the dist carrier's `android` platform (cross-compile-only).

Covers both halves of the plan's acceptance: the renderer accepts android and
emits the NDK toolchain triple, and the ABI vocabulary is enforced rather than
aliased (`arm64` must NOT be silently accepted as `arm64-v8a`, because the
dist directory and release asset names carry the ABI spelling verbatim and
publish_release.py parses it back out).

The CLI half is exercised through `build_deps.main()` so the argv the plan's
acceptance criteria name is checked end to end, not just the pure renderer.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

_SCRIPTS = Path(__file__).resolve().parents[2]  # native/scripts
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

import build_deps  # noqa: E402
from deps import manifest as manifest_mod  # noqa: E402
from deps import render as render_mod  # noqa: E402

_NDK = "/opt/test-ndk"


def _load():
    return manifest_mod.load()


# ---------------------------------------------------------------------------
# render()


def test_android_platform_is_accepted_and_emits_the_ndk_toolchain_triple() -> None:
    argv = render_mod.render(_load(), "libwebp", "android", "arm64-v8a", ndk=_NDK)
    assert f"-DCMAKE_TOOLCHAIN_FILE={_NDK}/build/cmake/android.toolchain.cmake" in argv
    assert "-DANDROID_ABI=arm64-v8a" in argv
    assert "-DANDROID_PLATFORM=android-24" in argv
    # base keys still merge in under the overlay
    assert "-DCMAKE_POSITION_INDEPENDENT_CODE=ON" in argv


def test_android_rejects_the_non_abi_arch_spelling() -> None:
    with pytest.raises(render_mod.RenderError) as exc:
        render_mod.render(_load(), "libwebp", "android", "arm64", ndk=_NDK)
    assert "arm64-v8a" in str(exc.value)


def test_render_without_an_explicit_ndk_uses_the_symbolic_default_not_the_environment(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """render() is pure (A2.1): ANDROID_NDK_HOME must not leak into the argv."""
    monkeypatch.setenv("ANDROID_NDK_HOME", "/should/never/appear")
    argv = render_mod.render(_load(), "libwebp", "android", "arm64-v8a")
    joined = " ".join(argv)
    assert "/should/never/appear" not in joined
    assert f"-DCMAKE_TOOLCHAIN_FILE={render_mod.DEFAULT_NDK}/build/cmake/android.toolchain.cmake" in argv


def test_android_default_dist_follows_the_committed_dist_naming_convention() -> None:
    argv = render_mod.render(_load(), "libwebp", "android", "arm64-v8a", ndk=_NDK)
    assert "-DCMAKE_INSTALL_PREFIX=native/third_party/libwebp-dist-android-arm64-v8a" in argv


def test_android_overlay_does_not_leak_into_the_other_platforms() -> None:
    loaded = _load()
    for platform, arch in (("macos", "arm64"), ("linux", "x86_64"), ("windows", "x86_64")):
        joined = " ".join(render_mod.render(loaded, "libwebp", platform, arch))
        assert "ANDROID_ABI" not in joined
        assert "android.toolchain.cmake" not in joined


# ---------------------------------------------------------------------------
# CLI


def test_cli_dry_run_prints_the_android_configure_argv(capsys) -> None:
    rc = build_deps.main(
        ["build", "libwebp", "--platform", "android", "--arch", "arm64-v8a",
         "--android-ndk", _NDK, "--dist", "/android/dist", "--dry-run"]
    )
    out = capsys.readouterr().out
    assert rc == 0
    assert f"-DCMAKE_TOOLCHAIN_FILE={_NDK}/build/cmake/android.toolchain.cmake" in out
    assert "-DANDROID_ABI=arm64-v8a" in out
    assert "-DANDROID_PLATFORM=android-24" in out


def test_cli_rejects_arm64_for_android_naming_the_abi_vocabulary(capsys) -> None:
    rc = build_deps.main(
        ["build", "libwebp", "--platform", "android", "--arch", "arm64",
         "--android-ndk", _NDK, "--dist", "/android/dist", "--dry-run"]
    )
    assert rc != 0
    assert "arm64-v8a" in capsys.readouterr().err


def test_cli_requires_the_ndk_for_android(capsys) -> None:
    rc = build_deps.main(
        ["build", "libwebp", "--platform", "android", "--arch", "arm64-v8a",
         "--dist", "/android/dist", "--dry-run"]
    )
    assert rc != 0
    assert "--android-ndk" in capsys.readouterr().err


def test_cli_rejects_auto_arch_for_android(capsys) -> None:
    rc = build_deps.main(
        ["build", "libwebp", "--platform", "android", "--android-ndk", _NDK,
         "--dist", "/android/dist", "--dry-run"]
    )
    assert rc != 0
    assert "arm64-v8a" in capsys.readouterr().err


def test_cli_rejects_the_abi_spelling_on_non_android_platforms(capsys) -> None:
    rc = build_deps.main(
        ["build", "libwebp", "--platform", "linux", "--arch", "arm64-v8a",
         "--dist", "/dist", "--dry-run"]
    )
    assert rc != 0
    assert "only valid with --platform android" in capsys.readouterr().err


def test_cli_rejects_the_ndk_flag_on_non_android_platforms(capsys) -> None:
    rc = build_deps.main(
        ["build", "libwebp", "--platform", "linux", "--arch", "x86_64",
         "--android-ndk", _NDK, "--dist", "/dist", "--dry-run"]
    )
    assert rc != 0
    assert "--android-ndk is only meaningful" in capsys.readouterr().err


def test_detect_platform_never_resolves_to_android() -> None:
    assert build_deps.detect_platform() in ("macos", "linux", "windows")


def test_legacy_component_form_accepts_android(capsys) -> None:
    rc = build_deps.main(
        ["--component", "libwebp", "--platform", "android", "--arch", "arm64-v8a",
         "--android-ndk", _NDK, "--dry-run"]
    )
    out = capsys.readouterr().out
    assert rc == 0
    assert "-DANDROID_ABI=arm64-v8a" in out
