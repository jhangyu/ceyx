"""Toolchain provisioning: vcpkg baseline/bootstrap/install/artefact-assert,
interpreter sanity, and a minimum-CMake-version gate.

Frozen interface (docs/logs/2026-09-13/pyci-plan.md WI-1's original ci.py
docstring; these six subcommands were declared and scaffolded to
`_not_yet()` at P0, never implemented until push 8's WI-22):

    vcpkg-baseline    linux_build.yml:124-127
    vcpkg-bootstrap   linux_build.yml:360-365 (identical body on macOS,
                      macos_build.yml:300-306 -- same commands, no
                      per-platform branch needed)
    vcpkg-install     linux_build.yml:367-379 (macOS's equivalent,
                      macos_build.yml:307-319, differs only in the
                      workflow-supplied triplet/feature values -- CLI
                      pass-through, R13, not a branch here)
    assert-vcpkg-artefacts   linux_build.yml:385-394 ONLY, ported here.
                      macOS's equivalent (macos_build.yml:330-345) asserts
                      a DIFFERENT shape (dylib/lipo-arch, not .so-absence)
                      and is NOT implemented here -- `platform` is accepted
                      and validated but only `"linux"` has a real branch;
                      calling this for macOS raises rather than silently
                      running the wrong assertion.
    verify-interpreter    linux_build.yml:185-198 (the hostedtoolcache-glibc
                      mismatch check; container-specific, but the check
                      itself has no platform branch of its own)
    ensure-cmake      linux_build.yml:312-317

R13 (workflow context is a CLI pass-through, never a `targets.py` key):
every path here (`$RUNNER_TEMP`, `$GITHUB_WORKSPACE`, the manifest root,
the triplet, the install root) is workflow-supplied and taken as an
explicit parameter; nothing in this module reads an environment variable
for that purpose. Tool NAMES would be a `targets.py` fact if this module
ever needed one -- it doesn't; every command here invokes `vcpkg`/`cmake`/
`python`/`pip` by their fixed, cross-platform names.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

from . import report, run


def vcpkg_baseline(vcpkg_json_path: str, github_env_path: str) -> int:
    """Replaces linux_build.yml:124-127 (identical shape wherever it
    appears -- reads the single source of truth, `vcpkg.json`'s
    `builtin-baseline`, and exports it for later steps in the same job)."""
    data = json.loads(Path(vcpkg_json_path).read_text())
    baseline = data["builtin-baseline"]
    report.github_env_append(github_env_path, "VCPKG_BASELINE", baseline)
    return 0


def vcpkg_bootstrap(baseline: str, runner_temp: str) -> int:
    """Replaces linux_build.yml:360-365 / macos_build.yml:300-306 --
    IDENTICAL shell body on both platforms (clone, checkout the pinned
    baseline, bootstrap, print version), no per-platform branch needed."""
    vcpkg_dir = str(Path(runner_temp) / "vcpkg")
    result = run.run(["git", "clone", "https://github.com/microsoft/vcpkg.git", vcpkg_dir])
    if result.stdout:
        report.plain(result.stdout.rstrip("\n"))
    if result.returncode != 0:
        report.error(f"git clone of vcpkg failed (rc={result.returncode}).")
        return result.returncode

    result = run.run(["git", "-C", vcpkg_dir, "checkout", baseline])
    if result.stdout:
        report.plain(result.stdout.rstrip("\n"))
    if result.returncode != 0:
        report.error(f"git checkout {baseline} failed in {vcpkg_dir} (rc={result.returncode}).")
        return result.returncode

    result = run.run([str(Path(vcpkg_dir) / "bootstrap-vcpkg.sh"), "-disableMetrics"])
    if result.stdout:
        report.plain(result.stdout.rstrip("\n"))
    if result.returncode != 0:
        report.error(f"bootstrap-vcpkg.sh failed (rc={result.returncode}).")
        return result.returncode

    result = run.run([str(Path(vcpkg_dir) / "vcpkg"), "version"])
    if result.stdout:
        report.plain(result.stdout.rstrip("\n"))
    return result.returncode


def vcpkg_install(
    triplet: str,
    workspace: str,
    runner_temp: str,
    features: list[str],
) -> int:
    """Replaces linux_build.yml:367-379 / macos_build.yml:307-319 --
    IDENTICAL shape, differing only in the caller-supplied `triplet` and
    `features` (workflow matrix values, R13 -- not a platform branch).
    `--x-no-default-features` + explicit `--x-feature=...` is transcribed
    verbatim, matching both real call sites' argv shape."""
    vcpkg_bin = str(Path(runner_temp) / "vcpkg" / "vcpkg")
    manifest_root = str(Path(workspace) / "native" / "vcpkg")
    install_root = str(Path(runner_temp) / "vcpkg-installed")
    argv = [
        vcpkg_bin, "install",
        f"--x-manifest-root={manifest_root}",
        f"--x-install-root={install_root}",
        f"--triplet={triplet}",
        "--x-no-default-features",
        *[f"--x-feature={f}" for f in features],
        "--no-print-usage",
    ]
    result = run.run(argv)
    if result.stdout:
        report.plain(result.stdout.rstrip("\n"))
    if result.stderr:
        report.plain(result.stderr.rstrip("\n"))
    report.marker("VCPKG_INSTALL_RC", result.returncode)
    return result.returncode


