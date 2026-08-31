"""Vendor LibRaw (+ bundled RawSpeed3 + LibRaw-cmake overlay) -- Python port
of ``native/scripts/fetch_libraw_dist.sh``.

Migration ruling (2026-09-01, contract item 11 / ENTRY-POINT RULE,
``docs/logs/2026-09-01/contract-windows-codec-round.md``). Exposed as
``build_deps.py fetch libraw``.

Each component is fetched via a throwaway git checkout, then its ``.git``
directory is stripped and the resolved revision recorded in a
``.vendor-rev`` sidecar -- this is required because a nested ``.git``
directory makes the parent repo treat the whole vendored tree as an opaque
untracked boundary (like a submodule gitlink), which would make it
impossible to track ``PROVENANCE.md`` inside ``third_party/libraw/`` per
this project's vendoring policy (untracked source, tracked
``PROVENANCE.md``).

Every revision/URL below is TRANSCRIBED VERBATIM from
``fetch_libraw_dist.sh`` -- a divergence found is a real bug, not a style
choice.

NO SHELL, ANYWHERE: every git invocation goes through :mod:`deps.run` as an
argv list with ``shell=False``.
"""
from __future__ import annotations

import shutil
import sys
from pathlib import Path
from typing import Optional

try:  # pragma: no cover - import style depends on how the caller invokes us
    from .run import SubprocessError, run
except ImportError:  # pragma: no cover - fallback for direct script execution
    from run import SubprocessError, run  # type: ignore[no-redef]

LIBRAW_REV = "df226ea4178ccd74245f4f13c23adddfa01411c9"
RAWSPEED_REV = "c835b05aecfacb7343f7c424abd620aa12116c3f"
# LibRaw ships no CMakeLists.txt of its own at the pinned revision (see
# third_party/libraw/README.cmake): this project vendors the community
# LibRaw/LibRaw-cmake overlay as a third pinned dependency.
LIBRAW_CMAKE_REV = "eb98e4325aef2ce85d2eb031c2ff18640ca616d3"
LIBRAW_URL = "https://github.com/LibRaw/LibRaw.git"
RAWSPEED_URL = "https://github.com/darktable-org/rawspeed.git"
LIBRAW_CMAKE_URL = "https://github.com/LibRaw/LibRaw-cmake.git"


class LibrawFetchError(RuntimeError):
    """Raised when a clone, patch application, or vendoring step fails."""


def _fail(message: str) -> LibrawFetchError:
    return LibrawFetchError(f"[fetch] FAILED: {message}")


def _log(message: str) -> None:
    print(f"[fetch] {message}")


def read_vendor_rev(directory: Path) -> Optional[str]:
    marker = Path(directory) / ".vendor-rev"
    if not marker.is_file():
        return None
    return marker.read_text(encoding="utf-8").strip()


def clone_at(url: str, rev: str, directory: Path) -> None:
    """Fetch ``url`` at ``rev`` into ``directory`` via a throwaway git
    checkout, unless ``directory/.vendor-rev`` already records ``rev``.
    Leaves ``directory/.git`` in place so patch application can use it;
    :func:`strip_git` removes it once that is done.

    A tracked ``PROVENANCE.md`` alongside the vendored source (only true for
    ``third_party/libraw/``) survives a re-fetch: it is backed up before the
    directory is wiped and restored after.
    """
    directory = Path(directory)
    if read_vendor_rev(directory) == rev:
        _log(f"{directory} already at {rev} (.vendor-rev)")
        return

    provenance = directory / "PROVENANCE.md"
    backup_text = provenance.read_text(encoding="utf-8") if provenance.is_file() else None

    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True, exist_ok=True)
    run(["git", "-C", str(directory), "init", "-q"])
    run(["git", "-C", str(directory), "remote", "add", "origin", url])
    run(["git", "-C", str(directory), "fetch", "-q", "--depth", "1", "origin", rev])
    run(["git", "-C", str(directory), "checkout", "-q", rev])

    if backup_text is not None:
        provenance.write_text(backup_text, encoding="utf-8")


def strip_git(directory: Path) -> None:
    """Remove ``directory/.git`` and record the resolved HEAD into
    ``directory/.vendor-rev``."""
    directory = Path(directory)
    rev = run(["git", "-C", str(directory), "rev-parse", "HEAD"]).stdout.strip()
    shutil.rmtree(directory / ".git", ignore_errors=True)
    (directory / ".vendor-rev").write_text(rev + "\n", encoding="utf-8")


