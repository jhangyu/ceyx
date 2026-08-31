"""Committed, reusable release-publishing CLI (round-6 contract AC5),
promoting the scratch v0.1.6 driver
(``native/scripts/tmp/r5-release-v016/publish_v016.py``) into a generic,
argv-driven tool.

This module never hardcodes a tag or an asset list -- both are supplied by
the caller via ``--tag`` and ``--manifest`` (a JSON file describing which
dist directories to package under which canonical asset names). Packaging,
hashing/locking, upload, and download-back verification are delegated to
:mod:`deps.publish`; this module only parses the manifest, derives
canonical asset names via ``native/deps/arch_map.toml``, and orchestrates
the calls.

Usage::

    python3 publish_release.py --manifest manifest.json --tag v0.1.7 \\
        --repo jhangyu/ceyx --work-dir /tmp/release-work [--dry-run] \\
        [--title TITLE] [--notes-file NOTES.md]

Manifest format (JSON)::

    {
      "assets": [
        {"dist_dir": "path/to/dist", "component": "dng_decoder_native",
         "platform": "windows", "arch": "x86_64"},
        ...
      ]
    }

Canonical asset name is always ``<component>-<platform>-<arch>.tar.gz``;
``arch`` is normalized against ``native/deps/arch_map.toml`` (accepts any
alias listed under an arch's ``uname`` list, e.g. ``amd64`` -> ``x86_64``;
rejects unknown arch tokens rather than passing them through unnormalized).

``--dry-run`` packages every asset, builds+writes ``artifacts.lock``, prints
the plan, and exits 0 -- no ``gh`` calls are made. This is the mode CI runs
on non-tag branches (round-6 contract AC11).
"""
from __future__ import annotations

import argparse
import json
import sys
import tomllib
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

NATIVE_DIR = Path(__file__).resolve().parents[1]
SCRIPTS_DIR = NATIVE_DIR / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

from deps.publish import (  # noqa: E402
    PublishError,
    build_artifacts_lock,
    package_dist,
    publish_release_assets,
    verify_release_assets,
    write_artifacts_lock,
)

ARCH_MAP_PATH = NATIVE_DIR / "deps" / "arch_map.toml"

# Component/platform combinations that must ship as a single atomic archive
# containing a fixed set of files (round-6 contract: "Windows DLL trio
# travels as one atomic group everywhere"). Extend this table -- never
# special-case a filename check outside it.
ATOMIC_FILE_COUNTS: Dict[tuple, int] = {
    ("dng_decoder_native", "windows"): 3,  # dng_decoder_native.dll, heif.dll, libde265.dll
}


class ManifestError(RuntimeError):
    """Raised for malformed/inconsistent manifest entries."""


def load_arch_map(path: Path = ARCH_MAP_PATH) -> Dict[str, Any]:
    if not path.is_file():
        raise ManifestError(f"arch_map.toml not found: {path}")
    with path.open("rb") as fh:
        return tomllib.load(fh)


def normalize_arch(arch: str, arch_map: Dict[str, Any]) -> str:
    """Resolve ``arch`` to its canonical key in ``arch_map`` (e.g. the
    table's own key, or any of its ``uname`` aliases). Raises
    :class:`ManifestError` for an arch token not present anywhere in the
    table -- silently passing through an unrecognized token is exactly the
    hardcoded-vocabulary drift this table exists to prevent.
    """
    if arch in arch_map:
        return arch
    for canonical, entry in arch_map.items():
        if arch in entry.get("uname", []):
            return canonical
    raise ManifestError(
        f"arch {arch!r} not found in {ARCH_MAP_PATH} (canonical keys or "
        f"'uname' aliases) -- extend the table rather than hardcoding a "
        f"new spelling"
    )


