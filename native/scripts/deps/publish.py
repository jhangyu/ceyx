"""Release publishing layer: package a built dist, hash-pin its assets, and
publish them to a GitHub Release through the carrier -- D12 subset (round-5
contract AC3).

Spec: docs/logs/2026-08-31/round-5-contract.md AC3 -- "發佈路徑產出
artifacts.lock（每資產含 SHA-256），下載回驗斷言在一次真實執行中通過（活體
證明，非 dry-run）."

Division of labour, consistent with the rest of ``deps/``:

  - This module does NOT build anything. It takes an already-built dist
    directory (produced by :mod:`deps.heif` / :mod:`deps.win_heif_dist` /
    :mod:`deps.execute`) and packages + publishes it.
  - Packaging (tar) uses stdlib :mod:`tarfile`, matching
    :mod:`deps.execute`'s "no external tar" rule -- there is no argv for a
    shell to re-interpret.
  - Hashing uses stdlib :mod:`hashlib` (SHA-256), the same primitive
    :mod:`deps.fetch` already uses for verify-before-extract.
  - The only external tool invoked is ``gh`` (GitHub CLI), and only through
    :func:`deps.run.run` -- argv lists, ``shell=False``, exit code read from
    ``CompletedProcess.returncode``, per spec Section 2.3/2.4.

``artifacts.lock`` is a JSON file (not TOML, to avoid a second parser
dependency for a flat name->sha256 map) recording, for every published
asset: its filename, byte size, and SHA-256. The download-back assertion
(:func:`verify_release_assets`) re-downloads each asset from the release and
recomputes its hash, asserting equality against the lock -- this is the
"活體證明" the contract requires: a lock file alone proves nothing about
what actually reached the release, only a round-trip does.
"""
from __future__ import annotations

import hashlib
import json
import tarfile
from pathlib import Path
from typing import Any, Dict, List, Sequence

try:  # pragma: no cover - import style depends on how the caller invokes us
    from .run import run
except ImportError:  # pragma: no cover - fallback for direct script execution
    from run import run  # type: ignore[no-redef]


class PublishError(RuntimeError):
    """Raised when packaging, hashing, upload, or download-back verification
    fails. Always names the concrete asset/tag involved."""


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def package_dist(dist_dir: Path, out_dir: Path, asset_name: str) -> Path:
    """Tar ``dist_dir`` into ``out_dir/asset_name`` (a ``.tar.gz``).

    Deterministic ordering (sorted directory walk) so re-packaging the same
    dist twice yields byte-identical output modulo mtimes -- not required by
    the contract, but cheap and avoids surprising hash churn between local
    runs.
    """
    if not dist_dir.is_dir():
        raise PublishError(f"dist directory does not exist: {dist_dir}")
    out_dir.mkdir(parents=True, exist_ok=True)
    archive_path = out_dir / asset_name
    with tarfile.open(archive_path, "w:gz") as tar:
        for entry in sorted(dist_dir.rglob("*")):
            if entry.is_file():
                tar.add(entry, arcname=str(entry.relative_to(dist_dir)))
    return archive_path


def build_artifacts_lock(asset_paths: Sequence[Path]) -> Dict[str, Any]:
    """Compute the SHA-256 lock entries for ``asset_paths``.

    Returns a dict shaped ``{"assets": {filename: {"sha256": ..., "size":
    ...}}}`` -- a flat, order-independent structure so re-running publish
    with a different asset ordering does not spuriously change the lock.
    """
    assets: Dict[str, Any] = {}
    for path in asset_paths:
        if not path.is_file():
            raise PublishError(f"asset does not exist: {path}")
        assets[path.name] = {
            "sha256": _sha256_file(path),
            "size": path.stat().st_size,
        }
    return {"assets": assets}