def overlay_rawspeed_patches(project_patch_src: Path, patch_dir: Path) -> bool:
    """Wholesale-replace ``patch_dir`` (LibRaw's own in-tree RawSpeed3 patch
    directory, wiped and restored to LibRaw's originals by every LibRaw
    re-clone) with this project's re-ported set, if one exists. REPLACES,
    never merges: a patch dropped as upstream-fixed must stay dropped rather
    than reappearing from LibRaw's tree (fetch_libraw_dist.sh:72-91)."""
    project_patch_src = Path(project_patch_src)
    patch_dir = Path(patch_dir)
    if not project_patch_src.is_dir():
        return False
    if patch_dir.exists():
        shutil.rmtree(patch_dir)
    patch_dir.mkdir(parents=True, exist_ok=True)
    for patch in sorted(project_patch_src.glob("*.patch")):
        shutil.copy2(patch, patch_dir / patch.name)
    _log(f"overlaid re-ported RawSpeed3 patch set from {project_patch_src}")
    return True


def _apply_or_skip(repo_dir: Path, patch: Path, *, label: str) -> None:
    """Apply ``patch`` at ``repo_dir`` if it is not already applied (forward
    check), report "already applied" if the reverse check succeeds (a
    forward-authored patch whose reverse-check-success means "already
    applied" -- this project's patch sets are all forward-authored against
    the pinned revision since the Phase 19 re-pin, unlike the pre-Phase-19
    arrangement where reverse-applying was sometimes the correct action),
    and raise if neither check succeeds."""
    check_forward = run(
        ["git", "-C", str(repo_dir), "apply", "--check", str(patch)], check=False
    )
    if check_forward.returncode == 0:
        run(["git", "-C", str(repo_dir), "apply", str(patch)])
        _log(f"applied {label}{patch.name}")
        return
    check_reverse = run(
        ["git", "-C", str(repo_dir), "apply", "--check", "--reverse", str(patch)], check=False
    )
    if check_reverse.returncode == 0:
        _log(f"{label}{patch.name} already applied")
        return
    raise _fail(f"could not apply {label}{patch.name} at {repo_dir}")


def apply_patches(repo_dir: Path, patch_dir: Path, *, label: str) -> None:
    repo_dir = Path(repo_dir)
    patch_dir = Path(patch_dir)
    if not patch_dir.is_dir() or not (repo_dir / ".git").is_dir():
        return
    for patch in sorted(patch_dir.glob("*.patch")):
        _apply_or_skip(repo_dir, patch, label=label)


def fetch(native_dir: Optional[Path] = None) -> Path:
    """Vendor LibRaw + RawSpeed3 + LibRaw-cmake into
    ``native_dir/third_party/{libraw,libraw-cmake}``. Mirrors
    fetch_libraw_dist.sh's ordering: clone all three -> overlay the
    project's RawSpeed3 patch set -> apply RawSpeed3 patches -> apply
    project LibRaw patches -> strip .git from all three."""
    native_dir = Path(native_dir) if native_dir is not None else Path(__file__).resolve().parents[2]
    dest = native_dir / "third_party" / "libraw"
    rs_dest = dest / "RawSpeed3" / "rawspeed"
    libraw_cmake_dest = native_dir / "third_party" / "libraw-cmake"
    project_patch_dir = native_dir / "patches" / "libraw"

    clone_at(LIBRAW_URL, LIBRAW_REV, dest)
    clone_at(RAWSPEED_URL, RAWSPEED_REV, rs_dest)
    clone_at(LIBRAW_CMAKE_URL, LIBRAW_CMAKE_REV, libraw_cmake_dest)

    patch_dir = dest / "RawSpeed3" / "patches"
    rawspeed_patch_src = native_dir / "patches" / "rawspeed3"
    overlay_rawspeed_patches(rawspeed_patch_src, patch_dir)

    apply_patches(rs_dest, patch_dir, label="")
    apply_patches(dest, project_patch_dir, label="project ")

    for directory in (dest, rs_dest, libraw_cmake_dest):
        if (directory / ".git").is_dir():
            strip_git(directory)

    _log(f"LibRaw {LIBRAW_REV} + RawSpeed {RAWSPEED_REV} + LibRaw-cmake {LIBRAW_CMAKE_REV} ready at {dest}")
    return dest


def main(argv: Optional[list] = None) -> int:
    import argparse

    parser = argparse.ArgumentParser(prog="deps.fetch_libraw")
    parser.add_argument("--native-dir", default=None)
    args = parser.parse_args(argv)
    try:
        fetch(Path(args.native_dir) if args.native_dir else None)
    except (LibrawFetchError, SubprocessError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
