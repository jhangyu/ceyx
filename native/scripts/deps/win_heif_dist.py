"""Windows HEIF dist assembly -- the Python port of
``native/scripts/build_heif_dist_windows.sh``.

Sibling of ``deps/heif.py`` (macOS/Linux), which explicitly refuses Windows
and points here. The two are deliberately NOT merged: the dists differ in
capability, not merely in toolchain. The Unix dist is full-capability
(libde265 + kvazaar HEVC encode + aom AV1), the Windows dist is
**decode-only** (libde265 + libheif, nothing else) per the lead's OPTION 1
ruling of 2026-08-31. Their assertion sets therefore differ in kind: asserting
``kvz_api_get``/``aom_codec_av1_cx`` here -- as ``heif.py`` correctly does for
Unix -- would demand encoders this dist intentionally does not contain.

The heavy lifting is NOT re-implemented: acquisition, the three cmake phases
and the manifest ``[outputs]`` check all come from ``deps/execute.py``, and PE
inspection from ``deps/win_pe.py``. What is genuinely Windows-specific and
lives here is only: the two-step ordering (libde265 must be installed on disk
before libheif's find module runs), the by-presence resolution of libde265's
import-library spelling, the decode-only assertion set, and the ``.pins``
stamp.

LICENCE (do not "optimise" this away): libheif and libde265 are
LGPL-3.0-or-later. They are built as SEPARATE SHARED LIBRARIES and linked
dynamically, which satisfies LGPL-3 section 4(d)(1) outright -- a user can
replace heif.dll / libde265.dll next to the application. Static linking into
dng_decoder_native would trigger the 4(d)(0) duty to ship relinkable object
files with every release. WITH_X265 stays OFF because x265 is GPL-2.0.

NO SHELL, ANYWHERE: every external tool runs through ``deps/run.py`` as an
argv list with ``shell=False``, which is what closes the four Windows
path-rewriting triggers (a shell re-interpreting a drive letter, a bare
leading slash, a flag value that looks like a path, and MSYS/Cygwin argv
translation). ``run.py`` additionally rejects an MSYS/Cygwin interpreter
outright, so this module must be invoked from native Windows Python.
"""
from __future__ import annotations

import shutil
import sys
from pathlib import Path
from typing import Any, Optional

try:  # pragma: no cover - import style depends on how the caller invokes us
    from . import execute as execute_mod
    from . import win_pe
    from .run import SubprocessError, run
except ImportError:  # pragma: no cover - fallback for direct script execution
    import execute as execute_mod  # type: ignore[no-redef]
    import win_pe  # type: ignore[no-redef]
    from run import SubprocessError, run  # type: ignore[no-redef]

PLATFORM = "windows"

# Resolution order matches the shell original's: upstream installs the runtime
# as bin/libde265.dll while its import library is lib/de265.lib. That asymmetry
# is UPSTREAM CONVENTION and must not be tidied -- heif.dll's import table names
# "libde265.dll", so a renamed copy would simply never be loaded.
_DE265_IMPLIB_CANDIDATES = ("lib/de265.lib", "lib/libde265.lib")
_DE265_DLL_CANDIDATES = ("bin/libde265.dll", "bin/de265.dll")

# Decode-only dist: the capability that must be present, and the licence
# contamination that must not be.
_REQUIRED_HEIF_SYMBOL = "heif_decode_image"
_REQUIRED_HEIF_DEPENDENCY = "de265"
_FORBIDDEN_HEIF_SYMBOL = "x265"

_REQUIRED_FILES = (
    "bin/heif.dll",
    "lib/heif.lib",
    "include/libheif/heif.h",
    "include/libde265/de265.h",
)


class WindowsHeifError(RuntimeError):
    """Raised when any stage of the Windows HEIF assembly fails. The message
    always names the artefact and the concrete missing fact, mirroring the
    shell script's ``[heif-win] FAILED:`` lines."""


def _fail(message: str) -> "WindowsHeifError":
    return WindowsHeifError(f"[heif-win] FAILED: {message}")


def _log(message: str) -> None:
    print(f"[heif-win] {message}")


def _component(loaded: dict[str, Any], name: str) -> dict[str, Any]:
    comp = loaded["manifest"].get("component", {}).get(name)
    if comp is None:
        raise _fail(f"manifest has no component.{name}")
    return comp


def want_pins(loaded: dict[str, Any], arch: str = "x86_64") -> str:
    """Reproduce the shell script's ``WANT_PINS`` string EXACTLY, derived from
    the manifest rather than from restated constants, so a version bump cannot
    leave the stamp and the build disagreeing.

    Byte-compatible with a ``.pins`` file written by
    build_heif_dist_windows.sh, so a dist built by either carrier is
    recognised as current by the other -- which is what makes the shell->Python
    switchover a no-op for an already-built tree rather than a forced rebuild.
    """
    heif = _component(loaded, "libheif")
    de265 = _component(loaded, "libde265")
    heif_sha = str(heif["source"]["default"]["sha256"])
    de265_sha = str(de265["source"]["windows"]["sha256"])
    return (
        f"libheif={heif['version']}:{heif_sha} "
        f"libde265={de265['version']}:{de265_sha} "
        f"platform=windows-{arch}"
    )