def write_artifacts_lock(lock: Dict[str, Any], lock_path: Path) -> None:
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock_path.write_text(json.dumps(lock, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def load_artifacts_lock(lock_path: Path) -> Dict[str, Any]:
    if not lock_path.is_file():
        raise PublishError(f"artifacts.lock does not exist: {lock_path}")
    return json.loads(lock_path.read_text(encoding="utf-8"))


def publish_release_assets(
    tag: str,
    asset_paths: Sequence[Path],
    *,
    repo: str,
    title: str = "",
    notes: str = "",
    prerelease: bool = True,
) -> None:
    """Create (or reuse) a GitHub Release at ``tag`` and upload every path in
    ``asset_paths`` via ``gh release``.

    Uses ``gh release view`` to check existence first (idempotent create,
    matching the existing ``macos_build.yml`` precedent at
    .github/workflows/macos_build.yml:485), then ``gh release upload
    --clobber`` so a re-run of this function overwrites its OWN prior
    assets rather than failing -- it must never touch a release/tag it did
    not create itself; callers are responsible for using a tag scoped to
    this purpose (e.g. an ``r5-test-*`` prefix), never a production release
    tag.
    """
    if not asset_paths:
        raise PublishError("no assets to publish")
    view = run(["gh", "release", "view", tag, "--repo", repo], check=False)
    if view.returncode != 0:
        create_argv = [
            "gh", "release", "create", tag,
            "--repo", repo,
            "--title", title or tag,
            "--notes", notes or f"Hash-pinned dependency assets for {tag} (D12).",
            # A real (non-prerelease) release must explicitly claim the
            # "Latest" label. The previous value here was `--latest=false`
            # -- every production release was actively told NOT to be
            # latest, which is why two consecutive releases published
            # without the label and needed a manual correction each time;
            # this was never a matter of relying on an implicit default.
            # A prerelease is excluded from "latest" consideration by
            # GitHub regardless, so it gets only --prerelease.
            #
            # Limitation: this flag is only applied when this call creates
            # the release (the branch above, `gh release view` not found).
            # If the release already exists, execution falls through
            # straight to `gh release upload` below and this flag is never
            # applied -- a re-run against an already-created release, or a
            # release created by some other path, will not retroactively
            # gain the Latest label from this function.
            "--prerelease" if prerelease else "--latest",
        ]
        run(create_argv)
    upload_argv = ["gh", "release", "upload", tag, "--repo", repo, "--clobber"]
    upload_argv.extend(str(p) for p in asset_paths)
    run(upload_argv)


def verify_release_assets(
    tag: str,
    lock: Dict[str, Any],
    *,
    repo: str,
    download_dir: Path,
) -> List[str]:
    """Download-back assertion: re-download every asset named in ``lock``
    from the release at ``tag`` and assert its SHA-256 matches the lock.

    This is the activity that makes the lock trustworthy -- a lock computed
    purely from local files says nothing about what actually reached the
    release (upload could silently truncate/corrupt/skip an asset). Returns
    the list of ``"ASSERT ok ..."`` lines printed (SUCCESS-LOUD, matching
    the heif.py precedent), for callers that want to log them.
    """
    download_dir.mkdir(parents=True, exist_ok=True)
    assets = lock.get("assets", {})
    if not assets:
        raise PublishError("artifacts.lock has no assets to verify")
    ok_lines: List[str] = []
    for name, entry in sorted(assets.items()):
        expected_sha256 = entry["sha256"]
        expected_size = entry["size"]
        dest = download_dir / name
        if dest.exists():
            dest.unlink()
        run([
            "gh", "release", "download", tag,
            "--repo", repo,
            "--pattern", name,
            "--dir", str(download_dir),
            "--clobber",
        ])
        if not dest.is_file():
            raise PublishError(
                f"[publish] download-back failed: {name!r} not found in {download_dir} after `gh release download`"
            )
        actual_size = dest.stat().st_size
        if actual_size != expected_size:
            raise PublishError(
                f"[publish] download-back size mismatch for {name!r}: "
                f"lock={expected_size} downloaded={actual_size}"
            )
        actual_sha256 = _sha256_file(dest)
        if actual_sha256 != expected_sha256:
            raise PublishError(
                f"[publish] download-back SHA-256 mismatch for {name!r}: "
                f"lock={expected_sha256} downloaded={actual_sha256}"
            )
        line = f"[publish] ASSERT ok download-back hash match: {name} sha256={actual_sha256} size={actual_size}"
        print(line)
        ok_lines.append(line)
    return ok_lines


def publish_dist(
    dist_dir: Path,
    *,
    component: str,
    platform: str,
    arch: str,
    tag: str,
    repo: str,
    work_dir: Path,
) -> Dict[str, Any]:
    """End-to-end D12 flow: package -> hash -> lock -> upload -> download-back
    verify. Returns the lock dict. Raises :class:`PublishError` (or lets a
    ``deps.run.SubprocessError`` propagate) on any failure -- there is no
    partial-success return value.
    """
    asset_name = f"{component}-{platform}-{arch}.tar.gz"
    package_dir = work_dir / "package"
    archive_path = package_dist(dist_dir, package_dir, asset_name)
    lock = build_artifacts_lock([archive_path])
    lock_path = work_dir / "artifacts.lock"
    write_artifacts_lock(lock, lock_path)
    publish_release_assets(tag, [archive_path], repo=repo)
    verify_release_assets(tag, lock, repo=repo, download_dir=work_dir / "download-back")
    return lock