def assert_vcpkg_artefacts(platform: str, triplet: str, runner_temp: str) -> int:
    """Replaces linux_build.yml:385-394 ONLY. `platform` is validated, not
    branched-and-ignored: macOS's equivalent (macos_build.yml:330-345)
    asserts a materially different shape (a dylib + `lipo -archs`
    architecture check, not a shared-object-absence glob) and is
    deliberately NOT implemented here -- calling this for macOS would
    silently run the wrong assertion and report a false PASS/FAIL rather
    than the real one. Add a `_assert_vcpkg_artefacts_macos` sibling
    function (same shape as this module's other per-platform splits) when
    a macOS caller actually needs it; do not fold it into this function's
    `.so`-glob logic."""
    if platform != "linux":
        raise ValueError(
            f"assert_vcpkg_artefacts: platform {platform!r} is not implemented -- only "
            "'linux' is ported (macOS's real check asserts a different shape, see docstring)"
        )
    vcpkg_prefix = Path(runner_temp) / "vcpkg-installed" / triplet
    lib_dir = vcpkg_prefix / "lib"

    ls_result = run.run(["ls", "-la", str(lib_dir)])
    if ls_result.stdout:
        report.plain(ls_result.stdout.rstrip("\n"))

    static_lib = lib_dir / "libwebp.a"
    if not static_lib.is_file():
        report.plain("FAIL: libwebp.a absent (expected a static archive)")
        return 1

    shared = sorted(lib_dir.glob("libwebp*.so*"))
    if shared:
        report.plain(
            f"FAIL: {len(shared)} libwebp shared object(s) present; the triplet must keep "
            "libwebp static (the LGPL dynamic exception covers libheif/libde265 only)"
        )
        return 1
    return 0


def verify_interpreter(forbid_hostedtoolcache: bool) -> int:
    """Replaces linux_build.yml:185-198: refuses a hostedtoolcache
    interpreter (glibc-version mismatch inside the container, CI run
    34703203982), then proves the resolved interpreter is alive and has
    tomllib/json. `forbid_hostedtoolcache` mirrors the shell's own
    conditional shape -- it is always `True` in the real call site, but the
    parameter exists so a test/future caller can exercise the "allowed"
    path without monkeypatching `sys.executable`'s resolved path."""
    resolved = run.run(["command", "-v", "python3"])
    resolved_path = resolved.stdout.strip() if resolved.returncode == 0 else sys.executable
    report.plain(f"python3 resolves to: {resolved_path}")

    if forbid_hostedtoolcache and (
        resolved_path.startswith("/__t/") or resolved_path.startswith("/opt/hostedtoolcache/")
    ):
        report.error(
            f"python3 resolved to {resolved_path}, a hostedtoolcache binary. Those are linked "
            "against the ubuntu-24.04 host's glibc 2.38 and cannot run in this glibc 2.35 "
            "container (CI run 34703203982)."
        )
        return 1

    version_info = sys.version_info[:2]
    if version_info < (3, 11):
        report.error(f"interpreter too old: {sys.version.split()[0]} (need >= 3.11)")
        return 1
    report.plain(f"interpreter alive: {sys.version.split()[0]} | tomllib+json OK")
    return 0


def ensure_cmake(min_version: str) -> int:
    """Replaces linux_build.yml:312-317: installs a pinned-minimum CMake
    via pip (the container ships 3.22, Halide 21 needs >= 3.28), then
    re-parses `cmake --version`'s own output to prove the PATH resolution
    -- not just the pip install -- actually landed a new-enough binary."""
    pip_result = run.run([sys.executable, "-m", "pip", "install",
                           "--disable-pip-version-check", f"cmake>={min_version}"])
    if pip_result.stdout:
        report.plain(pip_result.stdout.rstrip("\n"))
    if pip_result.returncode != 0:
        report.error(f"pip install cmake>={min_version} failed (rc={pip_result.returncode}).")
        return pip_result.returncode

    version_result = run.run(["cmake", "--version"])
    if version_result.stdout:
        report.plain(version_result.stdout.rstrip("\n"))
    if version_result.returncode != 0:
        report.error(f"cmake --version failed (rc={version_result.returncode}).")
        return version_result.returncode

    match = re.search(r"(\d+)\.(\d+)", version_result.stdout.split()[2]) if len(
        version_result.stdout.split()
    ) > 2 else None
    if not match:
        report.error(f"could not parse a version out of cmake --version's output: {version_result.stdout!r}")
        return 1
    found = (int(match.group(1)), int(match.group(2)))
    wanted = tuple(int(x) for x in min_version.split(".")[:2])
    if found < wanted:
        report.error(f"cmake on PATH is older than {min_version}; Halide 21 will not configure.")
        return 1
    return 0
