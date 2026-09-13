#!/usr/bin/env python3
"""ceyx unified CI entry point -- argparse dispatch only.

Mirrors Halcyon's shape exactly (`Halcyon/scripts/ci.py:1-26`): every
CI job body is one `python3 native/scripts/ci.py <subcommand>` line, and
the same command is runnable on a laptop. All logic lives in
`native/scripts/ci/`:

    targets.py    per-platform DATA ONLY (native/scripts/ci/targets.py)
    report.py     the only writer of MARKER=value / == banner == / ::error::
    run.py        list-argv subprocess wrapper (shell=False, RC self-capture)
    <gate>.py     one module per gate (verify_artifact, stage, orientation,
                  codec_probe, capability, minruntime, dt_needed, provision,
                  zlib_build), each registered here and nowhere else

If a function in this file grows past 15 lines it belongs in the module it
dispatches to (Halcyon's own rule, quoted verbatim -- this is ceyx's copy of
the same discipline, not a coincidence of naming).

Frozen CLI surface (workflows depend on these exact strings --
docs/logs/2026-09-13/pyci-plan.md WI-1):

    python3 native/scripts/ci.py selftest
    python3 native/scripts/ci.py marker-diff --baseline F --candidate F [--leg L]
    python3 native/scripts/ci.py verify-artifact   --platform linux|windows [--arch A]
    python3 native/scripts/ci.py verify-artifact   --platform macos --arch A --dylib-path D
    python3 native/scripts/ci.py import-closure    --platform P
    python3 native/scripts/ci.py min-runtime       --platform P [--arch A]
    python3 native/scripts/ci.py assert-exports    --platform linux|windows [--arch A]
    python3 native/scripts/ci.py assert-exports    --platform macos --arch A --dylib-path D
    python3 native/scripts/ci.py assert-exports    --platform android --artifact-dir D --ndk-home H
    python3 native/scripts/ci.py assert-no-avx512  --platform P
    python3 native/scripts/ci.py assert-orientation --platform P
    python3 native/scripts/ci.py codec-probe       --platform P --workspace W [--dist-dir D]
    python3 native/scripts/ci.py capability-vector --platform P --kind codec|build [--source probe|configure-log]
    python3 native/scripts/ci.py assert-configure-log --log-path F --pattern R --label L --error E
    python3 native/scripts/ci.py assert-staged-companions --platform macos --arch A --dylib-path D --artifact-dir T
    python3 native/scripts/ci.py stage             --platform linux --artifact-dir D --native-dir N
    python3 native/scripts/ci.py stage             --platform windows|android --artifact-dir D --source-dir S
    python3 native/scripts/ci.py stage             --platform macos --artifact-dir D --dylib-path P
    python3 native/scripts/ci.py assert-staged-group --platform linux|windows|android --artifact-dir D
    python3 native/scripts/ci.py dt-needed         --platform linux --artifact-dir D --runner-temp T
    python3 native/scripts/ci.py dt-needed         --platform android --artifact-dir D --runner-temp T --ndk-home H [--build-log L]
    python3 native/scripts/ci.py vcpkg-baseline    --github-env PATH
    python3 native/scripts/ci.py vcpkg-bootstrap   --baseline SHA --runner-temp T
    python3 native/scripts/ci.py vcpkg-install     --triplet T --workspace W --runner-temp T [--feature F ...]
    python3 native/scripts/ci.py assert-vcpkg-artefacts --platform linux --triplet T --runner-temp T
    python3 native/scripts/ci.py verify-interpreter --forbid-hostedtoolcache
    python3 native/scripts/ci.py ensure-cmake      --min 3.28
    python3 native/scripts/ci.py build-zlib        --version 1.3.1 --workspace W
    python3 native/scripts/ci.py locate-clang-cl   [--github-path PATH]
    python3 native/scripts/ci.py verify-vulkan-lib [--vulkan-sdk PATH]

`--platform` is always explicit and never inferred from the host OS.
`--arch` is required iff `targets.spec(platform)["requires_arch"]` is True
(C-G9: macOS yes, everything else is an argparse error if `--arch` is
passed at all).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Repo root is derived from this script's own on-disk location, never from
# the invocation cwd -- this file lives at native/scripts/ci.py.
REPO_ROOT = Path(__file__).resolve().parent.parent.parent

sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))

# Subcommands that take --platform, in the order they appear in the frozen
# surface above. Each maps to the (module, arch-requirement) it will
# dispatch to once that module exists; at P0 every one of them is
# unimplemented and returns via _not_yet().
_PLATFORM_COMMANDS = (
    "verify-artifact",
    "import-closure",
    "min-runtime",
    "assert-exports",
    "assert-no-avx512",
    "assert-orientation",
    "codec-probe",
    "capability-vector",
    "assert-staged-companions",
    "stage",
    "assert-staged-group",
    "dt-needed",
    "assert-vcpkg-artefacts",
)

# The three platforms `codec-probe` supports, PERMANENTLY -- android has no
# `probe_codecs` step at all (codec_probe.py docstring; plan WI-13's
# negative-space AC), unlike `_linux_only_commands` in dispatch() below,
# which names platforms whose twin modules simply have not landed YET and
# is expected to shrink as later pushes add them. This set is not expected
# to ever change: there is no android twin scheduled, and there never will
# be one to schedule.
_CODEC_PROBE_PLATFORMS = frozenset({"linux", "macos", "windows"})

# Same PERMANENT shape as `_CODEC_PROBE_PLATFORMS`: android has no
# capability-vector leg at all (capability.py's own docstring --
# android_build.yml:216's S-E2 step is a single `echo` SKIP line, not a
# `codec_capability_probe.py` call), verified by two independent greps
# before capability.py was written. There is no android twin scheduled,
# ever -- do not merge this with `_linux_only_commands` below.
_CAPABILITY_VECTOR_PLATFORMS = frozenset({"linux", "macos", "windows"})

# Subcommands that never take --platform at all.
_PLATFORMLESS_COMMANDS = (
    "selftest",
    "marker-diff",
    "assert-configure-log",
    "vcpkg-baseline",
    "vcpkg-bootstrap",
    "vcpkg-install",
    "verify-interpreter",
    "ensure-cmake",
    "build-zlib",
    "locate-clang-cl",
    "verify-vulkan-lib",
)


def _requires_arch(platform: str) -> bool:
    import ci.targets as targets

    return bool(targets.spec(platform).get("requires_arch", False))


def _enforce_arch_requirement(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    """C-G9, enforced post-parse: --arch is required for a platform that
    needs it, and rejected for one that doesn't. Applied to every command
    carrying both --platform and --arch. `parser.error()` prints usage and
    calls `sys.exit(2)`, matching argparse's own native validation-failure
    shape.

    Applicability is tested by PRESENCE of the `--arch` attribute
    (`"arch" not in vars(args)`), not by its VALUE being `None` --
    `getattr(args, "arch", None) is None` cannot distinguish "this command's
    parser never defined --arch at all" (codec-probe: no --arch parameter
    exists, C-G9 never applies) from "this command's parser defined --arch
    and the caller omitted it" (min-runtime on macOS: that IS the violation
    C-G9 exists to catch). Collapsing those two cases is exactly what made
    `codec-probe --platform macos` demand a flag the module has no
    parameter for -- a value-based check cannot see the difference; a
    presence check can.
    """
    if not hasattr(args, "platform") or "arch" not in vars(args):
        return
    requires = _requires_arch(args.platform)
    arch = args.arch
    if requires and arch is None:
        parser.error(f"--arch is required for --platform {args.platform!r}")
    if not requires and arch is not None:
        parser.error(f"--arch is not accepted for --platform {args.platform!r}")


def _enforce_orientation_flags(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    """assert-orientation only: --dylib-path is required for --platform
    macos and rejected elsewhere; --artifact-dir/--ndk-home are required
    together for --platform android and rejected elsewhere. Same
    reject-don't-silently-ignore posture as C-G9's --arch handling --
    the module (orientation.py) already refuses to run with a missing
    required parameter (ValueError), so the CLI refusing a WRONG one is
    the same discipline applied on the way in."""
    if args.command != "assert-orientation":
        return
    platform = args.platform
    dylib_path = args.dylib_path
    artifact_dir = args.artifact_dir
    ndk_home = args.ndk_home

    if platform == "macos":
        if dylib_path is None:
            parser.error("--dylib-path is required for --platform macos")
        if artifact_dir is not None or ndk_home is not None:
            parser.error("--artifact-dir/--ndk-home are not accepted for --platform macos")
    elif platform == "android":
        if artifact_dir is None or ndk_home is None:
            parser.error("--artifact-dir and --ndk-home are both required for --platform android")
        if dylib_path is not None:
            parser.error("--dylib-path is not accepted for --platform android")
    else:
        if dylib_path is not None or artifact_dir is not None or ndk_home is not None:
            parser.error(
                f"--dylib-path/--artifact-dir/--ndk-home are not accepted for --platform {platform!r}"
            )


def _enforce_codec_probe_flags(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    """codec-probe only: --dist-dir is required for --platform macos
    (its HEIF dist is a per-arch matrix value, workflow context rather
    than a platform fact -- codec_probe.py's own docstring) and rejected
    everywhere else. Same reject-don't-silently-ignore posture as C-G9's
    --arch and _enforce_orientation_flags's per-platform flags: an
    optional-but-unenforced flag would accept a workflow typo/copy-paste
    (e.g. --dist-dir passed to the linux leg) and silently do nothing,
    giving the caller zero signal. codec_probe.codec_probe() itself
    already validates this exact rule in both directions (see
    `_validate_dist_dir` there), so this is a second, earlier rejection
    point -- an argparse usage error instead of a gate failure -- not a
    redundant one; both stay."""
    if args.command != "codec-probe":
        return
    platform = args.platform
    dist_dir = args.dist_dir

    if platform == "macos":
        if dist_dir is None:
            parser.error("--dist-dir is required for --platform macos")
    elif dist_dir is not None:
        parser.error(f"--dist-dir is not accepted for --platform {platform!r}")


def _enforce_capability_vector_flags(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    """capability-vector only: `--source configure-log` is accepted ONLY
    for `--platform macos` (the cross leg -- capability.py's own
    docstring/ValueError), and rejects `--dylib-path`/`--json-out` with it
    (the cross leg's configure-log check writes no JSON and needs no
    dylib path at all). `--source probe` (the default) requires
    `--dylib-path` for macOS (its `artifact_path` is `None` in targets.py,
    mirroring orientation.py's macOS shape) and rejects it for every other
    platform. Same reject-not-ignore posture as C-G9/_enforce_orientation_
    flags/_enforce_codec_probe_flags: capability.capability_vector() itself
    already raises ValueError on each of these, so this is an earlier,
    cleaner argparse-layer rejection -- not a redundant one, both stay.
    Deliberately does NOT enforce `--platform` itself (android's permanent
    exclusion is handled in dispatch(), mirroring `_CODEC_PROBE_PLATFORMS`,
    not here -- this function only concerns the source/dylib-path/json-out
    combination, which is orthogonal to which platforms exist at all)."""
    if args.command != "capability-vector":
        return
    platform = args.platform
    source = args.source
    dylib_path = args.dylib_path
    json_out = args.json_out

    if source == "configure-log":
        if platform != "macos":
            parser.error("--source configure-log is only accepted for --platform macos")
        if dylib_path is not None:
            parser.error("--dylib-path is not accepted with --source configure-log")
        if json_out is not None:
            parser.error("--json-out is not accepted with --source configure-log")
    else:  # source == "probe" (the default)
        if platform == "macos":
            if dylib_path is None:
                parser.error("--dylib-path is required for --platform macos --source probe")
        elif dylib_path is not None:
            parser.error(f"--dylib-path is not accepted for --platform {platform!r}")


def _enforce_stage_flags(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    """`stage` only: each platform names its own source-location flag, and
    every other platform's flag is REJECTED (same reject-not-ignore posture
    as every other per-platform flag set above) -- linux's `--native-dir`
    resolves via `ci/stage.py`'s original `stage()` (still reads
    `targets.spec("linux")["dist_dir"]`); windows/android's `--source-dir`
    and macos's `--dylib-path` are workflow context `stage.py` cannot know
    (R5 -- see stage.py's own module docstring for exactly what path each
    platform's real workflow step supplies today)."""
    if args.command != "stage":
        return
    platform = args.platform
    native_dir = args.native_dir
    source_dir = args.source_dir
    dylib_path = args.dylib_path

    if platform == "linux":
        if native_dir is None:
            parser.error("--native-dir is required for --platform linux")
        if source_dir is not None or dylib_path is not None:
            parser.error("--source-dir/--dylib-path are not accepted for --platform linux")
    elif platform in ("windows", "android"):
        if source_dir is None:
            parser.error(f"--source-dir is required for --platform {platform!r}")
        if native_dir is not None or dylib_path is not None:
            parser.error(
                f"--native-dir/--dylib-path are not accepted for --platform {platform!r}"
            )
    elif platform == "macos":
        if dylib_path is None:
            parser.error("--dylib-path is required for --platform macos")
        if native_dir is not None or source_dir is not None:
            parser.error("--native-dir/--source-dir are not accepted for --platform macos")


def _enforce_dt_needed_flags(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    """`dt-needed` only: `--ndk-home`/`--build-log` are android-only
    (`ci/dt_needed.py`'s `dt_needed()` keyword-only params; no windows/macOS
    branch exists there on purpose, neither platform has a DT_NEEDED step
    today). Same reject-not-ignore posture as every other per-platform flag
    set above."""
    if args.command != "dt-needed":
        return
    platform = args.platform
    if platform == "android":
        if args.ndk_home is None:
            parser.error("--ndk-home is required for --platform android")
    else:
        if args.ndk_home is not None or args.build_log is not None:
            parser.error(
                f"--ndk-home/--build-log are not accepted for --platform {platform!r}"
            )


def _enforce_assert_exports_flags(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    """`assert-exports` only: `--dylib-path` is macOS-only, `--artifact-dir`/
    `--ndk-home` are required together for android, same reject-not-ignore
    posture as `_enforce_orientation_flags` (the module's own `assert_exports()`
    already raises `ValueError` on a missing per-platform-required one)."""
    if args.command != "assert-exports":
        return
    platform = args.platform
    dylib_path = args.dylib_path
    artifact_dir = args.artifact_dir
    ndk_home = args.ndk_home

    if platform == "macos":
        if dylib_path is None:
            parser.error("--dylib-path is required for --platform macos")
        if artifact_dir is not None or ndk_home is not None:
            parser.error("--artifact-dir/--ndk-home are not accepted for --platform macos")
    elif platform == "android":
        if artifact_dir is None or ndk_home is None:
            parser.error("--artifact-dir and --ndk-home are both required for --platform android")
        if dylib_path is not None:
            parser.error("--dylib-path is not accepted for --platform android")
    else:
        if dylib_path is not None or artifact_dir is not None or ndk_home is not None:
            parser.error(
                f"--dylib-path/--artifact-dir/--ndk-home are not accepted for --platform {platform!r}"
            )


def _enforce_verify_artifact_flags(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    """`verify-artifact` only: `--dylib-path` is macOS-only (its
    `artifact_path` is `None` in targets.py), same reject-not-ignore
    posture as every other per-platform flag set above."""
    if args.command != "verify-artifact":
        return
    if args.platform == "macos":
        if args.dylib_path is None:
            parser.error("--dylib-path is required for --platform macos")
    elif args.dylib_path is not None:
        parser.error(f"--dylib-path is not accepted for --platform {args.platform!r}")


def _add_platform_command(sub, name: str, help_text: str, extra=None, with_arch: bool = True):
    sp = sub.add_parser(name, help=help_text)
    sp.add_argument("--platform", required=True)
    if with_arch:
        sp.add_argument("--arch", default=None)
    if extra:
        extra(sp)
    return sp


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="ci.py", description="ceyx unified CI entry point")
    sub = p.add_subparsers(dest="command")

    sub.add_parser("selftest", help="run native/scripts/ci/tests/ in-process")

    md = sub.add_parser("marker-diff", help="multiset-diff two marker logs (C-G1)")
    md.add_argument("--baseline", required=True)
    md.add_argument("--candidate", required=True)
    md.add_argument("--leg", default=None)

    def _verify_artifact_extra(sp):
        # macOS-only: its dylib has no static `targets.py` path (two
        # per-arch matrix legs), same shape as assert-orientation/
        # assert-exports's macOS flag. Enforced post-parse by
        # `_enforce_verify_artifact_flags`.
        sp.add_argument("--dylib-path", default=None)

    _add_platform_command(
        sub, "verify-artifact", "run the artifact verification suite", _verify_artifact_extra
    )
    _add_platform_command(sub, "import-closure", "assert the import-closure gate")
    _add_platform_command(sub, "min-runtime", "assert min-runtime drift")
    def _assert_exports_extra(sp):
        # Platform-specific, same reject-not-ignore posture as
        # assert-orientation's --dylib-path/--artifact-dir/--ndk-home:
        # macOS's dylib and android's artifact-dir/NDK are workflow context
        # `verify_artifact.py` cannot know (R5). Enforced post-parse by
        # `_enforce_assert_exports_flags`.
        sp.add_argument("--dylib-path", default=None)
        sp.add_argument("--artifact-dir", default=None)
        sp.add_argument("--ndk-home", default=None)

    _add_platform_command(
        sub, "assert-exports", "assert exported FFI symbols", _assert_exports_extra
    )
    _add_platform_command(sub, "assert-no-avx512", "assert no AVX-512 codepath")

    def _orientation_extra(sp):
        # Platform-specific, not global: --dylib-path is macOS-only,
        # --artifact-dir/--ndk-home are android-only. All three are
        # optional at the argparse level and enforced post-parse by
        # _enforce_orientation_flags() (same pattern as C-G9's --arch),
        # because orientation.assert_orientation() itself raises
        # ValueError on a missing per-platform-required one rather than
        # silently doing nothing -- a flag accepted-and-ignored on the
        # wrong platform would give a caller no signal that it did
        # nothing, so wrong-platform flags are REJECTED, not ignored.
        sp.add_argument("--dylib-path", default=None)
        sp.add_argument("--artifact-dir", default=None)
        sp.add_argument("--ndk-home", default=None)

    _add_platform_command(
        sub, "assert-orientation", "assert orientation capability", _orientation_extra
    )

    def _codec_probe_extra(sp):
        # macOS-only: its HEIF dist directory is a per-arch matrix value
        # (workflow context, not a platform fact -- codec_probe.py's own
        # docstring, divergence 1), so it cannot live in targets.py and is
        # passed in explicitly instead, exactly like android's --ndk-home
        # on assert-orientation. Optional at the argparse level and
        # enforced post-parse by _enforce_codec_probe_flags() (same
        # reject-not-ignore posture as C-G9/_enforce_orientation_flags):
        # codec_probe.codec_probe() itself already validates this in both
        # directions (ValueError-free, returns exit code 2 + report.error),
        # so the CLI layer applying the same check is a second, earlier
        # rejection point (an argparse usage error vs. a gate failure),
        # not a redundant one -- both stay.
        sp.add_argument("--workspace", required=True)
        sp.add_argument("--dist-dir", default=None)

    # codec-probe carries NO --arch at all (same shape as stage/dt-needed):
    # codec_probe.codec_probe() has no arch parameter, and the macOS
    # per-arch distinction lives entirely in --dist-dir (the workflow's own
    # matrix value), not in an --arch flag C-G9 would otherwise enforce.
    _add_platform_command(
        sub, "codec-probe", "run the functional codec probe", _codec_probe_extra, with_arch=False
    )

    def _capability_vector_extra(sp):
        # `--dylib-path`: macOS-only (source=probe), same shape as
        # assert-orientation's macOS flag -- macOS's `artifact_path` is
        # `None` in targets.py (two per-arch matrix legs), so the caller
        # passes the real path in. `--expect`/`--expect-cap`: repeatable,
        # forwarded to codec_capability_probe.py verbatim (C-G18: these
        # tokens stay in YAML so render_expectations.py's step_anchor scan
        # can attribute them to the calling step). `--json-out`: present
        # on macOS-native/windows callers, absent on linux (capability.py
        # docstring divergence 1) -- optional here, not a targets.py fact.
        sp.add_argument("--dylib-path", default=None)
        sp.add_argument("--expect", action="append", default=[])
        sp.add_argument("--expect-cap", action="append", default=[])
        sp.add_argument("--json-out", default=None)
        sp.add_argument("--workspace", default=".")
        sp.add_argument("--log-path", default="cross_stage2_build.log")

    # capability-vector carries NO --arch (same shape as codec-probe):
    # capability.capability_vector() has no `arch` parameter at all (WI-16a
    # R2, commit 36ceb53c deleted it -- an earlier revision offered it "for
    # CLI-shape uniformity, unused by the module", which was itself the
    # accept-and-ignore posture rejected for codec-probe's --dist-dir, and
    # was corrected rather than kept once flagged).
    cv = _add_platform_command(
        sub,
        "capability-vector",
        "read/derive the codec or build capability vector",
        _capability_vector_extra,
        with_arch=False,
    )
    cv.add_argument("--kind", required=True, choices=["codec", "build"])
    cv.add_argument("--source", default="probe", choices=["probe", "configure-log"])

    def _staged_companions_extra(sp):
        sp.add_argument("--dylib-path", required=True)
        sp.add_argument("--artifact-dir", required=True)

    _add_platform_command(
        sub,
        "assert-staged-companions",
        "assert the macOS staged companion dylibs (arch + reachability + rpath)",
        _staged_companions_extra,
    )

    def _stage_extra(sp):
        sp.add_argument("--artifact-dir", required=True)
        # Platform-specific source args, enforced post-parse by
        # `_enforce_stage_flags` (same reject-not-ignore posture as every
        # other per-platform flag set in this file): linux's `--native-dir`
        # is `native/build-linux`-shaped (ci/stage.py's original `stage()`,
        # still reads `targets.spec("linux")["dist_dir"]` under the hood).
        # windows/android's `--source-dir` and macos's `--dylib-path` are
        # workflow context stage.py cannot know (R5) -- ported verbatim from
        # stage.py's own module docstring: windows is
        # `native/build-windows`, android is
        # `${NATIVE_DIR}/build-android/android-arm64`, macos is
        # `dirname($DYLIB)`, all resolved by the CALLER, never by this CLI.
        sp.add_argument("--native-dir", default=None)
        sp.add_argument("--source-dir", default=None)
        sp.add_argument("--dylib-path", default=None)

    def _staged_group_extra(sp):
        sp.add_argument("--artifact-dir", required=True)

    def _dt_needed_extra(sp):
        sp.add_argument("--artifact-dir", required=True)
        sp.add_argument("--runner-temp", required=True)
        # android-only (ci/dt_needed.py's `dt_needed()` keyword-only params);
        # enforced post-parse by `_enforce_dt_needed_flags`.
        sp.add_argument("--ndk-home", default=None)
        sp.add_argument("--build-log", default=None)

    _add_platform_command(sub, "stage", "stage the built artifact + companions", _stage_extra)
    _add_platform_command(
        sub, "assert-staged-group", "assert the staged atomic group", _staged_group_extra
    )
    _add_platform_command(
        sub, "dt-needed", "assert the DT_NEEDED import closure", _dt_needed_extra
    )
    def _assert_vcpkg_artefacts_extra(sp):
        # Workflow context (R13), not targets.py facts: the triplet is a
        # matrix value and $RUNNER_TEMP is a runner-supplied path.
        sp.add_argument("--triplet", required=True)
        sp.add_argument("--runner-temp", required=True)

    _add_platform_command(
        sub,
        "assert-vcpkg-artefacts",
        "assert vcpkg produced artefacts",
        _assert_vcpkg_artefacts_extra,
        with_arch=False,
    )

    acl = sub.add_parser(
        "assert-configure-log",
        help="assert a literal/pattern line is present in a build configure log",
    )
    acl.add_argument("--log-path", required=True)
    acl.add_argument("--pattern", required=True)
    acl.add_argument("--label", required=True)
    acl.add_argument("--error", required=True)

    vb = sub.add_parser("vcpkg-baseline", help="resolve and export the vcpkg baseline")
    vb.add_argument("--github-env", required=True)

    vboot = sub.add_parser("vcpkg-bootstrap", help="bootstrap vcpkg at a pinned baseline")
    vboot.add_argument("--baseline", required=True)
    vboot.add_argument("--runner-temp", required=True)

    vi = sub.add_parser("vcpkg-install", help="vcpkg install for a triplet")
    vi.add_argument("--triplet", required=True)
    vi.add_argument("--workspace", required=True)
    vi.add_argument("--runner-temp", required=True)
    vi.add_argument("--feature", action="append", default=[])

    vint = sub.add_parser("verify-interpreter", help="refuse a hostedtoolcache interpreter")
    vint.add_argument("--forbid-hostedtoolcache", action="store_true")

    ec = sub.add_parser("ensure-cmake", help="assert a minimum CMake version")
    ec.add_argument("--min", required=True)

    bz = sub.add_parser("build-zlib", help="build zlib from a pinned, checksummed tarball")
    bz.add_argument("--version", required=True)
    bz.add_argument("--workspace", required=True)

    lcl = sub.add_parser("locate-clang-cl", help="locate clang-cl on PATH or the LLVM install dir")
    lcl.add_argument("--github-path", default=None)

    vvl = sub.add_parser("verify-vulkan-lib", help="assert vulkan-1.lib is present under VULKAN_SDK")
    vvl.add_argument("--vulkan-sdk", default="")

    return p


def _not_yet(name: str) -> int:
    print(f"::error::subcommand '{name}' is not implemented yet (P0 scaffolding)", file=sys.stderr)
    return 2


def dispatch(args: argparse.Namespace) -> int:
    if args.command == "selftest":
        return _selftest()
    if args.command == "marker-diff":
        import ci.markerdiff as markerdiff

        return markerdiff.main(
            ["--baseline", args.baseline, "--candidate", args.candidate]
            + (["--leg", args.leg] if args.leg else [])
        )
    # Push 3 (WI-7/WI-8) wires seven of these commands for Linux ONLY -- the
    # other legs' twins land in later pushes (verify-artifact: WI-19/20/21,
    # push 7). Falling through to `_not_yet()` for any other --platform
    # keeps this push honest instead of calling into a module against
    # `targets.py` data (e.g. macOS's `artifact_path: None`) that push 3
    # never populated for it. `min-runtime` is NOT one of them as of push 6
    # (WI-16b): `ci/minruntime.py` was generalised to all four platforms via
    # `targets.spec(platform)["min_runtime_source"]`, so it dispatches to its
    # own module below regardless of platform, same as `assert-orientation`
    # and `codec-probe` before it.
    # `stage`/`assert-staged-group`/`dt-needed`/`assert-exports`/
    # `verify-artifact` are NOT in this set as of push 7 (WI-22): every one
    # of their modules genuinely supports its platforms now -- confirmed
    # against the committed modules, not a report that they were done
    # (P-10's own lesson). `import-closure` stays linux-only -- android's
    # equivalent needs a caller-supplied-artifact-dir redesign
    # (`_artifact_path`/`dist_dir` are both `None` for android in
    # targets.py) not attempted yet; see verify_artifact.import_closure()'s
    # docstring.
    _linux_only_commands = {
        "import-closure",
    }
    if args.command in _linux_only_commands and getattr(args, "platform", None) != "linux":
        return _not_yet(args.command)
    # `assert-no-avx512` is PERMANENTLY linux-only (same shape as
    # `_CODEC_PROBE_PLATFORMS`/`_CAPABILITY_VECTOR_PLATFORMS`), concept-search
    # confirmed: zero `-march`/`-mtune`/`/arch:`/ISA/baseline-CPU/SIMD-shaped
    # step on any other platform's workflow, not just an absent "avx"
    # string.
    _AVX512_PLATFORMS = frozenset({"linux"})
    if args.command == "assert-no-avx512" and args.platform not in _AVX512_PLATFORMS:
        print(
            f"::error::assert-no-avx512 has no --platform {args.platform!r} leg -- this "
            "portable-baseline gate is meaningful on Linux only, permanently (concept-level "
            "search found no ISA/baseline-CPU-shaped step on any other platform's workflow)",
            file=sys.stderr,
        )
        return 2
    # `dt-needed` is explicitly narrowed to {linux, android} (not widened to
    # all four): impl-18 confirmed via `grep -l DT_NEEDED .github/workflows/
    # *.yml` that only linux_build.yml and android_build.yml have this step
    # at all -- windows/macOS are genuinely unsupported, not "not yet
    # migrated". `ci.dt_needed.dt_needed()` already returns rc=2 with a
    # clear message for any other platform, but the explicit set here (same
    # permanent-exclusion shape as `_CODEC_PROBE_PLATFORMS`) makes the
    # narrowing auditable at the CLI layer too, one step earlier -- R3's
    # pattern: narrow the gate, then prove the narrowing is narrow.
    _DT_NEEDED_PLATFORMS = frozenset({"linux", "android"})
    if args.command == "dt-needed" and args.platform not in _DT_NEEDED_PLATFORMS:
        print(
            f"::error::dt-needed has no --platform {args.platform!r} leg -- only linux and "
            "android have a DT_NEEDED step, permanently (confirmed via `grep -l DT_NEEDED "
            ".github/workflows/*.yml`)",
            file=sys.stderr,
        )
        return 2
    if args.command == "verify-artifact":
        import ci.verify_artifact as verify_artifact

        return verify_artifact.verify_artifact(
            args.platform, args.arch, dylib_path=args.dylib_path
        )
    if args.command == "import-closure":
        import ci.verify_artifact as verify_artifact

        return verify_artifact.import_closure(args.platform)
    if args.command == "min-runtime":
        import ci.minruntime as minruntime

        return minruntime.min_runtime(args.platform, args.arch)
    if args.command == "assert-exports":
        import ci.verify_artifact as verify_artifact

        return verify_artifact.assert_exports(
            args.platform,
            args.arch,
            dylib_path=args.dylib_path,
            artifact_dir=args.artifact_dir,
            ndk_home=args.ndk_home,
        )
    if args.command == "assert-no-avx512":
        import ci.verify_artifact as verify_artifact

        return verify_artifact.assert_no_avx512(args.platform)
    if args.command == "assert-staged-companions":
        import ci.verify_artifact as verify_artifact

        return verify_artifact.verify_staged_companions(
            args.platform, args.dylib_path, args.artifact_dir, args.arch
        )
    if args.command == "stage":
        import ci.stage as stage

        if args.platform == "linux":
            return stage.stage(args.platform, args.artifact_dir, args.native_dir)
        if args.platform == "windows":
            return stage.stage_windows(args.source_dir, args.artifact_dir)
        if args.platform == "android":
            return stage.stage_android(args.source_dir, args.artifact_dir)
        if args.platform == "macos":
            companions = stage.declared_companions("macos")
            return stage.stage_macos(args.dylib_path, companions, args.artifact_dir)
        return _not_yet(args.command)
    if args.command == "assert-staged-group":
        import ci.stage as stage

        if args.platform == "linux":
            return stage.assert_staged_group(args.platform, args.artifact_dir)
        if args.platform == "windows":
            return stage.assert_staged_group_windows(args.artifact_dir)
        if args.platform == "android":
            return stage.assert_staged_group_android(args.artifact_dir)
        # macos has no assert-staged-group step: its staged-group
        # verification is `assert-staged-companions` (verify_artifact.py),
        # not this command -- see verify_staged_companions()'s docstring.
        return _not_yet(args.command)
    if args.command == "dt-needed":
        import ci.dt_needed as dt_needed

        kwargs = {}
        if args.ndk_home is not None:
            kwargs["ndk_home"] = args.ndk_home
        if args.build_log is not None:
            kwargs["build_log"] = args.build_log
        return dt_needed.dt_needed(args.platform, args.artifact_dir, args.runner_temp, **kwargs)
    if args.command == "assert-orientation":
        import ci.orientation as orientation

        return orientation.assert_orientation(
            args.platform,
            arch=args.arch,
            dylib_path=args.dylib_path,
            artifact_dir=args.artifact_dir,
            ndk_home=args.ndk_home,
        )
    if args.command == "codec-probe":
        # `_CODEC_PROBE_PLATFORMS` is a PERMANENT set, unlike
        # `_linux_only_commands` above: android is not a "not yet migrated"
        # gap awaiting a future twin -- it has no `probe_codecs` step at
        # all (codec_probe.py's own docstring; plan WI-13's negative-space
        # AC). Reusing `_linux_only_commands`'s scaffold-and-shrink shape
        # here would encode a lie a future reader could "fix" by building
        # an android leg that must never exist (the same stale-meaning
        # failure C-G6 warns against for orientation). The rejection
        # message below therefore does NOT say "not yet implemented".
        if args.platform not in _CODEC_PROBE_PLATFORMS:
            print(
                f"::error::codec-probe has no --platform {args.platform!r} leg -- "
                "android has no probe_codecs step to migrate, permanently "
                "(codec_probe.py docstring, plan WI-13 negative-space AC)",
                file=sys.stderr,
            )
            return 2
        import ci.codec_probe as codec_probe

        return codec_probe.codec_probe(args.platform, args.workspace, dist_dir=args.dist_dir)
    if args.command == "assert-configure-log":
        import ci.configure_log as configure_log

        return configure_log.assert_configure_log(
            args.log_path, args.pattern, args.label, args.error
        )
    if args.command == "capability-vector":
        # `_CAPABILITY_VECTOR_PLATFORMS` is PERMANENT, same shape as
        # `_CODEC_PROBE_PLATFORMS`: android has no capability-vector step
        # at all (capability.py docstring; android_build.yml:216 is an
        # honest SKIP echo, not a probe call) -- not "not yet migrated".
        if args.platform not in _CAPABILITY_VECTOR_PLATFORMS:
            print(
                f"::error::capability-vector has no --platform {args.platform!r} leg -- "
                "android has no capability-vector step, permanently "
                "(capability.py docstring: its S-E2 step is an honest SKIP echo)",
                file=sys.stderr,
            )
            return 2
        import ci.capability as capability

        return capability.capability_vector(
            args.platform,
            args.kind,
            source=args.source,
            dylib_path=args.dylib_path,
            expect=args.expect,
            expect_cap=args.expect_cap,
            json_out=args.json_out,
            workspace=args.workspace,
            log_path=args.log_path,
        )
    if args.command == "build-zlib":
        # `build-zlib` was declared in the frozen CLI-surface docstring and
        # its subparser since WI-1/push-1 but had NO dispatch branch until
        # now -- it silently fell through to `_not_yet()` (loud rc=2, but a
        # static reader who only checks "does a subparser/docstring entry
        # exist" would wrongly conclude this command was already wired).
        # Module owner: impl-15 (zlib_build.py, push 8, `windows_toolchain`
        # sibling) -- `build_zlib(version: str) -> int`, `--version`
        # spelling frozen, do not rename either side.
        import ci.zlib_build as zlib_build

        return zlib_build.build_zlib(args.version, args.workspace)
    if args.command == "locate-clang-cl":
        import ci.windows_toolchain as windows_toolchain

        return windows_toolchain.locate_clang_cl(args.github_path)
    if args.command == "verify-vulkan-lib":
        import ci.windows_toolchain as windows_toolchain

        return windows_toolchain.verify_vulkan_lib(args.vulkan_sdk)
    if args.command == "vcpkg-baseline":
        import ci.provision as provision

        return provision.vcpkg_baseline("native/vcpkg/vcpkg.json", args.github_env)
    if args.command == "vcpkg-bootstrap":
        import ci.provision as provision

        return provision.vcpkg_bootstrap(args.baseline, args.runner_temp)
    if args.command == "vcpkg-install":
        import ci.provision as provision

        return provision.vcpkg_install(args.triplet, args.workspace, args.runner_temp, args.feature)
    if args.command == "assert-vcpkg-artefacts":
        # PERMANENT exclusion, same shape as _CODEC_PROBE_PLATFORMS: macOS's
        # real artefact-assertion shape differs (dylib + `lipo -archs`, not
        # an .so-absence glob) and is deliberately unimplemented in
        # provision.py (see its docstring) -- checked here, before the
        # call, so an unsupported platform gets a clean ::error:: + exit 2
        # instead of an uncaught ValueError traceback (found by smoke-test,
        # not by a unit test: unit tests only exercised the module function
        # directly, never the CLI's own rejection path).
        _VCPKG_ARTEFACT_PLATFORMS = frozenset({"linux"})
        if args.platform not in _VCPKG_ARTEFACT_PLATFORMS:
            print(
                f"::error::assert-vcpkg-artefacts has no --platform {args.platform!r} leg -- "
                "only 'linux' is ported (macOS's real check asserts a different shape, see "
                "provision.py's module docstring)",
                file=sys.stderr,
            )
            return 2
        import ci.provision as provision

        return provision.assert_vcpkg_artefacts(args.platform, args.triplet, args.runner_temp)
    if args.command == "verify-interpreter":
        import ci.provision as provision

        return provision.verify_interpreter(args.forbid_hostedtoolcache)
    if args.command == "ensure-cmake":
        import ci.provision as provision

        return provision.ensure_cmake(args.min)
    if args.command in _PLATFORM_COMMANDS or args.command in _PLATFORMLESS_COMMANDS:
        return _not_yet(args.command)
    return 2


def _selftest() -> int:
    import unittest

    tests_dir = Path(__file__).resolve().parent / "ci" / "tests"
    loader = unittest.TestLoader()
    suite = loader.discover(str(tests_dir), top_level_dir=str(Path(__file__).resolve().parent))
    result = unittest.TextTestRunner(verbosity=0).run(suite)
    print(
        f"CI-SELFTEST: tests={result.testsRun} "
        f"failures={len(result.failures)} errors={len(result.errors)}"
    )
    return 0 if result.wasSuccessful() else 1


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command is None:
        parser.print_help()
        return 2
    if args.command in _PLATFORM_COMMANDS:
        _enforce_arch_requirement(parser, args)
        _enforce_orientation_flags(parser, args)
        _enforce_codec_probe_flags(parser, args)
        _enforce_capability_vector_flags(parser, args)
        _enforce_stage_flags(parser, args)
        _enforce_dt_needed_flags(parser, args)
        _enforce_assert_exports_flags(parser, args)
        _enforce_verify_artifact_flags(parser, args)
    return dispatch(args)


if __name__ == "__main__":
    sys.exit(main())
