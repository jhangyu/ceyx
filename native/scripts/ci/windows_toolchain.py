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
