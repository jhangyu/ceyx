"""Windows libwebp dist assembly -- the Python port of
``native/scripts/build_libwebp_dist_windows.sh``.

Migration ruling (2026-09-01, contract item 11 / ENTRY-POINT RULE,
``docs/logs/2026-09-01/contract-windows-codec-round.md``): the Windows
libwebp dist build MIGRATES into this carrier module, exposed as
``build_deps.py build webp-stack``. ``build_libwebp_dist_windows.sh`` stays
in the tree until the carrier-built dist is proven green on a Windows
runner (round 3) -- it is NOT deleted by this change; this round's proof is
the transcription tests below plus ``webp_dist_windows.yml``'s rewire to
call this module, not a local run (this host cannot build a Windows PE).

WEBP_VERSION below MUST equal ``native/vcpkg/vcpkg.json``'s libwebp override
("version": "1.6.0") -- see ``build_libwebp_dist_windows.sh``'s own header
comment for why: that manifest is what macOS/Linux resolve libwebp from,
and a Windows dist at a different version would be a silent behavioural
fork nothing would catch.

Every pin/flag/required-file value below is TRANSCRIBED VERBATIM from
``build_libwebp_dist_windows.sh`` -- a divergence found here is a real bug
in one of the two copies, not a style choice, and must be reported rather
than silently reconciled.

NO SHELL, ANYWHERE: every external tool runs through ``deps/run.py`` as an
argv list with ``shell=False``. Symbol proof is capture-then-match, never
``llvm-nm | grep``: under ``set -euo pipefail`` a matcher that exits at its
first hit kills the still-writing producer with SIGPIPE, and the pipeline
reports failure precisely BECAUSE the symbol was found (2026-08-28) --
``run.py`` makes this structurally impossible by rejecting a pipe character
in any argv element outright.
"""
from __future__ import annotations

import shutil
import sys
from pathlib import Path
from typing import Optional

try:  # pragma: no cover - import style depends on how the caller invokes us
    from . import win_pe
    from .fetch import fetch_tarball
    from .run import SubprocessError, run
except ImportError:  # pragma: no cover - fallback for direct script execution
    import win_pe  # type: ignore[no-redef]
    from fetch import fetch_tarball  # type: ignore[no-redef]
    from run import SubprocessError, run  # type: ignore[no-redef]

PLATFORM = "windows"

WEBP_VERSION = "1.6.0"
WEBP_URL = f"https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-{WEBP_VERSION}.tar.gz"
WEBP_SHA256 = "e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564"

# Windows archive names (clang-cl + MSVC-style .lib). CMake keeps the "lib"
# prefix on the archive names even under clang-cl + Ninja
# (CMAKE_STATIC_LIBRARY_PREFIX is derived from the compiler ID -- Clang, not
# MSVC's cl.exe -- so it defaults to "lib" here), unlike a pure MSVC build
# which would install webp.lib -- observed for real in run 33307093239.
REQUIRED_LIBS = ("libwebp.lib", "libwebpmux.lib", "libwebpdemux.lib", "libsharpyuv.lib")
REQUIRED_HEADERS = ("include/webp/encode.h", "include/webp/decode.h", "include/webp/mux.h")

REQUIRED_ENCODER_SYMBOLS = ("WebPEncodeRGBA", "WebPEncodeLosslessRGBA")
REQUIRED_MUX_SYMBOLS = ("WebPMuxSetChunk", "WebPMuxAssemble")

# CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded is mandatory for the same reason
# as every other Windows dist in this project: a /MD archive linked into the
# /MT decoder DLL fails as duplicate symbols or heap corruption, never as a
# clean configure error.
#
# WEBP_BUILD_* flags below disable the command-line TOOLS only. The mux and
# demux LIBRARIES are built regardless -- that distinction is the whole
# reason WebP metadata support needs no new dependency.
_CMAKE_ARGS_BASE = (
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_COMPILER=clang-cl",
    "-DCMAKE_CXX_COMPILER=clang-cl",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
    "-DBUILD_SHARED_LIBS=OFF",
    "-DWEBP_BUILD_ANIM_UTILS=OFF",
    "-DWEBP_BUILD_CWEBP=OFF",
    "-DWEBP_BUILD_DWEBP=OFF",
    "-DWEBP_BUILD_GIF2WEBP=OFF",
    "-DWEBP_BUILD_IMG2WEBP=OFF",
    "-DWEBP_BUILD_VWEBP=OFF",
    "-DWEBP_BUILD_WEBPINFO=OFF",
    "-DWEBP_BUILD_WEBPMUX=OFF",
    "-DWEBP_BUILD_EXTRAS=OFF",
)


