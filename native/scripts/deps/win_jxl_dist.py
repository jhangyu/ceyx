"""Windows libjxl dist assembly -- the Python port of
``native/scripts/build_libjxl_dist_windows.sh``.

Migration ruling (2026-09-01, contract item 10 / ENTRY-POINT RULE,
``docs/logs/2026-09-01/contract-windows-codec-round.md``): the Windows
libjxl dist build MIGRATES into this carrier module, exposed as
``build_deps.py build jxl-stack``. ``build_libjxl_dist_windows.sh`` was
retired (round 2, task #6) once the carrier-built dist was committed and
green; its transcription test is frozen in ``win_jxl_dist_test.py``.

UNLIKE ``win_heif_dist.py``, this module does NOT go through
``manifest.toml`` / ``deps/execute.py``. ``[component.libjxl]`` in the
manifest explicitly has no ``source.windows`` override (manifest.toml:571-574:
"fetch_libjxl_dist.sh has no Windows branch ... nothing live to transcribe
for Windows") -- the acquisition mechanism this module ports is a
self-contained git-clone-at-tag recipe that was never manifest-driven on
ANY platform (see manifest.toml:558-570: the pin is the git tag itself, not
a tarball SHA-256, because upstream has never published a source tarball
that includes the ``third_party/highway``/``third_party/brotli`` submodules
the static build needs). Every pin, flag and required-library name below is
TRANSCRIBED VERBATIM from ``build_libjxl_dist_windows.sh`` -- do not "clean
up" a value found to differ; that is a real bug in one of the two copies,
not a style choice, and must be reported rather than silently reconciled.

KNOWN RISK, carried over unedited from the shell script: libjxl's build is
the heaviest of the three Windows dists and its highway dependency has
historically been the most compiler-sensitive part. If clang-cl cannot
build it within the workflow timeout, the answer is NOT to disable features
(no ``-DJPEGXL_ENABLE_SKCMS=OFF``, no reduced highway target set) -- that
produces a library that loads and then encodes nothing, which is strictly
worse than an honest failure. Stop and report instead.

NO SHELL, ANYWHERE: every external tool runs through ``deps/run.py`` as an
argv list with ``shell=False``. Symbol proof is capture-then-match, never
``llvm-nm | grep``: under ``set -euo pipefail`` a matcher that exits at its
first hit kills the still-writing producer with SIGPIPE, and the pipeline
reports failure precisely BECAUSE the symbol was found (observed for real,
2026-08-28) -- ``run.py`` makes this structurally impossible by rejecting a
pipe character in any argv element outright.
"""
from __future__ import annotations

import shutil
import sys
from pathlib import Path
from typing import Optional

try:  # pragma: no cover - import style depends on how the caller invokes us
    from . import win_pe
    from .run import SubprocessError, run
except ImportError:  # pragma: no cover - fallback for direct script execution
    import win_pe  # type: ignore[no-redef]
    from run import SubprocessError, run  # type: ignore[no-redef]

PLATFORM = "windows"

# MUST equal fetch_libjxl_dist.sh's JXL_TAG (build_libjxl_dist_windows.sh:46
# grep-checks this at shell-script authoring time; here it is asserted by
# win_jxl_dist_test.py reading both files' source text directly).
JXL_TAG = "v0.12.0"
JXL_REPO_URL = "https://github.com/libjxl/libjxl.git"

# Only the submodules the static core library actually links need to be
# fetched -- brotli and highway (required unconditionally by encode/decode
# core) and skcms (required because JPEGXL_ENABLE_SKCMS=ON below).
JXL_NEEDED_SUBMODULES = ("third_party/brotli", "third_party/highway", "third_party/skcms")

# Windows archive names (clang-cl + MSVC-style .lib, not lib*.a). jxl_cms.lib
# is required in addition to jxl.lib itself: JxlGetDefaultCms lives in a
# separate cms archive on every platform (see the cmake args' SKCMS comment).
REQUIRED_LIBS = (
    "jxl.lib",
    "jxl_threads.lib",
    "jxl_cms.lib",
    "hwy.lib",
    "brotlicommon.lib",
    "brotlidec.lib",
    "brotlienc.lib",
)

REQUIRED_SYMBOLS = ("JxlEncoderProcessOutput", "JxlDecoderProcessInput", "JxlEncoderAddBox")

# CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded is mandatory for the same reason as
# every other Windows dist in this project: a /MD archive linked into the /MT
# decoder DLL fails as duplicate symbols or heap corruption, never as a clean
# configure error.
_CMAKE_ARGS_BASE = (
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_COMPILER=clang-cl",
    "-DCMAKE_CXX_COMPILER=clang-cl",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
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

_REQUIRED_HEADER = "include/jxl/encode.h"


class WindowsJxlError(RuntimeError):
    """Raised when any stage of the Windows libjxl assembly fails. The
    message always names the artefact and the concrete missing fact,
    mirroring the shell script's ``[jxl-win] FAILED:`` lines."""


def _fail(message: str) -> "WindowsJxlError":
    return WindowsJxlError(f"[jxl-win] FAILED: {message}")


def _log(message: str) -> None:
    print(f"[jxl-win] {message}")


def cmake_configure_args(dist: Path) -> list:
    """Return the full ``cmake -S <src> -B <build>`` argument list (minus
    ``-S``/``-B``/``-G Ninja``, which the caller supplies), for a dist
    installing into ``dist``. Kept as a pure function (no subprocess call)
    so the exact argv is unit-testable without a Windows toolchain."""
    return [f"-DCMAKE_INSTALL_PREFIX={dist}", *_CMAKE_ARGS_BASE]


def clone_source(stage: Path) -> Path:
    """Clone libjxl at ``JXL_TAG`` into ``stage/libjxl`` with only the
    submodules the static build needs, unless already present.

    Mirrors the shell original: a clone that already exists (``.git``
    present) is reused as-is, never re-cloned or fetched -- the pin
    comparison in :func:`want_pins` is what decides whether a rebuild is
    needed, not the presence of the checkout.
    """
    src = Path(stage) / "libjxl"
    if (src / ".git").is_dir():
        return src
    if src.exists():
        shutil.rmtree(src)
    _log(f"cloning {JXL_REPO_URL} @ {JXL_TAG} (submodules: {', '.join(JXL_NEEDED_SUBMODULES)})")
    run(["git", "clone", "--depth", "1", "--branch", JXL_TAG, JXL_REPO_URL, str(src)])
    run(
        ["git", "-C", str(src), "submodule", "update", "--init", "--depth", "1", "--", *JXL_NEEDED_SUBMODULES]
    )
    return src


def git_rev_parse_head(src: Path) -> str:
    result = run(["git", "-C", str(src), "rev-parse", "HEAD"])
    return result.stdout.strip()


def git_submodule_status(src: Path) -> str:
    argv = ["git", "-C", str(src), "submodule", "status", "--", *JXL_NEEDED_SUBMODULES]
    result = run(argv)
    return result.stdout.strip()


def compute_want_pins(commit: str, submodule_status: str, *, arch: str = "x86_64") -> str:
    """Pure formatting, split out from :func:`want_pins` so the stamp string
    shape is unit-testable without a real git checkout.

    Byte-compatible with a ``.pins`` file written by
    build_libjxl_dist_windows.sh (``WANT_PINS="tag=... commit=... platform=...
    \\n${SUBMODULE_STATUS}"``), so a dist built by either carrier is
    recognised as current by the other.
    """
    return f"tag={JXL_TAG} commit={commit} platform=windows-{arch}\n{submodule_status}"


def want_pins(src: Path, *, arch: str = "x86_64") -> str:
    commit = git_rev_parse_head(src)
    submodule_status = git_submodule_status(src)
    return compute_want_pins(commit, submodule_status, arch=arch)


def stamp_is_current(dist: Path, want: str) -> bool:
    """True when ``.pins`` matches AND every required library is on disk.

    Both halves matter: a stamp alone can outlive a deleted archive, and
    then the "already built" fast path ships nothing.
    """
    dist = Path(dist)
    stamp = dist / ".pins"
    if not stamp.is_file() or stamp.read_text(encoding="utf-8") != want:
        return False
    return all((dist / "lib" / lib).is_file() for lib in REQUIRED_LIBS)


def configure_build_install(src: Path, dist: Path, build_dir: Path) -> None:
    if build_dir.exists():
        shutil.rmtree(build_dir)
    argv = ["cmake", "-S", str(src), "-B", str(build_dir), "-G", "Ninja", *cmake_configure_args(Path(dist))]
    run(argv)
    run(["cmake", "--build", str(build_dir), "--parallel"])
    run(["cmake", "--install", str(build_dir)])


def assert_static_libs(dist: Path) -> None:
    """Every archive in :data:`REQUIRED_LIBS` must exist. Not an export
    check (there is nothing to export from a static archive) -- these are
    build INPUTS a consumer links against, so their presence on disk is the
    whole capability claim.
    """
    dist = Path(dist)
    missing = [lib for lib in REQUIRED_LIBS if not (dist / "lib" / lib).is_file()]
    if missing:
        listing = "\n".join(sorted(str(p.relative_to(dist)) for p in dist.rglob("*") if p.is_file()))
        raise _fail(
            f"dist is missing: {' '.join(missing)}\ncomplete listing of what WAS installed:\n{listing}"
        )
    header = dist / _REQUIRED_HEADER
    if not header.is_file():
        raise _fail(f"{_REQUIRED_HEADER} missing under {dist}")


def _read_nm_symbols(lib_path: Path, out_path: Path) -> str:
    """Capture ``llvm-nm --defined-only``'s complete output to a file and
    return it as text -- never streamed into a matcher (module docstring's
    no-pipe rationale).
    """
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
    """Prove ``lib/jxl.lib`` actually contains the encode+decode entry
    points, not merely that the archive file exists.
    """
    dist = Path(dist)
    text = _read_nm_symbols(dist / "lib" / "jxl.lib", dist / ".nm-jxl.txt")
    for symbol in REQUIRED_SYMBOLS:
        if not win_pe.symbol_present(text, symbol):
            raise win_pe.PeAssertionFailed(
                f"jxl.lib does not contain {symbol!r} -- see the module header comment: "
                "do NOT respond by disabling skcms or highway targets."
            )
        _log(f"ASSERT {symbol} OK")


def vendor_licenses(src: Path, dist: Path) -> None:
    """Copy each dependency's licence file(s) into ``dist/share/licenses/*``,
    asserting at least one was found for each -- an absent licence file is a
    packaging defect, not a thing to silently skip.
    """
    dist = Path(dist)
    pairs = (
        ("libjxl", src),
        ("highway", src / "third_party" / "highway"),
        ("brotli", src / "third_party" / "brotli"),
        ("skcms", src / "third_party" / "skcms"),
    )
    for name, source_dir in pairs:
        dest = dist / "share" / "licenses" / name
        dest.mkdir(parents=True, exist_ok=True)
        found = []
        if source_dir.is_dir():
            for pattern in ("LICENSE*", "COPYING*"):
                found.extend(sorted(source_dir.glob(pattern)))
        for path in found:
            if path.is_file():
                shutil.copy2(path, dest / path.name)
        if not any(dest.iterdir()):
            raise _fail(f"no licence file found for {name} under {source_dir}")


def build(
    dist: Path,
    *,
    arch: str = "x86_64",
    stage: Optional[Path] = None,
    force: bool = False,
) -> Path:
    """Build the whole Windows static libjxl dist into ``dist``.

    Unlike ``win_heif_dist.build``, this takes no ``loaded`` manifest
    argument: the acquisition this module ports was never manifest-driven
    (module docstring). If a caller has a loaded manifest in hand it is
    simply unused here.
    """
    dist = Path(dist)
    stage = Path(stage) if stage is not None else dist / ".stage"
    stage.mkdir(parents=True, exist_ok=True)

    src = clone_source(stage)
    want = want_pins(src, arch=arch)

    if not force and stamp_is_current(dist, want):
        _log("dist already at the pinned commit:")
        _log(f"  {want}")
        return dist

    _log(f"pinned commit for tag {JXL_TAG}")
    _log(want)

    configure_build_install(src, dist, stage / "build")

    assert_static_libs(dist)
    assert_symbols(dist)
    vendor_licenses(src, dist)

    (dist / ".pins").write_text(want, encoding="utf-8")
    shutil.rmtree(stage, ignore_errors=True)
    _log(f"dist ready at {dist}")
    return dist


def main(argv: Optional[list] = None) -> int:
    """Module entry point: ``python -m deps.win_jxl_dist --dist <dir>``.

    The frozen team contract (per the ENTRY-POINT RULE) is
    ``build_deps.py build jxl-stack --platform windows --arch x86_64
    --dist <dir>``, which is the form ``jxl_dist_windows.yml`` invokes. This
    module-level entry point is kept as the same direct-run convenience
    ``win_heif_dist.main`` offers, not as a second contract surface.
    """
    import argparse

    from .run import assert_native_windows_interpreter

    assert_native_windows_interpreter()

    parser = argparse.ArgumentParser(prog="deps.win_jxl_dist")
    parser.add_argument("--dist", required=True, help="install prefix the dist lands in")
    parser.add_argument("--arch", default="x86_64", choices=("x86_64",))
    parser.add_argument("--force", action="store_true", help="rebuild even when .pins is current")
    args = parser.parse_args(argv)

    try:
        build(Path(args.dist), arch=args.arch, force=args.force)
    except (WindowsJxlError, win_pe.PeInspectionError, win_pe.PeAssertionFailed) as exc:
        print(f"[jxl-win] {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
