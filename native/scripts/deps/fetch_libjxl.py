"""Fetch/build the macOS/Linux static libjxl distribution -- Python port of
``native/scripts/fetch_libjxl_dist.sh``.

Migration ruling (2026-09-01, contract item 11 / ENTRY-POINT RULE,
``docs/logs/2026-09-01/contract-windows-codec-round.md``). Exposed as
``build_deps.py fetch libjxl``.

Reuses ``JXL_TAG`` / ``JXL_REPO_URL`` / ``JXL_NEEDED_SUBMODULES`` from
``deps.win_jxl_dist`` (the Windows carrier, owned by another team member
this round -- IMPORTED here, never edited) instead of re-declaring them, so
the two platforms' pins can never silently drift apart. See that module's
docstring for the pin-mechanism rationale (commit-SHA + submodule status,
not a tarball SHA-256): it is identical on this platform, only the archive
suffix (``.a`` vs ``.lib``) and toolchain differ.

Every flag/lib name below is TRANSCRIBED VERBATIM from
``fetch_libjxl_dist.sh`` -- a divergence found is a real bug, not a style
choice.

NO SHELL, ANYWHERE: external tools run through :mod:`deps.run` as argv
lists with ``shell=False``. Symbol proof is capture-then-match (``nm -g``
into a file, then matched in Python), never ``nm | grep -q``: under ``set
-euo pipefail`` a matcher that exits at its first hit kills the
still-writing producer with SIGPIPE, and the pipeline reports failure
precisely BECAUSE the symbol was found (2026-08-28).
"""
from __future__ import annotations

import os
import platform as platform_module
import shutil
import sys
from pathlib import Path
from typing import Optional

try:  # pragma: no cover - import style depends on how the caller invokes us
    from . import win_jxl_dist
    from .run import SubprocessError, run
except ImportError:  # pragma: no cover - fallback for direct script execution
    import win_jxl_dist  # type: ignore[no-redef]
    from run import SubprocessError, run  # type: ignore[no-redef]

# Single source of truth shared with the Windows carrier (imported, not
# re-declared -- see module docstring).
JXL_TAG = win_jxl_dist.JXL_TAG
JXL_REPO_URL = win_jxl_dist.JXL_REPO_URL
JXL_NEEDED_SUBMODULES = win_jxl_dist.JXL_NEEDED_SUBMODULES

# libjxl_cms.a is required, not optional: libjxl.a references
# JxlGetDefaultCms (undefined `U` in libjxl.a, defined `T` only in
# libjxl_cms.a) -- an encode/decode consumer linking libjxl.a without
# libjxl_cms.a fails at link time (fetch_libjxl_dist.sh:46-49).
REQUIRED_LIBS = (
    "libjxl.a",
    "libjxl_cms.a",
    "libjxl_threads.a",
    "libhwy.a",
    "libbrotlicommon.a",
    "libbrotlidec.a",
    "libbrotlienc.a",
)
REQUIRED_SYMBOLS = ("JxlEncoderProcessOutput", "JxlDecoderProcessInput", "JxlEncoderAddBox")

_CMAKE_ARGS_BASE = (
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
    "-DBUILD_SHARED_LIBS=OFF",
    "-DBUILD_TESTING=OFF",
    "-DJPEGXL_ENABLE_TOOLS=OFF",
    "-DJPEGXL_ENABLE_BENCHMARK=OFF",
    "-DJPEGXL_ENABLE_EXAMPLES=OFF",
    "-DJPEGXL_ENABLE_FUZZERS=OFF",
    "-DJPEGXL_ENABLE_DOXYGEN=OFF",
    "-DJPEGXL_ENABLE_MANPAGES=OFF",
    "-DJPEGXL_ENABLE_SJPEG=OFF",
    "-DJPEGXL_ENABLE_OPENEXR=OFF",
    "-DJPEGXL_ENABLE_SKCMS=ON",
    "-DJPEGXL_ENABLE_JNI=OFF",
    "-DJPEGXL_FORCE_SYSTEM_BROTLI=OFF",
    "-DJPEGXL_FORCE_SYSTEM_HWY=OFF",
)