class WindowsWebpError(RuntimeError):
    """Raised when any stage of the Windows libwebp assembly fails. The
    message always names the artefact and the concrete missing fact,
    mirroring the shell script's ``[webp-win] FAILED:`` lines."""


def _fail(message: str) -> WindowsWebpError:
    return WindowsWebpError(f"[webp-win] FAILED: {message}")


def _log(message: str) -> None:
    print(f"[webp-win] {message}")


def compute_want_pins() -> str:
    """Byte-compatible with a ``.pins`` file written by
    build_libwebp_dist_windows.sh."""
    return f"libwebp={WEBP_VERSION}:{WEBP_SHA256} platform=windows-x86_64 archives=webp+mux+demux+sharpyuv"


def stamp_is_current(dist: Path, want: str) -> bool:
    dist = Path(dist)
    stamp = dist / ".pins"
    if not stamp.is_file() or stamp.read_text(encoding="utf-8") != want:
        return False
    return (dist / "lib" / "libwebp.lib").is_file() and (dist / "lib" / "libwebpmux.lib").is_file()


def cmake_configure_args(dist: Path) -> list:
    """Pure function (no subprocess call) so the exact argv is unit-testable
    without a Windows toolchain."""
    return [f"-DCMAKE_INSTALL_PREFIX={dist}", *_CMAKE_ARGS_BASE]


def fetch_source(stage: Path) -> Path:
    """Download+verify the libwebp release tarball and extract it into
    ``stage``. Returns the extracted source directory."""
    stage = Path(stage)
    stage.mkdir(parents=True, exist_ok=True)
    tarball = stage / f"libwebp-{WEBP_VERSION}.tar.gz"
    if tarball.exists():
        try:
            from .fetch import verify_sha256
        except ImportError:  # pragma: no cover
            from fetch import verify_sha256  # type: ignore[no-redef]
        try:
            verify_sha256(tarball, WEBP_SHA256)
        except Exception:  # noqa: BLE001 - any verification failure means re-download
            tarball.unlink(missing_ok=True)
    if not tarball.exists():
        _log(f"downloading {WEBP_URL}")
        fetch_tarball(WEBP_URL, WEBP_SHA256, tarball)
    _log(f"verified libwebp-{WEBP_VERSION}.tar.gz {WEBP_SHA256}")

    src_dir = stage / f"libwebp-{WEBP_VERSION}"
    if src_dir.exists():
        shutil.rmtree(src_dir)
    import tarfile

    with tarfile.open(tarball, "r:gz") as tf:
        try:
            tf.extractall(stage, filter="data")  # noqa: S202 - hash-pinned archive
        except TypeError:
            tf.extractall(stage)  # noqa: S202 - pre-PEP-706 interpreter
    if not src_dir.is_dir():
        raise _fail(f"{tarball.name} did not extract to {src_dir} (unexpected archive layout)")
    return src_dir


def configure_build_install(src: Path, dist: Path, build_dir: Path) -> None:
    if Path(build_dir).exists():
        shutil.rmtree(build_dir)
    argv = ["cmake", "-S", str(src), "-B", str(build_dir), "-G", "Ninja", *cmake_configure_args(Path(dist))]
    run(argv)
    run(["cmake", "--build", str(build_dir), "--parallel"])
    run(["cmake", "--install", str(build_dir)])


