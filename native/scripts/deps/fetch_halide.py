"""Fetch the vendored Halide v21.0.0 binary distribution -- Python port of
``native/scripts/fetch_halide_v21_dist.sh``.

Migration ruling (2026-09-01, contract item 11 / ENTRY-POINT RULE,
``docs/logs/2026-09-01/contract-windows-codec-round.md``): every CI fetch
script MIGRATES into a ``build_deps.py`` subcommand. Exposed as
``build_deps.py fetch halide``.

All pins (version, release commit, asset naming) are TRANSCRIBED VERBATIM
from ``fetch_halide_v21_dist.sh`` -- a divergence found here is a real bug,
not a style choice, and must be reported rather than silently reconciled.

No external ``curl``/``tar``/``unzip`` process: downloads go through
:func:`deps.fetch.download` (``urllib.request``) and archives are read with
stdlib :mod:`tarfile`/:mod:`zipfile`, so there is no argv for a shell to
mis-parse.
"""
from __future__ import annotations

import platform as platform_module
import shutil
import tarfile
import zipfile
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Optional, Tuple

try:  # pragma: no cover - import style depends on how the caller invokes us
    from .fetch import FetchError, download
except ImportError:  # pragma: no cover - fallback for direct script execution
    from fetch import FetchError, download  # type: ignore[no-redef]

HALIDE_VERSION = "21.0.0"
# Every asset name below shares this build commit (verified against the live
# release listing 2026-08-21, per fetch_halide_v21_dist.sh's own comment) --
# the ABI matches the pinned version in native/third_party/halide/VERSION.
HALIDE_COMMIT = "b629c80de18f1534ec71fddd8b567aa7027a0876"
HALIDE_RELEASE_BASE = f"https://github.com/halide/Halide/releases/download/v{HALIDE_VERSION}"

# (uname -s equivalent, normalised arch) -> (Halide's own platform tag, archive ext)
_PLATFORM_TABLE = {
    ("Darwin", "arm64"): ("arm-64-osx", "tar.gz"),
    ("Darwin", "x86_64"): ("x86-64-osx", "tar.gz"),
    ("Linux", "arm64"): ("arm-64-linux", "tar.gz"),
    ("Linux", "x86_64"): ("x86-64-linux", "tar.gz"),
    ("Windows", "x86_64"): ("x86-64-windows", "zip"),
    ("Windows", "x86_32"): ("x86-32-windows", "zip"),
}


class HalideFetchError(FetchError):
    """Raised when the host platform/arch is unsupported or the fetched
    archive has an unexpected layout."""


def _fail(message: str) -> HalideFetchError:
    return HalideFetchError(f"[halide] FAILED: {message}")


def _log(message: str) -> None:
    print(f"[halide] {message}")


def normalise_arch(system: str, machine: str) -> str:
    """Map a raw (``platform.system()``, ``platform.machine()``) pair to the
    two-way arch vocabulary this table uses, mirroring the shell script's
    per-OS ``case`` blocks (MSYS/Git-Bash reports x86_64 on 64-bit Windows,
    same as the shell original notes)."""
    machine = machine.lower()
    if system == "Darwin":
        if machine in ("arm64", "aarch64"):
            return "arm64"
        if machine == "x86_64":
            return "x86_64"
    elif system == "Linux":
        if machine in ("aarch64", "arm64"):
            return "arm64"
        if machine == "x86_64":
            return "x86_64"
    elif system == "Windows":
        if machine in ("x86_64", "amd64"):
            return "x86_64"
        if machine in ("i686", "i386"):
            return "x86_32"
    raise _fail(f"unsupported host arch: {system}/{machine}")


def resolve_asset(system: str, machine: str) -> Tuple[str, str, str]:
    """Return ``(platform_tag, archive_ext, asset_filename)`` for a raw
    ``(platform.system(), platform.machine())`` pair."""
    arch = normalise_arch(system, machine)
    key = (system, arch)
    if key not in _PLATFORM_TABLE:
        raise _fail(f"unsupported host OS: {system}")
    platform_tag, ext = _PLATFORM_TABLE[key]
    asset = f"Halide-{HALIDE_VERSION}-{platform_tag}-{HALIDE_COMMIT}.{ext}"
    return platform_tag, ext, asset