def stamp_is_current(dist: Path, loaded: dict[str, Any], arch: str = "x86_64") -> bool:
    """True when ``.pins`` matches AND both DLLs are actually on disk.

    Both halves matter: a stamp alone can outlive a deleted DLL, and then the
    "already built" fast path ships nothing.
    """
    dist = Path(dist)
    stamp = dist / ".pins"
    if not stamp.is_file() or stamp.read_text(encoding="utf-8").strip() != want_pins(loaded, arch).strip():
        return False
    if not (dist / "bin" / "heif.dll").is_file():
        return False
    return any((dist / candidate).is_file() for candidate in _DE265_DLL_CANDIDATES)


def build_libde265(loaded: dict[str, Any], arch: str, dist: Path, stage: Path) -> None:
    """Build libde265 from source. SELF-BUILT ON WINDOWS PERMANENTLY -- see
    manifest.toml [component.libde265.source.windows], which records three
    durable, non-version-related blockers against the vcpkg port (its dec265
    tool's bundled getopt clone does not compile under clang-cl; the port
    passes no /clang:-msse4.1 so libde265's if(MSVC) SIMD gap recurs where the
    manifest cannot reach it; and no built-in triplet expresses
    dynamic-library + static-CRT). Do not "migrate" this to vcpkg.
    """
    _log(f"building libde265 {execute_mod.component_version(loaded, 'libde265')}")
    execute_mod.build_component(loaded, "libde265", PLATFORM, arch, dist, stage)


def resolve_de265_import_library(dist: Path) -> str:
    """Return the libde265 import library that was ACTUALLY installed.

    The spelling differs by generator/toolchain (de265.lib vs libde265.lib).
    Guessing wrong does not fail loudly -- it yields a silent
    ``LIBDE265_FOUND=false`` inside libheif's configure, i.e. a green build
    that decodes nothing.
    """
    resolved = win_pe.resolve_existing(dist, _DE265_IMPLIB_CANDIDATES, what="libde265 import library")
    _log(f"libde265 import library: {resolved}")
    return resolved


def build_libheif(loaded: dict[str, Any], arch: str, dist: Path, stage: Path) -> None:
    """Build libheif against the just-installed libde265.

    ``LIBDE265_LIBRARY`` is passed again as an extra argument with the
    by-presence-resolved path. The manifest declares the nominal spelling
    (``{dist}/lib/de265.lib``) because it must render deterministically on a
    macOS dev machine with no dist on disk; this call supplies the real one.
    A later ``-D`` on a cmake command line wins for the same cache variable,
    so this overrides the rendered value rather than conflicting with it -- the
    manifest stays the declaration of WHICH options exist, and only this one
    path is resolved from the filesystem.
    """
    implib = resolve_de265_import_library(dist)
    extra = [f"-DLIBDE265_LIBRARY={Path(dist) / implib}"]
    _log(f"building libheif {execute_mod.component_version(loaded, 'libheif')}")
    execute_mod.build_component(loaded, "libheif", PLATFORM, arch, dist, stage, extra_args=extra)


def assert_layout(dist: Path) -> str:
    """Assert every shipped file is present, and return the resolved libde265
    DLL path (relative).

    Asserting each file BY NAME here means a layout change fails with a message
    naming the missing file, rather than as an opaque find_library() failure
    inside the decoder's configure much later.
    """
    dist = Path(dist)
    de265_dll = win_pe.resolve_existing(dist, _DE265_DLL_CANDIDATES, what="libde265 runtime DLL")
    _log(f"DE265_DLL={de265_dll}")

    missing = [required for required in _REQUIRED_FILES if not (dist / required).is_file()]
    if missing:
        listing = "\n".join(sorted(str(p.relative_to(dist)) for p in dist.rglob("*") if p.is_file()))
        raise _fail(f"dist is missing: {' '.join(missing)}\ncomplete listing of what WAS installed:\n{listing}")
    return de265_dll


