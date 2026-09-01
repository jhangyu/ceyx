"""Regression coverage for the 2026-09-01 push-rejection incident: an NDK
cross-build embeds full debug info by default, so libjxl.a shipped
unstripped at 199 MB and GitHub's pre-receive hook rejected the push
outright (100 MB per-file limit) after a full CI round-trip. Generic fix
(android_dist.strip_archives + a size tripwire in assert_dist), covers any
future EXPECTATIONS row, not just libjxl.

New file (not an edit to the shared test_android_dist.py or
test_android_dist_libjxl.py) since this concern is orthogonal to any one
component's row.
"""
from __future__ import annotations

import stat
import sys
from pathlib import Path
from unittest import mock

import pytest

pytestmark = pytest.mark.skipif(
    sys.platform == "win32",
    reason="fake NDK tools here are POSIX shebang scripts (chmod +x); "
    "Windows cannot execve them (WinError 193). Android-only concern anyway.",
)

_SCRIPTS = Path(__file__).resolve().parents[2]  # native/scripts
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from deps import android_dist  # noqa: E402


def _fake_ndk(tmp_path: Path) -> Path:
    ndk = tmp_path / "ndk"
    bindir = ndk / "toolchains" / "llvm" / "prebuilt" / "darwin-x86_64" / "bin"
    bindir.mkdir(parents=True)
    for tool in ("llvm-nm", "llvm-readelf", "llvm-strip"):
        (bindir / tool).write_text("", encoding="utf-8")
    return ndk


# ---------------------------------------------------------------------------
# strip_archives


def test_strip_archives_invokes_llvm_strip_on_every_declared_archive(tmp_path: Path) -> None:
    ndk = _fake_ndk(tmp_path)
    dist = tmp_path / "dist" / "lib"
    dist.mkdir(parents=True)
    for rel in android_dist.EXPECTATIONS["libjxl"]["archives"]:
        (tmp_path / "dist" / rel).write_bytes(b"fake-archive-bytes")

    calls: list[list[str]] = []

    def _fake_run(argv, **kwargs):
        calls.append(list(argv))
        import subprocess

        return subprocess.CompletedProcess(list(argv), 0, "", "")

    with mock.patch.object(android_dist, "run", _fake_run):
        stripped = android_dist.strip_archives(tmp_path / "dist", "libjxl", ndk)

    assert len(stripped) == 7  # all seven libjxl archives
    assert len(calls) == 7
    for argv in calls:
        assert argv[0].endswith("llvm-strip")
        assert "--strip-debug" in argv


def test_strip_archives_skips_a_missing_archive_without_erroring(tmp_path: Path) -> None:
    """assert_dist (run right after) is the one that names a missing
    archive as a hard failure; strip_archives must not pre-empt that with a
    different, less specific error."""
    ndk = _fake_ndk(tmp_path)
    dist = tmp_path / "dist"
    dist.mkdir()
    # No archives created at all.
    calls: list[list[str]] = []

    def _fake_run(argv, **kwargs):
        calls.append(list(argv))
        import subprocess

        return subprocess.CompletedProcess(list(argv), 0, "", "")

    with mock.patch.object(android_dist, "run", _fake_run):
        stripped = android_dist.strip_archives(dist, "libjxl", ndk)

    assert stripped == []
    assert calls == []


def test_strip_archives_refuses_an_undeclared_component(tmp_path: Path) -> None:
    with pytest.raises(android_dist.AndroidDistError):
        android_dist.strip_archives(tmp_path, "kvazaar", _fake_ndk(tmp_path))


# ---------------------------------------------------------------------------
# size tripwire


def _fake_ndk_with_working_tools(tmp_path: Path, symbols_text: str) -> Path:
    ndk = tmp_path / "ndk"
    bindir = ndk / "toolchains" / "llvm" / "prebuilt" / "darwin-x86_64" / "bin"
    bindir.mkdir(parents=True)
    nm = bindir / "llvm-nm"
    nm.write_text(f"#!{sys.executable}\nprint({symbols_text!r})\n", encoding="utf-8")
    nm.chmod(nm.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    readelf = bindir / "llvm-readelf"
    readelf.write_text(f"#!{sys.executable}\nprint('Machine: AArch64')\n", encoding="utf-8")
    readelf.chmod(readelf.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return ndk


def test_assert_dist_rejects_an_oversize_archive_naming_the_github_limit(tmp_path: Path) -> None:
    from deps import fetch_libjxl as fetch_libjxl_mod

    symbols = "\n".join(fetch_libjxl_mod.REQUIRED_SYMBOLS)
    ndk = _fake_ndk_with_working_tools(tmp_path, symbols)
    dist = tmp_path / "dist"
    (dist / "lib").mkdir(parents=True)
    # Sparse file: cheap to create, still reports the real size via stat().
    oversize = dist / "lib" / "libjxl.a"
    with open(oversize, "wb") as fh:
        fh.truncate(96 * 1024 * 1024)  # 96 MB >= the 95 MB tripwire
    with pytest.raises(android_dist.AndroidDistError) as exc:
        android_dist.assert_dist("libjxl", dist, ndk, "arm64-v8a")
    assert "libjxl.a" in str(exc.value)
    assert "95" in str(exc.value)
    assert "100" in str(exc.value)


def test_assert_dist_accepts_an_archive_just_under_the_tripwire(tmp_path: Path) -> None:
    from deps import fetch_libjxl as fetch_libjxl_mod

    symbols = "\n".join(fetch_libjxl_mod.REQUIRED_SYMBOLS + ("JxlGetDefaultCms",))
    ndk = _fake_ndk_with_working_tools(tmp_path, symbols)
    dist = tmp_path / "dist"
    for rel in android_dist.EXPECTATIONS["libjxl"]["archives"]:
        p = dist / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"tiny")  # well under the tripwire
    for rel in android_dist.EXPECTATIONS["libjxl"]["headers"]:
        p = dist / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("", encoding="utf-8")
    for name in ("libjxl", "highway", "brotli", "skcms"):
        d = dist / "share" / "licenses" / name
        d.mkdir(parents=True)
        (d / "LICENSE").write_text("x", encoding="utf-8")
    # Must not raise on size grounds (symbol/machine checks still run and pass).
    android_dist.assert_dist("libjxl", dist, ndk, "arm64-v8a")
