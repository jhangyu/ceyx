"""A-T3: libjxl-dist-android-arm64-v8a manifest overlay render checks.

Companion to test_render_android.py (which exercises the carrier's generic
android platform support via libwebp). This file is libjxl-specific: it
checks the [component.libjxl.cmake.android] overlay renders the same NDK
toolchain triple plus libjxl's own static/PIC/skcms flag set, and that the
android overlay does not leak into libjxl's other platforms.

A new file (not an edit to the shared test_render_android.py) per the
android-codec-team shared-file protocol -- three dist members render
android argv for three different components in parallel this round.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

_SCRIPTS = Path(__file__).resolve().parents[2]  # native/scripts
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from deps import manifest as manifest_mod  # noqa: E402
from deps import render as render_mod  # noqa: E402

_NDK = "/opt/test-ndk"


def _load():
    return manifest_mod.load()


def test_libjxl_android_overlay_emits_the_ndk_toolchain_triple() -> None:
    argv = render_mod.render(_load(), "libjxl", "android", "arm64-v8a", ndk=_NDK)
    assert f"-DCMAKE_TOOLCHAIN_FILE={_NDK}/build/cmake/android.toolchain.cmake" in argv
    assert "-DANDROID_ABI=arm64-v8a" in argv
    assert "-DANDROID_PLATFORM=android-24" in argv


def test_libjxl_android_overlay_keeps_the_base_jxl_flags() -> None:
    argv = render_mod.render(_load(), "libjxl", "android", "arm64-v8a", ndk=_NDK)
    assert "-DCMAKE_POSITION_INDEPENDENT_CODE=ON" in argv
    assert "-DBUILD_SHARED_LIBS=OFF" in argv
    assert "-DJPEGXL_ENABLE_SKCMS=ON" in argv
    assert "-DJPEGXL_ENABLE_JNI=OFF" in argv
    assert "-DJPEGXL_FORCE_SYSTEM_HWY=OFF" in argv
    assert "-DJPEGXL_FORCE_SYSTEM_BROTLI=OFF" in argv


def test_libjxl_android_default_dist_follows_the_committed_dist_naming_convention() -> None:
    argv = render_mod.render(_load(), "libjxl", "android", "arm64-v8a", ndk=_NDK)
    assert "-DCMAKE_INSTALL_PREFIX=native/third_party/libjxl-dist-android-arm64-v8a" in argv


def test_libjxl_android_rejects_the_non_abi_arch_spelling() -> None:
    with pytest.raises(render_mod.RenderError) as exc:
        render_mod.render(_load(), "libjxl", "android", "arm64", ndk=_NDK)
    assert "arm64-v8a" in str(exc.value)


def test_libjxl_android_overlay_does_not_leak_into_the_other_platforms() -> None:
    loaded = _load()
    for platform, arch in (("macos", "arm64"), ("linux", "x86_64"), ("windows", "x86_64")):
        joined = " ".join(render_mod.render(loaded, "libjxl", platform, arch))
        assert "ANDROID_ABI" not in joined
        assert "android.toolchain.cmake" not in joined
