"""Windows LLVM OpenMP runtime (libomp) dist, built from source with clang-cl.

User ruling R-11 (2026-10-02, supersedes R-10; Halcyon
docs/logs/2026-09-30/armci-contract.md): Microsoft ships the LLVM OpenMP
runtime (``libomp140.<arch>.dll``) only under ``debug_nonredist`` on the
windows-11-arm image, i.e. not redistributable, and vcomp140 (OpenMP 2.0) is
not an option because RawSpeed3 compiles OpenMP 3.0-4.5 constructs into the
decoder (task/taskloop/taskgroup/cancel/atomic read+write --
docs/logs/2026-10-01/winarm/vcomp-compat.txt). So this project builds the
runtime itself, for x86_64 AND arm64, exposed as ``build_deps.py build
libomp-stack`` -- same shape as the libwebp/libjxl Windows dists.

VERSION: LLVM_VERSION is the newest clang-cl on the Windows runners. The two
images differ (dist run 36943142762: windows-11-arm has clang 22.1.8,
windows-latest has clang 20.1.8), and one runtime version serves both: a
runtime at or above the compiler that emits the ``__kmpc_*`` calls is the
supported direction -- the export table is append-only (dllexports keeps
fixed ordinals "to maintain backwards compatible exports order",
openmp/runtime/src/CMakeLists.txt:345). Not measured at run time (CI is
compile-only).
LLVM_SHA256 is the release asset digest recorded by GitHub for
``llvmorg-22.1.8`` and was re-verified by hashing the downloaded file.
LLVM publishes only the monolithic ``llvm-project`` source tarball (no
per-subproject ``openmp-*.src`` tarball since 18.x); only ``openmp/`` and the
shared top-level ``cmake/`` are extracted from it.

NAMING: ``OPENMP_MSVC_NAME_SCHEME=ON`` is LLVM's own option for the
Visual-Studio spelling (openmp/runtime/CMakeLists.txt:406-410): the DLL is
``libomp140.<LIBOMP_ARCH>.dll`` and its import library ``libomp.lib``
(openmp/runtime/src/CMakeLists.txt:321-328) -- exactly the DLL name the
decoder imports today (libomp140.x86_64.dll; libomp140.aarch64.dll on arm64).
``LIBOMP_ARCH`` is passed explicitly rather than detected.

CRT: CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded (static CRT), like every other
Windows dist here. Upstream intends /MT too (openmp/runtime/cmake/
config-ix.cmake:89-96 rewrites /MD to /MT), but that string rewrite is a
no-op under CMP0091=NEW (cmake_minimum_required 3.20), where the runtime is
chosen by CMAKE_MSVC_RUNTIME_LIBRARY -- default /MD -- so it must be set.
``assert_static_crt`` proves it took effect (no VCRUNTIME140 import).

NO SHELL, ANYWHERE: every external tool runs through ``deps/run.py`` as an
argv list with ``shell=False`` (see win_webp_dist.py's docstring).
"""
from __future__ import annotations

import shutil
import sys
import tarfile
from pathlib import Path
from typing import Optional

try:  # pragma: no cover - import style depends on how the caller invokes us
    from . import win_pe
    from .fetch import fetch_tarball, verify_sha256
    from .run import SubprocessError, run
except ImportError:  # pragma: no cover - fallback for direct script execution
    import win_pe  # type: ignore[no-redef]
    from fetch import fetch_tarball, verify_sha256  # type: ignore[no-redef]
    from run import SubprocessError, run  # type: ignore[no-redef]

PLATFORM = "windows"

LLVM_VERSION = "22.1.8"
LLVM_TAG = f"llvmorg-{LLVM_VERSION}"
LLVM_URL = f"https://github.com/llvm/llvm-project/releases/download/{LLVM_TAG}/llvm-project-{LLVM_VERSION}.src.tar.xz"
LLVM_SHA256 = "922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888"

