"""Tarball and git acquisition with SHA-256 verification before extraction.

Spec: docs/logs/2026-08-30/Spec_build_rewrite.md §2.3, §4.1 (D3 round 1).
All external tool invocations go through :mod:`native.scripts.deps.run`,
which is the only place ``subprocess`` is called from this package -- so
this module never sets ``shell=True`` and never builds a pipeline.

Downloading uses :mod:`urllib.request` (stdlib), not an external ``curl``/
``wget`` process, so acquisition itself has no argv to mis-parse in the
first place -- only ``fetch_git`` shells out, and it does so via
:func:`native.scripts.deps.run.run`.
"""
from __future__ import annotations

import hashlib
import shutil
import urllib.error
import urllib.request
from pathlib import Path
from typing import Optional

try:  # pragma: no cover - import style depends on how the caller invokes us
    from .run import run
except ImportError:  # pragma: no cover - fallback for direct script execution
    from run import run  # type: ignore[no-redef]


class FetchError(RuntimeError):
    """Raised when acquisition or verification of a dependency source fails."""


def sha256_of(path: Path) -> str:
    """Return the lowercase hex SHA-256 digest of the file at ``path``."""
    digest = hashlib.sha256()
    with Path(path).open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_sha256(path: Path, expected_sha256: str) -> None:
    """Verify the file at ``path`` matches ``expected_sha256``.

    Raises :class:`FetchError` naming the file and both hashes (expected
    vs. actual) on mismatch (A3.3). Does not delete ``path`` -- the caller
    owns removal, so download and verification stay independently
    testable.
    """
    expected = expected_sha256.strip().lower()
    actual = sha256_of(path)
    if actual != expected:
        raise FetchError(
            f"SHA-256 mismatch for {path}: expected {expected}, got {actual}"
        )


def download(url: str, dest: Path) -> Path:
    """Download ``url`` to ``dest`` via a ``.part`` staging file that is
    atomically renamed into place only on success.

    On any download failure the partial file is removed before
    :class:`FetchError` is raised (A3.3) -- a caller can never observe a
    half-written file at ``dest`` itself.
    """
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    try:
        with urllib.request.urlopen(url) as response, part.open("wb") as fh:  # noqa: S310 - fixed manifest URLs, never user input
            shutil.copyfileobj(response, fh)
    except (urllib.error.URLError, OSError, ValueError) as exc:
        part.unlink(missing_ok=True)
        raise FetchError(f"download failed for {url}: {exc}") from exc
    part.replace(dest)
    return dest


def fetch_tarball(url: str, sha256: str, dest: Path) -> Path:
    """Download ``url`` to ``dest`` and verify its SHA-256 equals
    ``sha256``. Returns the path to the verified tarball.

    On a hash mismatch, the downloaded file is removed before
    :class:`FetchError` is raised, naming the file and both hashes
    (A3.3) -- a caller can never mistake a corrupted download for a
    verified one, and no stale corrupted file is left on disk to
    confuse the next run.
    """
    dest = Path(dest)
    downloaded = download(url, dest)
    try:
        verify_sha256(downloaded, sha256)
    except FetchError:
        downloaded.unlink(missing_ok=True)
        raise
    return downloaded


def fetch_git(repo: str, ref: str, dest: Path, *, depth: Optional[int] = 1) -> Path:
    """Clone ``repo`` at ``ref`` (tag/branch) into ``dest``.

    Used for components whose release tarball omits a file needed only on
    one platform's branch -- e.g. kvazaar on Windows is git-cloned rather
    than fetched as a tarball because the release tarball omits
    ``src/threadwrapper/src/pthread.cpp`` (handoff §B; spec §4.2 manifest
    example, ``component.kvazaar.source.windows``). Do not "fix" this by
    unifying the fetch mechanism across platforms -- macOS/Linux use the
    tarball deliberately.

    Invoked through :func:`native.scripts.deps.run.run`, i.e. as an argv
    list with ``shell=False`` -- never through a shell that could
    re-interpret ``dest`` if it is a Windows path with a drive letter.
    """
    dest = Path(dest)
    if dest.exists():
        shutil.rmtree(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    argv = ["git", "clone", "--branch", ref, "--quiet"]
    if depth is not None:
        argv += ["--depth", str(depth)]
    argv += [repo, str(dest)]
    run(argv)
    return dest