_LICENSE_DIRS = ("libjxl", "highway", "brotli", "skcms")


class JxlFetchError(RuntimeError):
    """Raised when any stage of the macOS/Linux libjxl assembly fails."""


def _fail(message: str) -> JxlFetchError:
    return JxlFetchError(f"[jxl] FAILED: {message}")


def _log(message: str) -> None:
    print(f"[jxl] {message}")


def resolve_arch(host_machine: Optional[str] = None) -> str:
    """``${CEYX_JXL_ARCH:-$(uname -m)}`` -- CEYX_JXL_ARCH lets
    macos_build.yml's cross-arch matrix request the OTHER arch's dist on a
    single-arch runner (mirrors DNG_HEIF_ARCH/fetch_heif_deps.sh)."""
    return os.environ.get("CEYX_JXL_ARCH") or (host_machine or platform_module.machine())


def resolve_dist(native_dir: Path, arch: str, host_arch: str) -> Path:
    if arch == host_arch:
        return native_dir / "third_party" / "libjxl-dist"
    return native_dir / "third_party" / f"libjxl-dist-{arch}"


def cmake_configure_args(dist: Path, *, arch: str, host_os: str) -> list:
    args = [f"-DCMAKE_INSTALL_PREFIX={dist}", *_CMAKE_ARGS_BASE]
    if host_os == "Darwin":
        args += ["-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0", f"-DCMAKE_OSX_ARCHITECTURES={arch}"]
    return args


def clone_source(stage: Path) -> Path:
    src = Path(stage) / "libjxl"
    if (src / ".git").is_dir():
        return src
    if src.exists():
        shutil.rmtree(src)
    _log(f"cloning {JXL_REPO_URL} @ {JXL_TAG} (submodules: {', '.join(JXL_NEEDED_SUBMODULES)})")
    run(["git", "clone", "--depth", "1", "--branch", JXL_TAG, JXL_REPO_URL, str(src)])
    run(["git", "-C", str(src), "submodule", "update", "--init", "--depth", "1", "--", *JXL_NEEDED_SUBMODULES])
    return src


def git_rev_parse_head(src: Path) -> str:
    return run(["git", "-C", str(src), "rev-parse", "HEAD"]).stdout.strip()


def git_submodule_status(src: Path) -> str:
    argv = ["git", "-C", str(src), "submodule", "status", "--", *JXL_NEEDED_SUBMODULES]
    return run(argv).stdout.strip()


def compute_want_pins(commit: str, submodule_status: str, *, arch: str) -> str:
    """Byte-compatible with fetch_libjxl_dist.sh's ``.pins`` stamp shape
    (``WANT_PINS="tag=... commit=... arch=...\\n${SUBMODULE_STATUS}"``)."""
    return f"tag={JXL_TAG} commit={commit} arch={arch}\n{submodule_status}"


def want_pins(src: Path, *, arch: str) -> str:
    return compute_want_pins(git_rev_parse_head(src), git_submodule_status(src), arch=arch)


def stamp_is_current(dist: Path, want: str) -> bool:
    dist = Path(dist)
    stamp = dist / ".pins"
    if not stamp.is_file() or stamp.read_text(encoding="utf-8") != want:
        return False
    return all((dist / "lib" / lib).is_file() for lib in REQUIRED_LIBS)


def configure_build_install(src: Path, dist: Path, build_dir: Path, *, arch: str, host_os: str) -> None:
    if Path(build_dir).exists():
        shutil.rmtree(build_dir)
    argv = ["cmake", "-S", str(src), "-B", str(build_dir), *cmake_configure_args(Path(dist), arch=arch, host_os=host_os)]
    run(argv, capture_output=False)
    run(["cmake", "--build", str(build_dir), "--parallel"], capture_output=False)
    run(["cmake", "--install", str(build_dir)], capture_output=False)


def assert_static_libs(dist: Path) -> None:
    dist = Path(dist)
    missing = [lib for lib in REQUIRED_LIBS if not (dist / "lib" / lib).is_file()]
    if missing:
        listing = "\n".join(sorted(str(p.relative_to(dist)) for p in dist.rglob("*.a")))
        raise _fail(f"dist is missing: {' '.join(missing)}\ninstalled archives:\n{listing}")