# Only these top-level entries of the monolithic tarball are extracted:
# openmp/ is the project, cmake/ is LLVM_COMMON_CMAKE_UTILS
# (openmp/CMakeLists.txt:4).
_SRC_ROOT = f"llvm-project-{LLVM_VERSION}.src"
_EXTRACT_TOPS = ("openmp", "cmake")

# Canonical arch (native/deps/arch_map.toml) -> LIBOMP_ARCH, which is also the
# DLL-name suffix under OPENMP_MSVC_NAME_SCHEME.
LIBOMP_ARCH = {"x86_64": "x86_64", "arm64": "aarch64"}

IMPORT_LIB = "lib/libomp.lib"
REQUIRED_HEADERS = ("include/omp.h",)
# Entry points the decoder's -fopenmp code generation calls; a runtime missing
# them links but cannot run a parallel region.
REQUIRED_EXPORTS = ("__kmpc_fork_call", "__kmpc_for_static_init_4", "omp_get_num_threads")
# LLVM installs backwards-compat copies (libiomp5md.dll/.lib) unconditionally
# on Windows (openmp/runtime/src/CMakeLists.txt:413-421). Nothing consumes
# them; they are removed so the committed dist holds only what ships.
_ALIAS_FILES = ("bin/libiomp5md.dll", "lib/libiomp5md.lib")

_CMAKE_ARGS_BASE = (
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_COMPILER=clang-cl",
    "-DCMAKE_CXX_COMPILER=clang-cl",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
    "-DOPENMP_MSVC_NAME_SCHEME=ON",
)


class WindowsLibompError(RuntimeError):
    """Raised when any stage of the Windows libomp assembly fails."""


def _fail(message: str) -> WindowsLibompError:
    return WindowsLibompError(f"[libomp-win] FAILED: {message}")


def _log(message: str) -> None:
    print(f"[libomp-win] {message}")


def dll_name(arch: str) -> str:
    try:
        return f"libomp140.{LIBOMP_ARCH[arch]}.dll"
    except KeyError:
        raise _fail(f"unsupported arch {arch!r} (known: {sorted(LIBOMP_ARCH)})") from None


def compute_want_pins(*, arch: str) -> str:
    return f"libomp={LLVM_TAG}:{LLVM_SHA256} platform=windows-{arch} dll={dll_name(arch)} crt=MT"


def stamp_is_current(dist: Path, want: str, *, arch: str) -> bool:
    dist = Path(dist)
    stamp = dist / ".pins"
    if not stamp.is_file() or stamp.read_text(encoding="utf-8") != want:
        return False
    return (dist / "bin" / dll_name(arch)).is_file() and (dist / IMPORT_LIB).is_file()


def cmake_configure_args(dist: Path, *, arch: str) -> list:
    """Pure function (no subprocess call) so the exact argv is unit-testable
    without a Windows toolchain."""
    argv = [f"-DCMAKE_INSTALL_PREFIX={dist}", *_CMAKE_ARGS_BASE, f"-DLIBOMP_ARCH={LIBOMP_ARCH[arch]}"]
    if arch == "arm64":
        # openmp/runtime/CMakeLists.txt:270-272 calls enable_language(ASM_MASM)
        # on every WIN32 target, and CMake's MASM lookup asks for `ml` on a
        # non-x64 target -- absent from the arm64 developer environment. No
        # MASM source is assembled on aarch64 (src/CMakeLists.txt:113-116 uses
        # z_Linux_asm.S via clang-cl), so LLVM's own MASM-compatible assembler
        # only has to satisfy that enable_language call.
        argv.append("-DCMAKE_ASM_MASM_COMPILER=llvm-ml")
    return argv


def _wanted(name: str) -> bool:
    parts = name.split("/", 2)
    return len(parts) >= 2 and parts[0] == _SRC_ROOT and parts[1] in _EXTRACT_TOPS


