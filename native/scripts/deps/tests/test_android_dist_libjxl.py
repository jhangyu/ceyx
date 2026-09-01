"""A-T3: libjxl's android_dist.py EXPECTATIONS row.

Companion to test_android_dist.py (which covers libwebp + the shared
ELF-helper machinery). New file, not an edit to the shared one, per the
android-codec-team shared-file protocol.

Verified against CI run 33457380294 (jxl_dist_android.yml): libjxl 0.12.0
built and installed fine; the assertion layer refused with "no android dist
expectations declared for 'libjxl'" -- this file adds that row.
"""
from __future__ import annotations

import stat
import sys
from pathlib import Path

import pytest

_SCRIPTS = Path(__file__).resolve().parents[2]  # native/scripts
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from deps import android_dist  # noqa: E402
from deps import fetch_libjxl as fetch_libjxl_mod  # noqa: E402


def test_libjxl_expectations_cover_all_seven_required_archives() -> None:
    spec = android_dist.EXPECTATIONS["libjxl"]
    assert set(spec["archives"]) == {
        "lib/libjxl.a", "lib/libjxl_cms.a", "lib/libjxl_threads.a", "lib/libhwy.a",
        "lib/libbrotlicommon.a", "lib/libbrotlidec.a", "lib/libbrotlienc.a",
    }


def test_libjxl_capability_symbols_match_the_desktop_carrier_verbatim() -> None:
    """Imported, not re-picked: android and desktop must assert the same
    capability set for the identical pinned source (fetch_libjxl.py)."""
    spec = android_dist.EXPECTATIONS["libjxl"]
    assert spec["archives"]["lib/libjxl.a"] == list(fetch_libjxl_mod.REQUIRED_SYMBOLS)


def test_libjxl_cms_link_order_trap_is_asserted() -> None:
    spec = android_dist.EXPECTATIONS["libjxl"]
    assert "JxlGetDefaultCms" in spec["archives"]["lib/libjxl_cms.a"]


def test_libjxl_headers_and_licence_dir_and_machine_probe() -> None:
    spec = android_dist.EXPECTATIONS["libjxl"]
    assert "include/jxl/encode.h" in spec["headers"]
    assert "include/jxl/decode.h" in spec["headers"]
    assert spec["licence_dir"] == "share/licenses/libjxl"
    assert spec["machine_probe"] == "lib/libjxl.a"


def _fake_ndk_with_symbol_tool(tmp_path: Path, symbols_text: str) -> Path:
    """A fake NDK whose llvm-nm prints ``symbols_text`` for any archive --
    good enough to satisfy the archives checked BEFORE the missing one, so
    the assertion under test fails for the reason it names, not for a fake
    tool being unexecutable."""
    ndk = tmp_path / "ndk"
    bindir = ndk / "toolchains" / "llvm" / "prebuilt" / "darwin-x86_64" / "bin"
    bindir.mkdir(parents=True)
    nm = bindir / "llvm-nm"
    nm.write_text(f"#!{sys.executable}\nprint({symbols_text!r})\n", encoding="utf-8")
    nm.chmod(nm.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    (bindir / "llvm-readelf").write_text("", encoding="utf-8")
    return ndk


def test_assert_dist_fails_loudly_when_libjxl_cms_is_missing(tmp_path: Path) -> None:
    symbols = "\n".join(fetch_libjxl_mod.REQUIRED_SYMBOLS)
    ndk = _fake_ndk_with_symbol_tool(tmp_path, symbols)
    dist = tmp_path / "dist"
    (dist / "lib").mkdir(parents=True)
    (dist / "lib" / "libjxl.a").write_bytes(b"")
    with pytest.raises(android_dist.AndroidDistError) as exc:
        android_dist.assert_dist("libjxl", dist, ndk, "arm64-v8a")
    assert "libjxl_cms.a" in str(exc.value)
