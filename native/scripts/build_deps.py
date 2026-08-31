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

Round 4 (D3 execution layer) adds the form that actually BUILDS:

    python3 native/scripts/build_deps.py build <component> \
        --platform <auto|macos|linux|windows> \
        --arch <auto|arm64|x86_64> \
        --dist <dir> [--stage <name>] [--jobs N] [--dry-run]

``<component>`` is either a manifest component (built generically through
``deps.execute``) or the pseudo-component ``heif-stack``, which runs the
Unix libde265/kvazaar/aom/libheif assembly in ``deps.heif`` -- the port of
``native/scripts/fetch_heif_deps.sh``. This subcommand shape is an agreed
interface contract shared with the Windows carrier work; do not change it
unilaterally.

The legacy ``--component X --dry-run`` form above is retained verbatim so
nothing that already invokes it has to change at the same time as the
execution layer lands.

The manifest loader is imported lazily (inside ``main()``, not at module
scope) so this file has no hard import-time dependency on D1's package,
which had not landed in the tree when this file was written -- see the
try/except in ``_load_manifest_module`` below.
"""
from __future__ import annotations

import argparse
import os
import platform as platform_module
import sys
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Optional

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from deps.fetch import FetchError  # noqa: E402
from deps.run import SubprocessError, assert_native_windows_interpreter  # noqa: E402


_PLATFORM_CHOICES = ("auto", "macos", "linux", "windows")
_ARCH_CHOICES = ("auto", "arm64", "x86_64")

# Pseudo-component migrating the Windows libjxl dist build off
# build_libjxl_dist_windows.sh (2026-09-01 contract item 10 / ENTRY-POINT
# RULE, docs/logs/2026-09-01/contract-windows-codec-round.md). Unlike
# heif-stack this has no macOS/Linux implementation here: those platforms
# already build libjxl through the ordinary manifest component ("libjxl",
# self-built from git via deps/execute.py) and have no shell script or
# committed dist to migrate away from -- jxl-stack exists purely to give the
# Windows-only build_libjxl_dist_windows.sh replacement a
# `build_deps.py build <name>` home, per the ENTRY-POINT RULE that every
# migrated capability lands as a build_deps.py subcommand.
_JXL_STACK_COMPONENT = "jxl-stack"


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


def build_subcommand_parser() -> argparse.ArgumentParser:
    """The round-4 execution CLI (``build_deps.py build <component> ...``).

    Kept as its own parser rather than an argparse subparser of the legacy
    one so the legacy ``--component`` form stays REQUIRED-argument-exact and
    its existing behaviour is provably untouched by this addition.
    """
    parser = argparse.ArgumentParser(
        prog="build_deps.py build",
        description="Acquire, configure, build and install a manifest component.",
    )
    parser.add_argument("component", help="manifest component name, or the 'heif-stack' group")
    parser.add_argument("--platform", choices=_PLATFORM_CHOICES, default="auto")
    parser.add_argument("--arch", choices=_ARCH_CHOICES, default="auto")
    parser.add_argument("--dist", required=True, help="install prefix the built artefacts land in")
    parser.add_argument(
        "--stage",
        default="all",
        help="for heif-stack on macOS/Linux: which stage to run "
             "(all|libde265|kvazaar|aom|libheif|assemble). The per-component split "
             "exists so each build is its own foreground invocation short enough to "
             "fit inside a normal command timeout. The Windows dist has no stage "
             "split, so any value but 'all' is rejected there rather than ignored.",
    )
    parser.add_argument("--jobs", type=int, default=None, help="cmake --build --parallel value")
    parser.add_argument(
        "--force",
        action="store_true",
        help="heif-stack on Windows only: rebuild even when the .pins stamp is current.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the resolved configure argv and exit 0 without building",
    )
    return parser


def resolve_dist(raw: str, target_platform: str, *, host_is_windows: Optional[bool] = None) -> Path:
    """Turn ``--dist`` into the path to hand the renderer, WITHOUT resolving a
    foreign platform's path against the host's filesystem.

    Round-4 defect this exists to fix: an unconditional ``Path(raw).resolve()``
    is correct when the host and the target agree, but on a macOS host
    rendering Windows argv (spec §8.1's explicit use case -- inspecting and
    unit-testing the exact Windows command line from a dev machine) it
    prepends the Unix cwd to a drive-lettered path, and ``PureWindowsPath``
    then renders the result as ``\\Users\\...\\C:\\ceyx\\dist``. That is the
    drive-letter-mangling family recorded on 2026-08-30 (``D:/a/...`` ->
    ``\\d\\a\\...``), reintroduced by our own convenience call rather than by a
    shell.

    Rules:
      - host and target agree  -> ``resolve()``, so a relative ``--dist`` and
        symlinks behave exactly as a developer expects.
      - host and target differ -> the value is passed through as the TARGET's
        own path type and must already be absolute for that target. A relative
        path is unresolvable in that direction (there is no meaningful cwd on
        the other platform), so it is rejected loudly instead of being turned
        into a plausible-looking wrong path.

    Note the real builds are never cross: Windows dists are built on Windows
    runners. The cross direction is inspection/dry-run, which is precisely
    where a silently wrong path is most likely to be believed.

    ``host_is_windows`` is injectable purely so a test can exercise the
    Windows-HOST branch from a Unix dev box. It is a parameter rather than a
    patched ``os.name`` deliberately: ``os`` is a shared global, and
    reassigning ``os.name`` changes ``pathlib``'s own behaviour process-wide
    (observed: ``Path.resolve()`` then raises ``UnsupportedOperation``). That
    is the same "no global monkeypatching" rule the rest of this suite follows
    for ``os.environ``/``subprocess``.
    """
    if host_is_windows is None:
        host_is_windows = os.name == "nt"
    target_is_windows = target_platform == "windows"
    if host_is_windows == target_is_windows:
        return Path(raw).resolve()

    pure = PureWindowsPath(raw) if target_is_windows else PurePosixPath(raw)
    is_absolute = bool(getattr(pure, "drive", "")) or bool(pure.root)
    if not is_absolute:
        raise ValueError(
            f"--dist {raw!r} is relative, but the target platform "
            f"({target_platform}) is not the host. A relative path cannot be "
            f"resolved against another platform's working directory -- pass an "
            f"absolute path (e.g. C:/ceyx-dist for windows)."
        )
    return Path(str(pure))


def publish_subcommand_parser() -> argparse.ArgumentParser:
    """The round-5 D12 publish CLI (``build_deps.py publish ...``).

    Packages an already-built ``--dist`` directory, hash-pins it into
    ``artifacts.lock``, uploads it to a GitHub Release (``--tag``), and
    downloads it back to assert the published bytes match the lock. This
    subcommand never builds anything itself -- run ``build_deps.py build``
    first.
    """
    parser = argparse.ArgumentParser(
        prog="build_deps.py publish",
        description="Package, hash-pin, and publish a built dist to a GitHub Release, "
                     "then verify the upload via a download-back hash check.",
    )
    parser.add_argument("component", help="component/group name used to name the asset (e.g. heif-stack)")
    parser.add_argument("--platform", choices=_PLATFORM_CHOICES, default="auto")
    parser.add_argument("--arch", choices=_ARCH_CHOICES, default="auto")
    parser.add_argument("--dist", required=True, help="the already-built dist directory to publish")
    parser.add_argument("--tag", required=True, help="GitHub Release tag to publish/verify against")
    parser.add_argument("--repo", required=True, help="GitHub repo in OWNER/NAME form")
    parser.add_argument("--work-dir", required=True, help="scratch dir for the tarball, lock, and download-back")
    return parser


def _run_publish(argv: list) -> int:
    """Handler for ``build_deps.py publish ...`` (D12)."""
    args = publish_subcommand_parser().parse_args(argv)
    resolved_platform = detect_platform() if args.platform == "auto" else args.platform
    resolved_arch = detect_arch() if args.arch == "auto" else args.arch

    from deps import publish as publish_module  # noqa: PLC0415
    from deps.run import SubprocessError as _SubprocessError  # noqa: PLC0415

    dist_dir = Path(args.dist).resolve()
    work_dir = Path(args.work_dir).resolve()
    print(
        f"[build_deps] publish component={args.component!r} platform={resolved_platform} "
        f"arch={resolved_arch} dist={dist_dir} tag={args.tag!r} repo={args.repo!r}",
        file=sys.stderr,
    )
    try:
        lock = publish_module.publish_dist(
            dist_dir,
            component=args.component,
            platform=resolved_platform,
            arch=resolved_arch,
            tag=args.tag,
            repo=args.repo,
            work_dir=work_dir,
        )
    except (publish_module.PublishError, _SubprocessError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print(f"[build_deps] publish ok: {json_dumps_compact(lock)}", file=sys.stderr)
    return 0


def json_dumps_compact(obj) -> str:
    import json as _json

    return _json.dumps(obj, sort_keys=True)


def _run_build(argv: list) -> int:
    """Handler for ``build_deps.py build ...``. Imports the execution layer
    lazily so the legacy dry-run path keeps working even if a future edit
    breaks an import in deps/execute.py or deps/heif.py."""
    args = build_subcommand_parser().parse_args(argv)

    resolved_platform = detect_platform() if args.platform == "auto" else args.platform
    resolved_arch = detect_arch() if args.arch == "auto" else args.arch

    manifest_module = _load_manifest_module()
    render_module = _load_render_module()
    if manifest_module is None or render_module is None:
        print(
            "[build_deps] error: the deps package (native/scripts/deps/) is not "
            "importable -- cannot acquire dependencies without it.",
            file=sys.stderr,
        )
        return 1

    try:
        loaded = manifest_module.load()
    except manifest_module.ManifestError as exc:
        print(f"[build_deps] error: manifest failed validation: {exc}", file=sys.stderr)
        return 1

    from deps import execute as execute_module  # noqa: PLC0415 - see docstring
    from deps import heif as heif_module  # noqa: PLC0415

    try:
        dist = resolve_dist(args.dist, resolved_platform)
    except ValueError as exc:
        print(f"[build_deps] error: {exc}", file=sys.stderr)
        return 1
    print(
        f"[build_deps] build component={args.component!r} platform={resolved_platform} "
        f"arch={resolved_arch} dist={dist}",
        file=sys.stderr,
    )

    try:
        if args.component == heif_module.COMPONENT:
            # The HEIF stack is one pseudo-component with TWO implementations,
            # dispatched on platform. They are deliberately not merged: the
            # dists differ in capability, not merely in toolchain. Unix is
            # full-capability (libde265 + kvazaar HEVC encode + aom AV1);
            # Windows is decode-only (libde265 + libheif) per the 2026-08-31
            # OPTION 1 ruling. Their assertion sets therefore differ in kind --
            # demanding kvz_api_get/aom_codec_av1_cx of the Windows dist would
            # require encoders it intentionally does not contain.
            if resolved_platform == "windows":
                from deps import win_heif_dist  # noqa: PLC0415 - lazy, see docstring

                if args.stage != "all":
                    print(
                        f"[build_deps] error: --stage {args.stage!r} is a macOS/Linux "
                        f"concept; the Windows HEIF dist is built in one pass and has "
                        f"no stage split. Rejected rather than silently ignored.",
                        file=sys.stderr,
                    )
                    return 1
                if args.dry_run:
                    for name in ("libde265", "libheif"):
                        print(f"# {name}")
                        for token in render_module.render(
                            loaded, name, resolved_platform, resolved_arch, dist=str(dist)
                        ):
                            print(token)
                    return 0
                try:
                    win_heif_dist.build(loaded, dist, arch=resolved_arch, force=args.force)
                except (
                    win_heif_dist.WindowsHeifError,
                    win_heif_dist.win_pe.PeInspectionError,
                    win_heif_dist.win_pe.PeAssertionFailed,
                ) as exc:
                    print(f"[heif-win] {exc}", file=sys.stderr)
                    return 1
                return 0

            if args.force:
                print(
                    "[build_deps] error: --force is Windows-only; the macOS/Linux "
                    "stack is made idempotent by its per-stage artefact checks and "
                    "the .pins stamp. Rejected rather than silently ignored.",
                    file=sys.stderr,
                )
                return 1
            if args.dry_run:
                for name in ("kvazaar", "libheif"):
                    print(f"# {name}")
                    for token in render_module.render(
                        loaded, name, resolved_platform, resolved_arch, dist=str(dist)
                    ):
                        print(token)
                return 0
            return heif_module.build(
                loaded,
                resolved_platform,
                resolved_arch,
                dist,
                stage_arg=args.stage,
            )

        if args.component == _JXL_STACK_COMPONENT:
            # Windows-only (see the constant's docstring above). macOS/Linux
            # requests are rejected loudly rather than silently no-op'd or
            # routed to the ordinary "libjxl" manifest component under a
            # different name -- a caller who typed jxl-stack on macOS/Linux
            # almost certainly meant the manifest component "libjxl" and
            # should be told so, not given a surprising alias.
            if resolved_platform != "windows":
                print(
                    f"[build_deps] error: {_JXL_STACK_COMPONENT!r} currently only "
                    "supports --platform windows. macOS/Linux build libjxl through "
                    "the ordinary manifest component 'libjxl' directly "
                    "(build_deps.py build libjxl ...), which already has a "
                    "self-built git acquisition on both platforms.",
                    file=sys.stderr,
                )
                return 1
            if args.stage != "all":
                print(
                    f"[build_deps] error: --stage {args.stage!r} is a macOS/Linux "
                    f"heif-stack concept; the Windows libjxl dist is built in one "
                    f"pass and has no stage split. Rejected rather than silently "
                    f"ignored.",
                    file=sys.stderr,
                )
                return 1
            from deps import win_jxl_dist  # noqa: PLC0415 - lazy, see docstring

            if args.dry_run:
                # jxl-stack is NOT manifest-rendered (win_jxl_dist.py module
                # docstring: [component.libjxl] has no source.windows to
                # render against) -- there is no argv preview to print here,
                # unlike heif-stack's --dry-run branch above.
                print(
                    f"# {_JXL_STACK_COMPONENT} (windows): self-contained git-clone "
                    f"recipe transcribed from build_libjxl_dist_windows.sh, tag="
                    f"{win_jxl_dist.JXL_TAG} -- not manifest-rendered, no argv preview"
                )
                return 0
            try:
                win_jxl_dist.build(dist, arch=resolved_arch, force=args.force)
            except (
                win_jxl_dist.WindowsJxlError,
                win_jxl_dist.win_pe.PeInspectionError,
                win_jxl_dist.win_pe.PeAssertionFailed,
            ) as exc:
                print(f"[jxl-win] {exc}", file=sys.stderr)
                return 1
            return 0

        known_components = sorted(loaded["manifest"].get("component", {}))
        if args.component not in known_components:
            print(
                f"[build_deps] error: unknown component {args.component!r} (not present "
                f"in native/deps/manifest.toml; known components: {known_components}, "
                f"plus the groups {heif_module.COMPONENT!r} and {_JXL_STACK_COMPONENT!r})",
                file=sys.stderr,
            )
            return 1

        if args.dry_run:
            for token in render_module.render(
                loaded, args.component, resolved_platform, resolved_arch, dist=str(dist)
            ):
                print(token)
            return 0

        execute_module.build_component(
            loaded,
            args.component,
            resolved_platform,
            resolved_arch,
            dist,
            dist / ".stage",
            jobs=args.jobs,
        )
        return 0
    except (
        execute_module.ExecuteError,
        heif_module.HeifError,
        render_module.RenderError,
        SubprocessError,
        FetchError,
    ) as exc:
        # Reported, not re-raised: these are the expected failure modes
        # (a tool exited non-zero, a hash did not match, an assertion found
        # the capability genuinely absent) and their messages already name
        # the concrete artefact. A traceback would bury that under frames.
        print(str(exc), file=sys.stderr)
        return 1


def main(argv: Optional[list] = None) -> int:
    assert_native_windows_interpreter()

    tokens = list(sys.argv[1:] if argv is None else argv)
    if tokens and tokens[0] == "build":
        return _run_build(tokens[1:])
    if tokens and tokens[0] == "publish":
        return _run_publish(tokens[1:])

    parser = build_arg_parser()
    args = parser.parse_args(tokens)

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