def assert_capabilities(dist: Path, de265_dll: str) -> None:
    """Prove the dist can actually decode, and carries no GPL contamination.

    Proof, not assumption: a libheif built without a working libde265
    configures and installs perfectly happily and then decodes nothing.

    Every check reads the tool's COMPLETE output from a file and matches it in
    Python. No pipeline exists anywhere in this path -- ``run.py`` rejects a
    pipe character in argv outright -- so the SIGPIPE-under-pipefail inversion
    that made the shell version's contamination check unable to fire cannot
    recur here.
    """
    dist = Path(dist)
    heif_dll = dist / "bin" / "heif.dll"

    exports = win_pe.read_exports(heif_dll, dist / ".heif_exports.txt")
    deps_text = win_pe.read_dependents(heif_dll, dist / ".heif_deps.txt")

    win_pe.assert_symbol_exported(exports, _REQUIRED_HEIF_SYMBOL, dll_name="heif.dll")
    _log(f"ASSERT {_REQUIRED_HEIF_SYMBOL} OK ({win_pe.count_exported_symbols(exports)} exports seen)")

    win_pe.assert_depends_on(
        deps_text,
        _REQUIRED_HEIF_DEPENDENCY,
        dll_name="heif.dll",
        consequence=(
            "it was built WITHOUT an HEVC decoder and would silently decode nothing."
        ),
    )
    _log("ASSERT de265 dependency OK")

    win_pe.assert_absent(
        exports,
        _FORBIDDEN_HEIF_SYMBOL,
        where="heif.dll's export table",
        why="x265 is GPL-2.0 and must never be linked into this dist",
    )
    _log("ASSERT no-x265 OK")

    assert_architecture(dist, de265_dll)


def _file_tool_output(targets: list[Path]) -> Optional[str]:
    """Return ``file``'s output for ``targets``, or None when the tool is
    unavailable. Best-effort by design, exactly as the shell original: on a
    runner without ``file`` the architecture check is SKIPPED, and a skip is
    reported as a skip, never as a pass.
    """
    if shutil.which("file") is None:
        return None
    try:
        result = run(["file", *[str(t) for t in targets]], check=False)
    except (OSError, SubprocessError):
        return None
    if result.returncode != 0:
        return None
    return result.stdout or ""


def assert_architecture(dist: Path, de265_dll: str) -> None:
    dist = Path(dist)
    targets = [dist / "bin" / "heif.dll", dist / de265_dll]
    output = _file_tool_output(targets)
    if win_pe.assert_machine_x86_64(output, dll_names="heif.dll / libde265.dll"):
        _log("ASSERT PE32+ x86-64 OK")
    else:
        _log("NOTICE: 'file' unavailable; architecture check SKIPPED (not passed).")


def build(
    loaded: dict[str, Any],
    dist: Path,
    *,
    arch: str = "x86_64",
    stage: Optional[Path] = None,
    force: bool = False,
) -> Path:
    """Build the whole Windows decode-only HEIF dist into ``dist``.

    Ordering is load-bearing: libde265 must be INSTALLED ON DISK before
    libheif is configured, because libheif's FindLIBDE265 module resolves
    against the installed tree.
    """
    dist = Path(dist)
    stage = Path(stage) if stage is not None else dist / ".stage"

    if not force and stamp_is_current(dist, loaded, arch):
        _log("dist already at the pinned versions:")
        _log(f"  {want_pins(loaded, arch)}")
        return dist

    stage.mkdir(parents=True, exist_ok=True)
    build_libde265(loaded, arch, dist, stage)
    build_libheif(loaded, arch, dist, stage)

    de265_dll = assert_layout(dist)
    assert_capabilities(dist, de265_dll)

    (dist / ".pins").write_text(want_pins(loaded, arch), encoding="utf-8")
    shutil.rmtree(stage, ignore_errors=True)
    _log(f"dist ready at {dist}")
    return dist


def main(argv: Optional[list] = None) -> int:
    """Module entry point: ``python -m deps.win_heif_dist --dist <dir>``.

    INTERIM, and deliberately additive. The frozen team contract is
    ``build_deps.py build <component> --platform windows --arch x86_64
    --dist <dir>``, whose Windows route for the ``heif-stack`` pseudo-component
    has to land in ``build_deps.py`` -- a file this round's Windows work does
    not own. Rather than edit that shared surface unilaterally, or wire CI to a
    dispatch that does not exist yet and burn a red round proving it, the
    module exposes its own entry point so the workflow has a path that provably
    works today. Once ``build_deps.py`` routes heif-stack/windows here, the
    workflow switches to the contract form and this stays as the direct-run
    convenience it already is.
    """
    import argparse

    from .run import assert_native_windows_interpreter

    # Same startup guard build_deps.py applies: refuse an MSYS/Cygwin
    # interpreter, which reports os.name == "posix" and would silently
    # reintroduce the shell path-rewriting this whole carrier exists to avoid.
    assert_native_windows_interpreter()

    parser = argparse.ArgumentParser(prog="deps.win_heif_dist")
    parser.add_argument("--dist", required=True, help="install prefix the dist lands in")
    parser.add_argument("--arch", default="x86_64", choices=("x86_64",))
    parser.add_argument("--force", action="store_true", help="rebuild even when .pins is current")
    args = parser.parse_args(argv)

    from . import manifest as manifest_mod

    try:
        build(manifest_mod.load(), Path(args.dist), arch=args.arch, force=args.force)
    except (WindowsHeifError, win_pe.PeInspectionError, win_pe.PeAssertionFailed) as exc:
        print(f"[heif-win] {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