def _read_nm_symbols(lib_path: Path, out_path: Path) -> str:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        result = run(["nm", "-g", str(lib_path)], check=False)
    except (OSError, SubprocessError) as exc:
        raise _fail(f"could not read symbols of {lib_path} (nm not runnable: {exc})") from exc
    text = (result.stdout or "") + (result.stderr or "")
    out_path.write_text(text, encoding="utf-8")
    return text


def assert_symbols(dist: Path) -> None:
    dist = Path(dist)
    text = _read_nm_symbols(dist / "lib" / "libjxl.a", dist / ".stage" / "nm-libjxl.txt")
    for symbol in REQUIRED_SYMBOLS:
        if symbol not in text:
            raise _fail(f"{symbol} not found in libjxl.a")
        _log(f"ok {symbol}")


def strip_archives(dist: Path, host_os: str) -> None:
    """Release binaries ship no debug info (ruling Q5) -- strip local/debug
    symbols from every installed archive."""
    dist = Path(dist)
    strip_cmd = ["strip", "-S"] if host_os == "Darwin" else ["strip", "--strip-debug"]
    for archive in sorted((dist / "lib").glob("*.a")):
        run(strip_cmd + [str(archive)], check=False)


def vendor_licenses(src: Path, dist: Path) -> None:
    dist = Path(dist)
    pairs = {
        "libjxl": src,
        "highway": src / "third_party" / "highway",
        "brotli": src / "third_party" / "brotli",
        "skcms": src / "third_party" / "skcms",
    }
    assert tuple(pairs) == _LICENSE_DIRS
    for name, source_dir in pairs.items():
        dest = dist / "share" / "licenses" / name
        dest.mkdir(parents=True, exist_ok=True)
        found = []
        if Path(source_dir).is_dir():
            for pattern in ("LICENSE*", "COPYING*"):
                found.extend(sorted(Path(source_dir).glob(pattern)))
        for path in found:
            if path.is_file():
                shutil.copy2(path, dest / path.name)
        if not any(dest.iterdir()):
            raise _fail(f"no licence file found for {name} under {source_dir}")


def build(dist: Optional[Path] = None, *, arch: Optional[str] = None, force: bool = False) -> Path:
    """Build the whole macOS/Linux static libjxl dist. Mirrors
    fetch_libjxl_dist.sh's ordering: clone -> compute pins -> skip if
    current -> configure/build/install -> assert -> strip -> vendor
    licenses -> stamp."""
    native_dir = Path(__file__).resolve().parents[2]
    host_os = platform_module.system()
    host_arch = platform_module.machine()
    arch = arch if arch is not None else resolve_arch(host_arch)
    dist = Path(dist) if dist is not None else resolve_dist(native_dir, arch, host_arch)
    stage = dist / ".stage"
    stage.mkdir(parents=True, exist_ok=True)

    src = clone_source(stage)
    want = want_pins(src, arch=arch)

    if not force and stamp_is_current(dist, want):
        _log(f"dist already at the pinned commit: {want.splitlines()[0]}")
        return dist

    _log(f"pinned commit for tag {JXL_TAG}")
    _log(want)

    configure_build_install(src, dist, stage / "build", arch=arch, host_os=host_os)

    assert_static_libs(dist)
    assert_symbols(dist)
    strip_archives(dist, host_os)
    vendor_licenses(src, dist)

    (dist / ".pins").write_text(want, encoding="utf-8")
    shutil.rmtree(stage, ignore_errors=True)
    _log(f"dist ready at {dist}")
    return dist


def main(argv: Optional[list] = None) -> int:
    import argparse

    parser = argparse.ArgumentParser(prog="deps.fetch_libjxl")
    parser.add_argument("--dist", default=None)
    parser.add_argument("--arch", default=None)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)
    try:
        build(Path(args.dist) if args.dist else None, arch=args.arch, force=args.force)
    except (JxlFetchError, SubprocessError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
