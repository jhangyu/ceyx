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
    python3 native/scripts/ci.py verify-artifact   --platform P [--arch A]
    python3 native/scripts/ci.py import-closure    --platform P
    python3 native/scripts/ci.py min-runtime       --platform P [--arch A]
    python3 native/scripts/ci.py assert-exports    --platform P [--arch A]
    python3 native/scripts/ci.py assert-no-avx512  --platform P
    python3 native/scripts/ci.py assert-orientation --platform P
    python3 native/scripts/ci.py codec-probe       --platform P --workspace W [--dist-dir D]
    python3 native/scripts/ci.py capability-vector --platform P --kind codec|build [--source probe|configure-log]
    python3 native/scripts/ci.py stage             --platform P
    python3 native/scripts/ci.py assert-staged-group --platform P
    python3 native/scripts/ci.py dt-needed         --platform P
    python3 native/scripts/ci.py vcpkg-baseline    --github-env PATH
    python3 native/scripts/ci.py vcpkg-bootstrap   --baseline SHA
    python3 native/scripts/ci.py vcpkg-install     --triplet T
    python3 native/scripts/ci.py assert-vcpkg-artefacts --platform P [--arch A]
    python3 native/scripts/ci.py verify-interpreter --forbid-hostedtoolcache
    python3 native/scripts/ci.py ensure-cmake      --min 3.28
    python3 native/scripts/ci.py build-zlib        --version 1.3.1

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

# Subcommands that never take --platform at all.
_PLATFORMLESS_COMMANDS = (
    "selftest",
    "marker-diff",
    "vcpkg-baseline",
    "vcpkg-bootstrap",
    "vcpkg-install",
    "verify-interpreter",
    "ensure-cmake",
    "build-zlib",
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

    _add_platform_command(sub, "verify-artifact", "run the artifact verification suite")
    _add_platform_command(sub, "import-closure", "assert the import-closure gate")
    _add_platform_command(sub, "min-runtime", "assert min-runtime drift")
    _add_platform_command(sub, "assert-exports", "assert exported FFI symbols")
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

    cv = _add_platform_command(
        sub, "capability-vector", "read/derive the codec or build capability vector"
    )
    cv.add_argument("--kind", required=True, choices=["codec", "build"])
    cv.add_argument("--source", default=None, choices=["probe", "configure-log"])

    def _stage_extra(sp):
        sp.add_argument("--artifact-dir", required=True)
        sp.add_argument("--native-dir", required=True)

    def _staged_group_extra(sp):
        sp.add_argument("--artifact-dir", required=True)

    def _dt_needed_extra(sp):
        sp.add_argument("--artifact-dir", required=True)
        sp.add_argument("--runner-temp", required=True)

    _add_platform_command(sub, "stage", "stage the built artifact + companions", _stage_extra)
    _add_platform_command(
        sub, "assert-staged-group", "assert the staged atomic group", _staged_group_extra
    )
    _add_platform_command(
        sub, "dt-needed", "assert the DT_NEEDED import closure", _dt_needed_extra
    )
    _add_platform_command(sub, "assert-vcpkg-artefacts", "assert vcpkg produced artefacts")

    vb = sub.add_parser("vcpkg-baseline", help="resolve and export the vcpkg baseline")
    vb.add_argument("--github-env", required=True)

    vboot = sub.add_parser("vcpkg-bootstrap", help="bootstrap vcpkg at a pinned baseline")
    vboot.add_argument("--baseline", required=True)

    vi = sub.add_parser("vcpkg-install", help="vcpkg install for a triplet")
    vi.add_argument("--triplet", required=True)

    vint = sub.add_parser("verify-interpreter", help="refuse a hostedtoolcache interpreter")
    vint.add_argument("--forbid-hostedtoolcache", action="store_true")

    ec = sub.add_parser("ensure-cmake", help="assert a minimum CMake version")
    ec.add_argument("--min", required=True)

    bz = sub.add_parser("build-zlib", help="build zlib from a pinned, checksummed tarball")
    bz.add_argument("--version", required=True)

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
    # Push 3 (WI-7/WI-8) wires these eight commands for Linux ONLY -- the
    # other legs' twins land in later pushes (verify-artifact: WI-19/20/21,
    # push 7). Falling through to `_not_yet()` for any other --platform
    # keeps this push honest instead of calling into a module against
    # `targets.py` data (e.g. macOS's `artifact_path: None`) that push 3
    # never populated for it.
    _linux_only_commands = {
        "verify-artifact",
        "import-closure",
        "min-runtime",
        "assert-exports",
        "assert-no-avx512",
        "stage",
        "assert-staged-group",
        "dt-needed",
    }
    if args.command in _linux_only_commands and getattr(args, "platform", None) != "linux":
        return _not_yet(args.command)
    if args.command == "verify-artifact":
        import ci.verify_artifact as verify_artifact

        return verify_artifact.verify_artifact(args.platform, args.arch)
    if args.command == "import-closure":
        import ci.verify_artifact as verify_artifact

        return verify_artifact.import_closure(args.platform)
    if args.command == "min-runtime":
        import ci.verify_artifact as verify_artifact

        return verify_artifact.min_runtime(args.platform, args.arch)
    if args.command == "assert-exports":
        import ci.verify_artifact as verify_artifact

        return verify_artifact.assert_exports(args.platform, args.arch)
    if args.command == "assert-no-avx512":
        import ci.verify_artifact as verify_artifact

        return verify_artifact.assert_no_avx512(args.platform)
    if args.command == "stage":
        import ci.stage as stage

        return stage.stage(args.platform, args.artifact_dir, args.native_dir)
    if args.command == "assert-staged-group":
        import ci.stage as stage

        return stage.assert_staged_group(args.platform, args.artifact_dir)
    if args.command == "dt-needed":
        import ci.dt_needed as dt_needed

        return dt_needed.dt_needed(args.platform, args.artifact_dir, args.runner_temp)
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
    return dispatch(args)


if __name__ == "__main__":
    sys.exit(main())