def already_present(dest: Path) -> bool:
    """Windows ships an import library (Halide.lib), POSIX hosts a static
    archive -- mirrors the shell original's dual check."""
    dest = Path(dest)
    return (dest / "lib" / "libHalide.a").is_file() or (dest / "lib" / "Halide.lib").is_file()


def extract_zip_stripping_top(archive_path: Path, dest: Path) -> None:
    """No ``tar --strip-components`` equivalent for zip: extract to a
    staging dir and hoist the single top-level directory's contents into
    ``dest`` (mirrors the shell original's ``TOP=... && tar -cf - . | tar
    -xf -`` dotfile-safe hoist)."""
    with TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        with zipfile.ZipFile(archive_path) as zf:
            zf.extractall(tmp_path)  # noqa: S202 - asset URL is fixed, not user input
        entries = [p for p in tmp_path.iterdir() if p.is_dir()]
        if len(entries) != 1:
            raise _fail(f"unexpected archive layout in {archive_path.name}: top-level entries {entries}")
        top = entries[0]
        dest.mkdir(parents=True, exist_ok=True)
        for item in top.iterdir():
            target = dest / item.name
            if target.exists():
                if target.is_dir():
                    shutil.rmtree(target)
                else:
                    target.unlink()
            shutil.move(str(item), str(target))


def extract_tar_stripping_top(archive_path: Path, dest: Path) -> None:
    """Equivalent of ``tar -xzf ... --strip-components=1``."""
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive_path, "r:gz") as tf:
        for member in tf.getmembers():
            parts = member.name.split("/", 1)
            if len(parts) != 2 or not parts[1]:
                continue
            member.name = parts[1]
            try:
                tf.extract(member, dest, filter="data")  # noqa: S202 - asset URL is fixed
            except TypeError:
                tf.extract(member, dest)  # noqa: S202 - pre-PEP-706 interpreter


def fetch(dest: Optional[Path] = None, *, force: bool = False) -> Path:
    """Fetch (or reuse) the Halide v21 dist into ``dest`` (default
    ``native/third_party/halide``)."""
    native_dir = Path(__file__).resolve().parents[2]
    dest = Path(dest) if dest is not None else native_dir / "third_party" / "halide"
    if not force and already_present(dest):
        _log(f"third_party/halide already present ({dest}) -- nothing to do.")
        return dest

    system = platform_module.system()
    machine = platform_module.machine()
    _, ext, asset = resolve_asset(system, machine)
    url = f"{HALIDE_RELEASE_BASE}/{asset}"

    with TemporaryDirectory() as tmp:
        archive_path = Path(tmp) / asset
        _log(f"downloading {asset} (host: {system}/{machine}) ...")
        download(url, archive_path)
        if ext == "zip":
            extract_zip_stripping_top(archive_path, dest)
        else:
            extract_tar_stripping_top(archive_path, dest)

    version_text = (
        f"halide/Halide@v{HALIDE_VERSION}\n"
        f"binary_provenance: vendored from https://github.com/halide/Halide/releases/tag/v{HALIDE_VERSION}\n"
        f"asset: {asset}\n"
        "abi_notes: schedule changes break AOT artifacts; bump requires full regen.\n"
    )
    (dest / "VERSION").write_text(version_text, encoding="utf-8")
    _log(f"OK -- dist ready at {dest}")
    return dest


def main(argv: Optional[list] = None) -> int:
    import argparse
    import sys

    parser = argparse.ArgumentParser(prog="deps.fetch_halide")
    parser.add_argument("--dest", default=None, help="override native/third_party/halide")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)
    try:
        fetch(Path(args.dest) if args.dest else None, force=args.force)
    except HalideFetchError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
