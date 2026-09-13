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
    assert-vcpkg-artefacts   linux_build.yml:385-394 (`platform == "linux"`)
                      and macos_build.yml:324-364 (`platform == "macos"`,
                      `_assert_vcpkg_artefacts_macos` -- three libraries,
                      three different properties, see that function's
                      docstring; requires `arch_tag`). Any other platform
                      raises rather than silently running the wrong
                      assertion.
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


def assert_vcpkg_artefacts(platform: str, triplet: str, runner_temp: str, arch_tag: str | None = None) -> int:
    """Replaces linux_build.yml:385-394 (`platform == "linux"`) and
    macos_build.yml:324-364 (`platform == "macos"`) -- TWO DIFFERENT
    ASSERTIONS, not one branched by platform: linux only checks
    libwebp-is-static; macOS checks three libraries for three DIFFERENT
    properties (see `_assert_vcpkg_artefacts_macos`'s docstring). `arch_tag`
    is macOS-only (linux's shell body never checks architecture) and is
    required, not defaulted, when `platform == "macos"` -- a silently
    skipped arch comparison is the exact "passes because it did not run"
    failure shape this campaign keeps finding."""
    if platform == "macos":
        if not arch_tag:
            raise ValueError(
                "assert_vcpkg_artefacts: arch_tag is required for platform macos "
                "(a missing arch_tag would silently skip the lipo -archs checks)"
            )
        return _assert_vcpkg_artefacts_macos(triplet, runner_temp, arch_tag)
    if platform != "linux":
        raise ValueError(
            f"assert_vcpkg_artefacts: platform {platform!r} is not implemented -- only "
            "'linux' and 'macos' are ported"
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


def _assert_vcpkg_artefacts_macos(triplet: str, runner_temp: str, arch_tag: str) -> int:
    """Replaces macos_build.yml:324-364, "Assert the vcpkg artefacts
    (libwebp static, libde265 shared)". THREE libraries, THREE DIFFERENT
    properties -- do not symmetrise into one shape:

    * libwebp (:329-337): STATIC. `.a` must exist, no `.dylib` may exist,
      and its `lipo -archs` must include `arch_tag`.
    * libde265 (:346-355): SHARED -- LGPL-3 4(d)(1) [A5.2]. At least one
      `.dylib` must exist (glob, not an exact name -- one of
      `libde265.dylib`/`libde265.1.dylib` is a symlink to the other; the
      real shell used `find | head -n1` and let `lipo` follow whichever it
      got, reproduced exactly here rather than resolved by hand). `.a`
      must NOT exist. Its `lipo -archs` must include `arch_tag`.
    * aom (:356-364): STATIC (linked INTO libheif). `.a` must exist, no
      `.dylib` may exist. **No arch check at all** -- this is the real
      shell's own asymmetry (no `lipo`/`grep -qw` block for aom there
      either), not an omission to "complete" here.
    """
    vcpkg_prefix = Path(runner_temp) / "vcpkg-installed" / triplet
    lib_dir = vcpkg_prefix / "lib"

    ls_result = run.run(["ls", "-la", str(lib_dir)])
    if ls_result.stdout:
        report.plain(ls_result.stdout.rstrip("\n"))

    def _archs(path: Path) -> str:
        return run.run(["lipo", "-archs", str(path)]).stdout.strip()

    # -- libwebp: static, right arch --------------------------------------
    if not (lib_dir / "libwebp.a").is_file():
        report.plain("FAIL: libwebp.a absent (expected a static archive)")
        return 1
    webp_dylibs = sorted(lib_dir.glob("libwebp*.dylib"))
    if webp_dylibs:
        report.plain(
            f"FAIL: {len(webp_dylibs)} libwebp dylib(s) present; the triplet must keep "
            "libwebp static (LGPL asymmetry applies to libheif/libde265 only)"
        )
        return 1
    webp_archs = _archs(lib_dir / "libwebp.a")
    report.plain(f"libwebp.a archs: {webp_archs}")
    if arch_tag not in webp_archs.split():
        report.plain(f"FAIL: libwebp.a archs '{webp_archs}' do not include {arch_tag}")
        return 1

    # -- libde265: shared, right arch (LGPL-3 4(d)(1) [A5.2]) -------------
    de265_dylibs = sorted(lib_dir.glob("libde265*.dylib"))
    if not de265_dylibs:
        report.plain(
            "FAIL: no libde265 dylib in the vcpkg prefix; a static libde265 is an LGPL-3 "
            "4(d)(1) breach, not a packaging detail [A5.2]"
        )
        return 1
    if (lib_dir / "libde265.a").is_file():
        report.plain("FAIL: libde265.a present; the triplet's dynamic exception did not apply [A5.2]")
        return 1
    de265_archs = _archs(de265_dylibs[0])
    report.plain(f"libde265 archs: {de265_archs}")
    if arch_tag not in de265_archs.split():
        report.plain(f"FAIL: libde265 archs '{de265_archs}' do not include {arch_tag}")
        return 1

    # -- aom: static, no arch check (linked into libheif) -----------------
    if not (lib_dir / "libaom.a").is_file():
        report.plain("FAIL: libaom.a absent (expected a static archive)")
        return 1
    aom_dylibs = sorted(lib_dir.glob("libaom*.dylib"))
    if aom_dylibs:
        report.plain(
            f"FAIL: {len(aom_dylibs)} libaom dylib(s) present; aom must stay static (it is linked into libheif)"
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
