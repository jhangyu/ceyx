#!/usr/bin/env python3
"""Single entry point for third-party dependency acquisition.

Spec: docs/logs/2026-08-30/Spec_build_rewrite.md §4.1.

    python3 native/scripts/build_deps.py --component heif-stack \
        --platform <auto|macos|linux|windows> --arch <auto|arm64|x86_64> \
        [--only <name>] [--dry-run]

Round 2 (Plan_build_rewrite.md D3 completion / F4 remediation): this file
parses the CLI surface described above, performs the Windows-native-
interpreter startup assertion (spec §2.3), resolves ``platform``/``arch``
from ``auto``, loads the manifest via ``deps.manifest.load()`` and rejects
any ``--component`` name absent from ``manifest.toml``'s ``[component.*]``
tables. ``--dry-run`` prints the ``deps.render.render()`` argv for the
resolved component/platform/arch, one argv element per line, and exits 0.
The assertion suite (D4/M3) is a separate deliverable this CLI will wire
in once it lands.

The manifest loader is imported lazily (inside ``main()``, not at module
scope) so this file has no hard import-time dependency on D1's package,
which had not landed in the tree when this file was written -- see the
try/except in ``_load_manifest_module`` below.
"""
from __future__ import annotations

import argparse
import platform as platform_module
import sys
from pathlib import Path
from typing import Optional

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from deps.run import assert_native_windows_interpreter  # noqa: E402


_PLATFORM_CHOICES = ("auto", "macos", "linux", "windows")
_ARCH_CHOICES = ("auto", "arm64", "x86_64")


def detect_platform() -> str:
    """Resolve ``auto`` to one of macos/linux/windows using the running
    interpreter's own view of the host, never a shelled-out ``uname``."""
    system = platform_module.system()
    if system == "Darwin":
        return "macos"
    if system == "Linux":
        return "linux"
    if system == "Windows":
        return "windows"
    raise RuntimeError(f"unsupported host platform: {system!r}")


def detect_arch() -> str:
    """Resolve ``auto`` to the canonical arch vocabulary (``arm64`` /
    ``x86_64``). Real arch-vocabulary normalisation across every
    dependency's own naming (aom/kvazaar/CMake/uname) belongs to D1's
    ``arch_map.toml`` (spec §4.3) -- this function only distinguishes the
    two architectures this project ships, for CLI resolution purposes."""
    machine = platform_module.machine().lower()
    if machine in ("arm64", "aarch64"):
        return "arm64"
    if machine in ("x86_64", "amd64"):
        return "x86_64"
    raise RuntimeError(f"unsupported host architecture: {machine!r}")


def _load_manifest_module():
    """Import D1's manifest loader lazily. Returns ``None`` (rather than
    raising) if it has not landed yet, so this CLI skeleton stays usable
    standalone even if the ``deps`` package is somehow absent from the tree
    -- callers that need the manifest must check for ``None`` and report
    clearly, never silently proceed without it."""
    try:
        from deps import manifest as manifest_module  # type: ignore[attr-defined]
    except ImportError:
        return None
    return manifest_module


def _load_render_module():
    """Import D2's argv renderer lazily, mirroring ``_load_manifest_module``
    above (same has-it-landed-yet defensiveness)."""
    try:
        from deps import render as render_module  # type: ignore[attr-defined]
    except ImportError:
        return None
    return render_module


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="build_deps.py",
        description="Fetch and build (or resolve from the registry) third-party dependencies.",
    )
    parser.add_argument("--component", required=True, help="component or component-group name from the manifest")
    parser.add_argument("--platform", choices=_PLATFORM_CHOICES, default="auto")
    parser.add_argument("--arch", choices=_ARCH_CHOICES, default="auto")
    parser.add_argument("--only", default=None, help="restrict to a single named component within the group")
    parser.add_argument("--dry-run", action="store_true", help="resolve inputs and report without building")
    return parser


def main(argv: Optional[list] = None) -> int:
    assert_native_windows_interpreter()

    parser = build_arg_parser()
    args = parser.parse_args(argv)

    resolved_platform = detect_platform() if args.platform == "auto" else args.platform
    resolved_arch = detect_arch() if args.arch == "auto" else args.arch

    manifest_module = _load_manifest_module()
    if manifest_module is None:
        print(
            "[build_deps] error: manifest loader (native/scripts/deps/manifest.py, D1) "
            "is not available yet -- cannot acquire dependencies without it.",
            file=sys.stderr,
        )
        return 1

    try:
        loaded = manifest_module.load()
    except manifest_module.ManifestError as exc:
        print(f"[build_deps] error: manifest failed validation: {exc}", file=sys.stderr)
        return 1

    known_components = sorted(loaded["manifest"].get("component", {}))
    if args.component not in known_components:
        print(
            f"[build_deps] error: unknown --component {args.component!r} "
            f"(not present in native/deps/manifest.toml; known components: {known_components})",
            file=sys.stderr,
        )
        return 1

    print(
        f"[build_deps] component={args.component!r} only={args.only!r} "
        f"platform={resolved_platform} arch={resolved_arch}",
        file=sys.stderr,
    )

    render_module = _load_render_module()
    if render_module is None:
        print(
            "[build_deps] error: argv renderer (native/scripts/deps/render.py, D2) "
            "is not available yet -- cannot resolve the configure command line.",
            file=sys.stderr,
        )
        return 1

    try:
        argv = render_module.render(loaded, args.component, resolved_platform, resolved_arch)
    except render_module.RenderError as exc:
        print(f"[build_deps] error: render failed: {exc}", file=sys.stderr)
        return 1

    if args.dry_run:
        for token in argv:
            print(token)
        return 0

    print(
        "[build_deps] error: acquisition is not wired in yet -- D4 (assertion suite) "
        "is a round-2 deliverable still in progress.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