def load_manifest(path: Path) -> List[Dict[str, str]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    assets = data.get("assets")
    if not isinstance(assets, list) or not assets:
        raise ManifestError(f"manifest {path} has no non-empty 'assets' list")
    for entry in assets:
        for key in ("dist_dir", "component", "platform", "arch"):
            if key not in entry:
                raise ManifestError(f"manifest entry missing {key!r}: {entry}")
    return assets


def _assert_atomic_file_count(dist_dir: Path, component: str, platform: str) -> None:
    expected = ATOMIC_FILE_COUNTS.get((component, platform))
    if expected is None:
        return
    actual = sum(1 for p in dist_dir.rglob("*") if p.is_file())
    if actual != expected:
        raise PublishError(
            f"[publish_release] atomic group check failed for "
            f"{component}-{platform}: expected {expected} files in "
            f"{dist_dir}, found {actual}"
        )
    print(
        f"[publish_release] ASSERT ok atomic group {component}-{platform}: "
        f"{actual} files"
    )


def build_plan(
    manifest_assets: Sequence[Dict[str, str]], arch_map: Dict[str, Any]
) -> List[Dict[str, Any]]:
    """Resolve each manifest entry into a concrete (dist_dir, asset_name)
    plan entry with the canonical name derived from arch_map."""
    plan = []
    seen_names = set()
    for entry in manifest_assets:
        dist_dir = Path(entry["dist_dir"])
        component = entry["component"]
        platform = entry["platform"]
        arch = normalize_arch(entry["arch"], arch_map)
        ext = entry.get("ext", "tar.gz")
        asset_name = f"{component}-{platform}-{arch}.{ext}"
        if asset_name in seen_names:
            raise ManifestError(f"duplicate asset name in manifest: {asset_name}")
        seen_names.add(asset_name)
        plan.append(
            {
                "dist_dir": dist_dir,
                "component": component,
                "platform": platform,
                "arch": arch,
                "asset_name": asset_name,
            }
        )
    return plan


def run_publish(args: argparse.Namespace) -> int:
    arch_map = load_arch_map()
    manifest_assets = load_manifest(Path(args.manifest))
    plan = build_plan(manifest_assets, arch_map)

    work_dir = Path(args.work_dir)
    package_dir = work_dir / "package"
    archive_paths: List[Path] = []

    for item in plan:
        dist_dir = item["dist_dir"]
        if not dist_dir.is_dir():
            raise ManifestError(f"dist_dir does not exist: {dist_dir}")
        _assert_atomic_file_count(dist_dir, item["component"], item["platform"])
        archive_path = package_dist(dist_dir, package_dir, item["asset_name"])
        archive_paths.append(archive_path)
        print(f"[publish_release] packaged {item['asset_name']} <- {dist_dir}")

    lock = build_artifacts_lock(archive_paths)
    lock_path = work_dir / "artifacts.lock"
    write_artifacts_lock(lock, lock_path)
    print(f"[publish_release] wrote lock: {lock_path}")
    for name, entry in sorted(lock["assets"].items()):
        print(f"  {name}: sha256={entry['sha256']} size={entry['size']}")

    if args.dry_run:
        print("[publish_release] --dry-run: skipping upload/download-back verify")
        return 0

    if not args.tag:
        raise ManifestError("--tag is required for a non-dry-run publish")
    if not args.repo:
        raise ManifestError("--repo is required for a non-dry-run publish")

    notes = ""
    if args.notes_file:
        notes = Path(args.notes_file).read_text(encoding="utf-8")

    upload_paths = list(archive_paths) + [lock_path]
    publish_release_assets(
        args.tag,
        upload_paths,
        repo=args.repo,
        title=args.title or args.tag,
        notes=notes,
        prerelease=args.prerelease,
    )
    print(f"[publish_release] uploaded {len(upload_paths)} assets to {args.tag}")

    verify_release_assets(
        args.tag, lock, repo=args.repo, download_dir=work_dir / "download-back"
    )
    return 0


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Package dist directories into canonically-named tar.gz assets, "
            "hash-pin them into artifacts.lock, and (unless --dry-run) "
            "publish + download-back verify them against a GitHub release. "
            "Tag and asset list are always supplied by the caller (manifest "
            "+ --tag), never hardcoded."
        )
    )
    parser.add_argument(
        "--manifest",
        required=True,
        help="Path to a JSON manifest describing dist_dir/component/platform/arch entries.",
    )
    parser.add_argument("--tag", default="", help="Release tag (required unless --dry-run).")
    parser.add_argument("--repo", default="", help="GitHub repo, e.g. jhangyu/ceyx.")
    parser.add_argument(
        "--work-dir",
        default="native/scripts/tmp/round6-ci/publish-work",
        help="Scratch directory for packaged archives, artifacts.lock, and download-back verification.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Package + lock + print plan only; no gh calls.",
    )
    parser.add_argument("--title", default="", help="Release title (defaults to --tag).")
    parser.add_argument("--notes-file", default="", help="Path to a file with release notes body.")
    parser.add_argument(
        "--prerelease",
        action="store_true",
        default=True,
        help="Mark the release as a prerelease (default: True, matching deps.publish default).",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    try:
        return run_publish(args)
    except (ManifestError, PublishError) as exc:
        print(f"[publish_release] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