def fetch_source(stage: Path) -> Path:
    """Download+verify the llvm-project tarball, extract openmp/ + cmake/
    into ``stage``. Returns the openmp source directory."""
    stage = Path(stage)
    stage.mkdir(parents=True, exist_ok=True)
    tarball = stage / f"llvm-project-{LLVM_VERSION}.src.tar.xz"
    if tarball.exists():
        try:
            verify_sha256(tarball, LLVM_SHA256)
        except Exception:  # noqa: BLE001 - any verification failure means re-download
            tarball.unlink(missing_ok=True)
    if not tarball.exists():
        _log(f"downloading {LLVM_URL}")
        fetch_tarball(LLVM_URL, LLVM_SHA256, tarball)
    _log(f"verified {tarball.name} {LLVM_SHA256}")

    root = stage / _SRC_ROOT
    if root.exists():
        shutil.rmtree(root)
    # Stream mode ("r|xz"): one forward pass over a ~167 MB xz archive.
    # Random-access mode would re-decompress from the start for every member.
    with tarfile.open(tarball, "r|xz") as tf:
        for member in tf:
            if not _wanted(member.name):
                continue
            try:
                tf.extract(member, stage, filter="data")  # noqa: S202 - hash-pinned archive
            except TypeError:
                tf.extract(member, stage)  # noqa: S202 - pre-PEP-706 interpreter
    src = root / "openmp"
    if not (src / "CMakeLists.txt").is_file() or not (root / "cmake").is_dir():
        raise _fail(f"{tarball.name} did not yield {src} and {root / 'cmake'} (unexpected archive layout)")
    return src


def configure_build_install(src: Path, dist: Path, build_dir: Path, *, arch: str) -> None:
    if Path(build_dir).exists():
        shutil.rmtree(build_dir)
    argv = ["cmake", "-S", str(src), "-B", str(build_dir), "-G", "Ninja", *cmake_configure_args(Path(dist), arch=arch)]
    run(argv)
    run(["cmake", "--build", str(build_dir), "--parallel"])
    run(["cmake", "--install", str(build_dir)])


def prune_aliases(dist: Path) -> None:
    for relative in _ALIAS_FILES:
        (Path(dist) / relative).unlink(missing_ok=True)


def assert_layout(dist: Path, *, arch: str) -> None:
    dist = Path(dist)
    required = (f"bin/{dll_name(arch)}", IMPORT_LIB, *REQUIRED_HEADERS)
    missing = [r for r in required if not (dist / r).is_file()]
    if missing:
        listing = "\n".join(sorted(str(p.relative_to(dist)) for p in dist.rglob("*") if p.is_file()))
        raise _fail(f"dist is missing: {' '.join(missing)}\ncomplete listing of what WAS installed:\n{listing}")


def _read_import_lib_members(lib_path: Path, out_path: Path) -> str:
    """``llvm-ar t`` on a COFF import library lists each short-import member
    under the name of the DLL it imports from."""
    try:
        result = run(["llvm-ar", "t", str(lib_path)], check=False)
    except (OSError, SubprocessError) as exc:
        raise win_pe.PeInspectionError(
            f"could not list {lib_path} (llvm-ar: not runnable ({exc})) -- "
            "the measurement did NOT happen; this is an instrument failure"
        ) from exc
    text = (result.stdout or "") + (result.stderr or "")
    out_path.write_text(text, encoding="utf-8")
    if result.returncode != 0:
        raise win_pe.PeInspectionError(
            f"could not list {lib_path} (llvm-ar exit {result.returncode}) -- "
            "the measurement did NOT happen; this is an instrument failure"
        )
    return text


def assert_import_lib_targets_dll(dist: Path, *, arch: str) -> None:
    """The import library is what the decoder links against; it must name the
    DLL we ship, or the decoder imports a file nobody stages."""
    dist = Path(dist)
    name = dll_name(arch)
    text = _read_import_lib_members(dist / IMPORT_LIB, dist / ".libomp_implib_members.txt")
    if not win_pe.token_present_ci(text, name):
        raise win_pe.PeAssertionFailed(f"{IMPORT_LIB} does not import from {name}; members as read:\n{text}")
    _log(f"ASSERT {IMPORT_LIB} imports from {name} OK")