def assert_layout(dist: Path) -> None:
    """Layout proof: assert each required file by name, where the message
    names the missing file, rather than as an opaque find_library() failure
    inside the decoder's configure much later."""
    dist = Path(dist)
    missing = [
        required
        for required in (*(f"lib/{lib}" for lib in REQUIRED_LIBS), *REQUIRED_HEADERS)
        if not (dist / required).is_file()
    ]
    if missing:
        listing = "\n".join(sorted(str(p.relative_to(dist)) for p in dist.rglob("*") if p.is_file()))
        raise _fail(f"dist is missing: {' '.join(missing)}\ncomplete listing of what WAS installed:\n{listing}")


def _read_nm_symbols(lib_path: Path, out_path: Path) -> str:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        result = run(["llvm-nm", "--defined-only", str(lib_path)], check=False)
    except (OSError, SubprocessError) as exc:
        raise win_pe.PeInspectionError(
            f"could not read symbols of {lib_path} (llvm-nm: not runnable ({exc})) -- "
            "the measurement did NOT happen; this is an instrument failure, not a "
            "capability failure"
        ) from exc
    text = (result.stdout or "") + (result.stderr or "")
    out_path.write_text(text, encoding="utf-8")
    if result.returncode != 0:
        raise win_pe.PeInspectionError(
            f"could not read symbols of {lib_path} (llvm-nm exit {result.returncode}) -- "
            "the measurement did NOT happen; this is an instrument failure, not a "
            "capability failure"
        )
    return text


def assert_symbols(dist: Path) -> None:
    dist = Path(dist)
    enc_text = _read_nm_symbols(dist / "lib" / "libwebp.lib", dist / ".webp_enc_syms.txt")
    for symbol in REQUIRED_ENCODER_SYMBOLS:
        if not win_pe.symbol_present(enc_text, symbol):
            raise win_pe.PeAssertionFailed(f"{symbol} absent from libwebp.lib")
        _log(f"ASSERT encoder {symbol} OK")

    mux_text = _read_nm_symbols(dist / "lib" / "libwebpmux.lib", dist / ".webp_mux_syms.txt")
    for symbol in REQUIRED_MUX_SYMBOLS:
        if not win_pe.symbol_present(mux_text, symbol):
            raise win_pe.PeAssertionFailed(f"{symbol} absent from libwebpmux.lib")
        _log(f"ASSERT mux {symbol} OK")


def vendor_license(src: Path, dist: Path) -> None:
    dist = Path(dist)
    dest = dist / "share" / "licenses" / "libwebp"
    dest.mkdir(parents=True, exist_ok=True)
    licence = Path(src) / "COPYING"
    if not licence.is_file():
        raise _fail(f"no COPYING file found under {src}")
    shutil.copy2(licence, dest / "COPYING")


def build(dist: Path, *, stage: Optional[Path] = None, force: bool = False) -> Path:
    """Build the whole Windows static libwebp dist into ``dist``."""
    dist = Path(dist)
    stage = Path(stage) if stage is not None else dist / ".stage"

    want = compute_want_pins()
    if not force and stamp_is_current(dist, want):
        _log(f"dist already at the pinned version {WEBP_VERSION}")
        return dist

    src = fetch_source(stage)
    configure_build_install(src, dist, stage / "build")

    assert_layout(dist)
    assert_symbols(dist)
    vendor_license(src, dist)

    (dist / ".pins").write_text(want, encoding="utf-8")
    shutil.rmtree(stage, ignore_errors=True)
    _log(f"dist ready at {dist}")
    return dist


def main(argv: Optional[list] = None) -> int:
    """Module entry point: ``python -m deps.win_webp_dist --dist <dir>``.

    The frozen team contract (per the ENTRY-POINT RULE) is
    ``build_deps.py build webp-stack --platform windows --arch x86_64
    --dist <dir>``, which is the form ``webp_dist_windows.yml`` invokes.
    """
    import argparse

    from .run import assert_native_windows_interpreter

    assert_native_windows_interpreter()

    parser = argparse.ArgumentParser(prog="deps.win_webp_dist")
    parser.add_argument("--dist", required=True)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)

    try:
        build(Path(args.dist), force=args.force)
    except (WindowsWebpError, win_pe.PeInspectionError, win_pe.PeAssertionFailed) as exc:
        print(f"[webp-win] {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
