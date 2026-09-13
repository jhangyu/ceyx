"""Windows zlib toolchain build (WI-24, pyci python-ization campaign).

Replaces windows_build.yml's "Build zlib 1.3.1 (static, /MT) for the
Windows toolchain" step (ae56dc82:311-346). RawSpeed3's CheckZLIB
(cmake/Modules/CheckZLIB.cmake:57-66) LINK-tests uncompress()/zError() at
CMake configure time, which a find-module shim cannot satisfy against
CMake's own FetchContent zlib target (no .lib exists until BUILD time) --
see the pre-migration step's own comment block for the full root-cause
trail (run 33176207480: every header/prototype check passed, only the two
link checks failed). This module builds a REAL static zlib and installs it
to ``<workspace>/zlib-install``, matching third_party.cmake:44-45's own pin
(same version, same sha256, same /MT runtime) so the two do not fork.

Transcribed verbatim except one intentional, semantics-preserving carrier
swap: sha256 verification uses ``hashlib`` instead of spawning
``sha256sum -c`` -- same algorithm, same expected digest, no subprocess
needed for a computation Python already does natively. curl/tar/cmake
remain real subprocess calls with the identical argv the shell used,
because re-implementing tar extraction or driving a CMake build from
Python would be a real behaviour change, not a carrier swap.

``set -e`` semantics preserved: the ORIGINAL shell aborts at the first
failing command with THAT command's own exit code (never a synthesized 1),
and the ``sha256sum -c`` step's "SHA_RC=$?" marker never prints on a
mismatch because the abort happens before that echo line runs -- both are
replicated here: each step below returns early with the failing
operation's own returncode, and no marker line is printed on a failure
path that had none in the shell.
"""

from __future__ import annotations

from pathlib import Path
import hashlib

from . import report, run

# Same version and same sha256 as third_party.cmake:44-45 and the
# pre-migration shell (windows_build.yml:316-317) -- do not fork this pin.
_PINS = {
    "1.3.1": {
        "url": "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz",
        "sha256": "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23",
    },
}

# Hard-required artifacts everything downstream depends on, transcribed
# verbatim from the shell's own `for required in ...` list
# (windows_build.yml:340).
_REQUIRED_INSTALLED_FILES = ("include/zlib.h", "include/zconf.h", "lib/zlibstatic.lib")


def _print_output(result) -> None:
    if result.stdout:
        report.plain(result.stdout.rstrip("\n"))
    if result.stderr:
        report.plain(result.stderr.rstrip("\n"))


def build_zlib(version: str, workspace: str) -> int:
    """Fetch, checksum, build (Ninja + clang-cl, static /MT) and install a
    pinned zlib release under ``<workspace>/zlib-install``.

    Returns the first failing operation's own return code (matching the
    shell's `set -e` abort-with-that-command's-RC semantics), or 0 once
    every required installed file is verified present. Raises
    ``ValueError`` for a version with no pinned sha256 -- a configuration
    error, not a runtime failure the shell could ever have hit (the version
    was a literal in the shell, never a variable)."""
    if version not in _PINS:
        raise ValueError(f"build_zlib: no pinned sha256 for zlib version {version!r}")
    pin = _PINS[version]

    workspace_path = Path(workspace)
    src_dir = workspace_path / "zlib-src"
    build_dir = workspace_path / "zlib-build"
    install_dir = workspace_path / "zlib-install"
    tarball = workspace_path / "zlib.tar.gz"

    src_dir.mkdir(parents=True, exist_ok=True)

    curl_result = run.run(
        ["curl", "-fsSL", "-o", str(tarball), pin["url"]], cwd=str(workspace_path)
    )
    _print_output(curl_result)
    if curl_result.returncode != 0:
        return curl_result.returncode

    digest = hashlib.sha256(tarball.read_bytes()).hexdigest()
    if digest != pin["sha256"]:
        # PORTED AS-IS: `sha256sum -c` failing under `set -e` aborts BEFORE
        # the shell's "SHA_RC=$?" echo ever runs -- no marker line here.
        report.plain(f"{tarball.name}: FAILED")
        report.plain(f"expected sha256 {pin['sha256']}, got {digest}")
        return 1
    report.marker("SHA_RC", 0)

    tar_result = run.run(
        ["tar", "-xzf", str(tarball), "-C", str(src_dir), "--strip-components=1"],
        cwd=str(workspace_path),
    )
    _print_output(tar_result)
    if tar_result.returncode != 0:
        return tar_result.returncode

    configure_result = run.run(
        [
            "cmake", "-S", str(src_dir), "-B", str(build_dir), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_C_COMPILER=clang-cl",
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
            f"-DCMAKE_INSTALL_PREFIX={install_dir}",
            "-DZLIB_BUILD_EXAMPLES=OFF",
        ],
        cwd=str(workspace_path),
    )
    _print_output(configure_result)
    if configure_result.returncode != 0:
        return configure_result.returncode

    build_result = run.run(
        ["cmake", "--build", str(build_dir), "--target", "install", "--", "-k", "0"],
        cwd=str(workspace_path),
    )
    _print_output(build_result)
    if build_result.returncode != 0:
        return build_result.returncode

    # List EVERYTHING installed -- a filtered listing previously omitted
    # zconf.h and made an incomplete install look complete (round-4
    # incident, windows_build.yml:331-335's own comment).
    report.section("installed zlib (complete)")
    for path in sorted(p for p in install_dir.rglob("*") if p.is_file()):
        report.plain(str(path))

    for required in _REQUIRED_INSTALLED_FILES:
        if not (install_dir / required).is_file():
            report.error(f"zlib install is missing {required}.")
            return 1

    report.plain("zlib install verified: zlib.h, zconf.h, zlibstatic.lib all present")
    return 0