def assert_exports(dist: Path, *, arch: str) -> None:
    dist = Path(dist)
    name = dll_name(arch)
    exports = win_pe.read_exports(dist / "bin" / name, dist / ".libomp_exports.txt")
    for symbol in REQUIRED_EXPORTS:
        win_pe.assert_symbol_exported(exports, symbol, dll_name=name)
        _log(f"ASSERT export {symbol} OK")


def assert_static_crt(dist: Path, *, arch: str) -> None:
    dist = Path(dist)
    name = dll_name(arch)
    deps_text = win_pe.read_dependents(dist / "bin" / name, dist / ".libomp_deps.txt")
    win_pe.assert_absent(
        deps_text,
        "VCRUNTIME140",
        where=f"{name}'s import table",
        why="CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded did not take effect (dynamic CRT)",
    )
    _log("ASSERT static CRT (no VCRUNTIME140 import) OK")


def assert_architecture(dist: Path, *, arch: str) -> None:
    target = Path(dist) / "bin" / dll_name(arch)
    output = None
    if shutil.which("file") is not None:
        try:
            result = run(["file", str(target)], check=False)
            if result.returncode == 0:
                output = result.stdout or ""
        except (OSError, SubprocessError):
            output = None
    if win_pe.assert_machine(output, arch, dll_names=target.name):
        _log(f"ASSERT PE32+ {arch} OK")
    else:
        _log("NOTICE: 'file' unavailable; architecture check SKIPPED (not passed).")


def vendor_license(src: Path, dist: Path) -> None:
    licence = Path(src) / "LICENSE.TXT"
    if not licence.is_file():
        raise _fail(f"no LICENSE.TXT found under {src}")
    dest = Path(dist) / "share" / "licenses" / "libomp"
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(licence, dest / "LICENSE.TXT")


def build(dist: Path, *, arch: str, stage: Optional[Path] = None, force: bool = False) -> Path:
    """Build the Windows libomp dist (DLL + import lib + omp.h) into ``dist``."""
    dist = Path(dist)
    stage = Path(stage) if stage is not None else dist / ".stage"

    want = compute_want_pins(arch=arch)
    if not force and stamp_is_current(dist, want, arch=arch):
        _log(f"dist already at the pinned version {LLVM_TAG}")
        return dist

    src = fetch_source(stage)
    configure_build_install(src, dist, stage / "build", arch=arch)
    prune_aliases(dist)

    assert_layout(dist, arch=arch)
    assert_import_lib_targets_dll(dist, arch=arch)
    assert_exports(dist, arch=arch)
    assert_static_crt(dist, arch=arch)
    assert_architecture(dist, arch=arch)
    vendor_license(src, dist)

    (dist / ".pins").write_text(want, encoding="utf-8")
    shutil.rmtree(stage, ignore_errors=True)
    _log(f"dist ready at {dist}")
    return dist


def main(argv: Optional[list] = None) -> int:
    """Module entry point: ``python -m deps.win_libomp_dist --dist <dir>``.
    The workflow uses ``build_deps.py build libomp-stack``."""
    import argparse

    from .run import assert_native_windows_interpreter

    assert_native_windows_interpreter()

    parser = argparse.ArgumentParser(prog="deps.win_libomp_dist")
    parser.add_argument("--dist", required=True)
    parser.add_argument("--arch", default="x86_64", choices=tuple(LIBOMP_ARCH))
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)

    try:
        build(Path(args.dist), arch=args.arch, force=args.force)
    except (WindowsLibompError, win_pe.PeInspectionError, win_pe.PeAssertionFailed) as exc:
        print(f"[libomp-win] {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
