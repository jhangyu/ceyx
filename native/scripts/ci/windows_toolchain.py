"""Windows per-runner toolchain locate/verify checks (WI-24, pyci
python-ization campaign).

Two unrelated single-purpose steps, transcribed verbatim from
windows_build.yml's own body text (ae56dc82): locating clang-cl on PATH (or
its LLVM/bin fallback), and verifying the Vulkan SDK's import library is
present. Deliberately NOT part of zlib_build.py -- R12/P-10's lesson
(a module whose behaviour cannot be predicted from its name is the exact
defect this campaign keeps deleting) applies just as much to a Windows
grab-bag module as it did to the original stage.py.
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

from . import report, run

_FALLBACK_CLANG_CL = "/c/Program Files/LLVM/bin/clang-cl.exe"
# Emitted VERBATIM into $GITHUB_PATH on the fallback branch. Inside the
# original shell's double-quoted `echo "C:\\Program Files\\LLVM\\bin"`,
# `\\` collapses to a single backslash under bash's own quoting rules, so
# the line actually written to disk is a single-backslash Windows path --
# not the doubled form the shell SOURCE reads as.
_FALLBACK_PATH_EXPORT = "C:\\Program Files\\LLVM\\bin"


def locate_clang_cl(github_path: str | None = None) -> int:
    """Replaces windows_build.yml:119-132.

    Returns the RC of the final `clang-cl --version` invocation (the shell
    body has no `set -e`, so its own exit status is whatever that last
    command's status is), or 1 if clang-cl could not be located at all by
    either PATH lookup or the LLVM/bin fallback."""
    clang_cl = shutil.which("clang-cl")
    if not clang_cl:
        if os.access(_FALLBACK_CLANG_CL, os.X_OK):
            clang_cl = _FALLBACK_CLANG_CL
            # Refuse a silent no-op export rather than swallow it, same
            # discipline as report.github_env_append's own refusal for an
            # empty/missing $GITHUB_ENV path -- in real CI this argument is
            # always populated by the platform, so this branch is a
            # caller-configuration guard, not a behaviour the pre-migration
            # shell could ever exercise (it always had a real $GITHUB_PATH).
            if not github_path:
                report.error(
                    "locate_clang_cl: clang-cl found only via the LLVM/bin fallback but "
                    "no github_path was given to append it to PATH."
                )
                return 1
            with open(github_path, "a", encoding="utf-8") as fh:
                fh.write(_FALLBACK_PATH_EXPORT + "\n")
        else:
            report.error(
                "clang-cl not found on the runner. native/cmake/pipeline.cmake requires "
                "it (cl.exe has no -ffp-contract=off equivalent)."
            )
            return 1
    report.plain(f"clang-cl: {clang_cl}")
    result = run.run([clang_cl, "--version"])
    if result.stdout:
        report.plain(result.stdout.rstrip("\n"))
    if result.stderr:
        report.plain(result.stderr.rstrip("\n"))
    return result.returncode


def verify_vulkan_lib(vulkan_sdk: str = "") -> int:
    """Replaces windows_build.yml:208-217.

    Returns 1 (matching the shell's explicit `exit 1`) if vulkan-1.lib is
    missing under `<vulkan_sdk>/Lib`, else the RC of the final `ls -la`
    (matching the shell body's no-`set -e` "exit status of the last
    command" behaviour)."""
    report.plain(f"VULKAN_SDK={vulkan_sdk or '<unset>'}")
    lib_dir = f"{vulkan_sdk}/Lib"
    lib_path = Path(lib_dir) / "vulkan-1.lib"
    if not lib_path.is_file():
        report.error(
            "vulkan-1.lib missing under VULKAN_SDK; cmake/ffi.cmake's WIN32 branch "
            "would fail configure."
        )
        # PORTED AS-IS: the shell's `ls -la ... || true` swallows a failing
        # listing (e.g. the Lib dir itself missing) -- it never gates the
        # return code, which is the explicit `exit 1` right after it.
        ls_result = run.run(["ls", "-la", lib_dir])
        if ls_result.stdout:
            report.plain(ls_result.stdout.rstrip("\n"))
        return 1
    ls_result = run.run(["ls", "-la", str(lib_path)])
    if ls_result.stdout:
        report.plain(ls_result.stdout.rstrip("\n"))
    return ls_result.returncode


# --- windows-arm64 leg (2026-09-30, contract armci-contract.md R-9) --------

_VSWHERE = "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"


def assert_vs_component(component: str, out_path: str = "vswhere_component.txt") -> int:
    """Hard-assert a Visual Studio component is installed (vswhere -requires).

    The windows-11-arm image publishes no VS component list, so the ARM64 VC
    tools are asserted up front rather than surfacing later as a link error
    naming something else. Output goes to a FILE and is read back (no pipe);
    an EMPTY answer is a failure, and the full instance listing is printed
    for diagnosis."""
    rc = run.run_to_file(
        [_VSWHERE, "-products", "*", "-requires", component, "-property", "installationPath"],
        out_path,
    ).returncode
    text = Path(out_path).read_text(errors="replace").strip() if Path(out_path).is_file() else ""
    report.marker("VSWHERE_RC", rc)
    report.plain(f"vswhere -requires {component}: {text or '<empty>'}")
    if rc != 0 or not text:
        report.error(f"no Visual Studio instance provides {component} on this runner.")
        listing = run.run([_VSWHERE, "-products", "*", "-all", "-format", "text"])
        report.plain((listing.stdout + listing.stderr).rstrip("\n"))
        return 1
    return 0


# The generator host triple on windows-11-arm. Halide v21.0.0 publishes no
# arm-64-windows HOST dist (release assets: x86-64-windows / x86-32-windows),
# so stage 1 builds x86_64 generators (against the x86-64-windows Halide dist)
# that run under Windows-on-ARM x64 emulation. clang-cl on that image is the
# native arm64 LLVM, hence the explicit --target. Passed through CFLAGS/
# CXXFLAGS, which CMake folds into CMAKE_<LANG>_FLAGS together with its MSVC
# defaults (/DWIN32 /D_WINDOWS /EHsc /GR); a -DCMAKE_CXX_FLAGS on the command
# line would REPLACE those defaults (pitfall N16).
_STAGE1_HOST_TARGET = "--target=x86_64-pc-windows-msvc"


def cross_stage1(build_dir: str, aot_target: str) -> int:
    """Cross stage 1 on the Windows arm64 row: build the x86_64 Halide
    generators and run them (target dng_all_aot) with
    DNG_AOT_TARGET_OVERRIDE=<aot_target>. Same DNG_HOST_GENERATORS_ONLY
    mechanism as macos_build.yml's cross stage 1; the AOT output lands in
    <build_dir>/halide_generated, which stage 2 consumes via
    DNG_PREBUILT_AOT_DIR in the same job. Configure and build logs are
    written to files (cross_stage1_configure.log / cross_stage1_build.log)
    and replayed; each RC is emitted as its own marker."""
    env = dict(os.environ)
    env["CFLAGS"] = _STAGE1_HOST_TARGET
    env["CXXFLAGS"] = _STAGE1_HOST_TARGET
    configure = [
        "cmake", "-S", "native", "-B", build_dir, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=clang-cl",
        "-DCMAKE_CXX_COMPILER=clang-cl",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
        "-DDNG_HOST_GENERATORS_ONLY=ON",
        f"-DDNG_AOT_TARGET_OVERRIDE={aot_target}",
    ]
    rc = run.run_to_file(configure, "cross_stage1_configure.log", env=env).returncode
    report.plain(Path("cross_stage1_configure.log").read_text(errors="replace").rstrip("\n"))
    report.marker("CROSS_STAGE1_CONFIGURE_RC", rc)
    if rc != 0:
        return rc
    build = ["cmake", "--build", build_dir, "--target", "dng_all_aot", "--", "-v", "-k", "0"]
    rc = run.run_to_file(build, "cross_stage1_build.log", env=env).returncode
    report.plain(Path("cross_stage1_build.log").read_text(errors="replace").rstrip("\n"))
    report.marker("CROSS_STAGE1_RC", rc)
    aot_dir = Path(build_dir) / "halide_generated"
    listing = sorted(p.name for p in aot_dir.iterdir()) if aot_dir.is_dir() else []
    report.plain(f"--- AOT artifacts in {aot_dir}: {len(listing)}")
    for name in listing:
        report.plain(name)
    if rc == 0 and not listing:
        report.error(f"stage 1 reported success but {aot_dir} is empty -- nothing for stage 2 to link.")
        return 1
    return rc
