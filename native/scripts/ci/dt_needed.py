"""DT_NEEDED verification — two DIFFERENT algorithms, one per platform that
has a DT_NEEDED check at all (only linux and android do; confirmed by
``grep -l DT_NEEDED .github/workflows/*.yml`` — windows and macOS have no
equivalent step and are not handled here).

RULING (lead6-pyci-opus, 2026-09-13, WI-22): these are NOT one algorithm
with a platform parameter. Linux's check is a fixed two-level chain for
exactly two named libraries via plain ``readelf``. Android's is two
independent, structurally different checks via ``llvm-readelf``: a
bidirectional presence/absence assertion keyed off a configure-log STL
marker (no linux equivalent — android's C++ STL choice is a build-time
branch, linux's isn't), and a device-measured whitelist closure over every
.so in the artifact dir (``assert_android_so_dt_needed_closure.py``,
already its own script, invoked here rather than re-implemented). A third
android step in the source YAML — ``assert_import_closure.py`` run
alongside the closure script — is NOT ported here: it is the exact same
declaration-based check linux and windows already get through the
pre-existing, already-platform-generic ``ci.py import-closure`` command
(``verify_artifact.import_closure()``), so folding it into this module
would duplicate rather than migrate it. The workflow wiring for
``import-closure --platform android`` is WI-19's concern, not this
module's.

De-pipelined throughout (tool output to a file/string, then matched in
Python) per the pipefail lesson: ``readelf | grep -q`` INVERTS on a
successful match under ``set -euo pipefail`` (grep exits first, readelf
takes SIGPIPE, pipeline reports 141) — ``run.run_to_file``/``run.capture``
plus a plain Python substring match never hits that trap (C-G3/run.py
rule 2).
"""

from __future__ import annotations

import os
from pathlib import Path

from . import report, run

_ANDROID_STL_SHARED_LOG_LINE = (
    "-- Android STL: c++_shared -- staged libc++_shared.so next to dng_decoder_native"
)
_ANDROID_STL_STATIC_LOG_LINE = (
    "-- Android STL: c++_static -- libc++_shared.so not required, nothing staged"
)


def _needed_level(so_path: Path, dump_path: Path, needle: str, error_text: str, tool: str = "readelf") -> int:
    result = run.run_to_file([tool, "-d", str(so_path)], dump_path)
    dump_text = dump_path.read_text(encoding="utf-8", errors="replace")
    report.plain(dump_text.rstrip("\n"))
    if result.returncode != 0:
        rc = result.returncode
    else:
        matched = [line for line in dump_text.splitlines() if needle in line]
        for line in matched:
            report.plain(line)
        rc = 0 if matched else 1
    report.bare_rc(rc, f"DT_NEEDED {needle} in {so_path}")
    if rc != 0:
        report.error(error_text)
        return 1
    return 0


def _dt_needed_linux(artifact_dir: str, runner_temp: str) -> int:
    """Replaces linux_build.yml's "Verify DT_NEEDED references the staged
    HEIF companions" step (:983-1011, d33cc607). Two fixed levels:

    1. The decoder's own NEEDED entry must name ``libheif.so.1`` directly.
    2. The staged ``libheif.so.1``'s own NEEDED entry must name
       ``libde265.so.0`` transitively (the decoder itself has no direct
       DT_NEEDED entry for libde265 -- GNU ld's ``--as-needed`` only
       records a direct dependency for a symbol actually referenced, and
       no translation unit here calls a de265_* symbol directly).
    """
    native_dir = Path(artifact_dir) / "native"
    so = native_dir / "libdng_decoder_native.so"
    heif_so = native_dir / "libheif.so.1"

    report.section("Level 1: decoder's own NEEDED (expect libheif.so.1)")
    rc = _needed_level(
        so,
        Path(runner_temp) / "dt_needed_decoder.txt",
        "libheif.so.1",
        f"{so} has no DT_NEEDED entry for libheif.so.1 — the staged "
        "companion would never be loaded.",
    )
    if rc != 0:
        return rc

    report.section("Level 2: libheif.so.1's own NEEDED (expect libde265.so.0, transitive)")
    return _needed_level(
        heif_so,
        Path(runner_temp) / "dt_needed_heif.txt",
        "libde265.so.0",
        f"{heif_so} has no DT_NEEDED entry for libde265.so.0 — the HEVC "
        "decoder linkage is UNVERIFIED, refusing to publish.",
    )


def _resolve_llvm_readelf(ndk_home: str) -> str:
    """Same NDK-toolchain-root join orientation.py uses for llvm-nm — a
    fixed, already-verified layout (r27c), not a PATH lookup."""
    return os.path.join(ndk_home, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin", "llvm-readelf")


def _find_decoder_so(artifact_dir: str) -> Path | None:
    matches = sorted(Path(artifact_dir, "native").glob("libdng_decoder_native*.so"))
    return matches[0] if matches else None


def _dt_needed_android_stl(artifact_dir: str, runner_temp: str, llvm_readelf: str, build_log: str) -> int:
    """Replaces android_build.yml's "Assert decoder DT_NEEDED matches the
    resolved Android STL (A-T8-FIX libc++_shared handling)" step
    (:366-417). Bidirectional: whichever STL branch heif.cmake's configure
    log says was taken, the decoder's DT_NEEDED table for libc++_shared.so
    must agree — present iff c++_shared, absent iff anything else."""
    so = _find_decoder_so(artifact_dir)
    if so is None:
        report.error(f"no libdng_decoder_native*.so found under {artifact_dir}/native")
        return 1

    dump_path = Path(runner_temp) / "android_decoder_dynamic.txt"
    result = run.run_to_file([llvm_readelf, "-d", str(so)], dump_path)
    report.rc("READELF", result.returncode)
    if result.returncode != 0:
        report.plain(dump_path.read_text(encoding="utf-8", errors="replace").rstrip("\n"))
        report.error(f"llvm-readelf -d failed on {so} (rc={result.returncode}); DT_NEEDED is UNVERIFIED, refusing to publish.")
        return 1
    dump_text = dump_path.read_text(encoding="utf-8", errors="replace")

    log_text = Path(build_log).read_text(encoding="utf-8", errors="replace") if Path(build_log).exists() else ""
    stl_shared = _ANDROID_STL_SHARED_LOG_LINE in log_text
    stl_static = _ANDROID_STL_STATIC_LOG_LINE in log_text
    report.marker("STL_SHARED_LOG", stl_shared)
    report.marker("STL_STATIC_LOG", stl_static)
    if not stl_shared and not stl_static:
        report.error(
            "configure log shows neither of heif.cmake's expected 'Android STL: ...' "
            "lines — the STL branch that ran cannot be identified, refusing to publish "
            "an unverified decoder."
        )
        return 1

    has_libcxx_shared = "libc++_shared.so" in dump_text
    report.marker("DT_NEEDED_HAS_LIBCXX_SHARED", has_libcxx_shared)

    if stl_shared and not has_libcxx_shared:
        report.plain(dump_text.rstrip("\n"))
        report.error(
            f"{so}'s DT_NEEDED does not list libc++_shared.so, but the configure log says "
            "CMAKE_ANDROID_STL_TYPE=c++_shared — staging without an actual link-time need "
            "(or a build that silently dropped it) would ship a dead file."
        )
        return 1
    if not stl_shared and has_libcxx_shared:
        report.plain(dump_text.rstrip("\n"))
        report.error(
            f"{so}'s DT_NEEDED lists libc++_shared.so, but the configure log says the STL "
            "is c++_static (not c++_shared) — an unexpected partial/shared C++ link crept in."
        )
        return 1
    report.bare_rc(0, "android STL DT_NEEDED matches configure log")
    return 0


def _dt_needed_android_closure(artifact_dir: str, llvm_readelf: str) -> int:
    """Replaces the device-whitelist half of android_build.yml's "Assert
    DT_NEEDED closure across the packaged Android .so set (Task 11
    follow-up)" step (:437-483). The declaration-based half of that same
    step (``assert_import_closure.py``) is NOT ported here — see module
    docstring — it is the pre-existing ``ci.py import-closure`` command."""
    rc, out = run.capture([
        "python3", "native/scripts/assert_android_so_dt_needed_closure.py",
        "--readelf", llvm_readelf,
        "--so-dir", str(Path(artifact_dir) / "native"),
        "--bundled", "libheif.so,libde265.so,libdng_decoder_native.so",
        "--optional-bundled", "libc++_shared.so",
    ])
    if out:
        report.plain(out.rstrip("\n"))
    report.rc("DT_NEEDED_CLOSURE", rc)
    if rc != 0:
        report.error(
            "a packaged .so has a DT_NEEDED entry neither bundled nor in the measured "
            "system whitelist — see the output above for which library/libraries are "
            "unexplained (all offenders are named, not just the first)."
        )
    return rc


def _dt_needed_android(artifact_dir: str, runner_temp: str, ndk_home: str | None, build_log: str) -> int:
    if not ndk_home:
        report.error("dt_needed: --ndk-home is required for platform android")
        return 2
    llvm_readelf = _resolve_llvm_readelf(ndk_home)
    if not os.access(llvm_readelf, os.X_OK):
        report.error(f"llvm-readelf not found at {llvm_readelf} — NDK layout may have changed (r27c expected).")
        return 1

    report.section("Android level 1: decoder DT_NEEDED matches resolved STL")
    rc = _dt_needed_android_stl(artifact_dir, runner_temp, llvm_readelf, build_log)
    if rc != 0:
        return rc

    report.section("Android level 2: DT_NEEDED closure across packaged .so set (device whitelist)")
    return _dt_needed_android_closure(artifact_dir, llvm_readelf)


def dt_needed(
    platform: str,
    artifact_dir: str,
    runner_temp: str,
    *,
    ndk_home: str | None = None,
    build_log: str = "android_build.log",
) -> int:
    """Dispatches to the platform's own DT_NEEDED algorithm. ``ndk_home``
    and ``build_log`` are android-only; unused on linux. No windows/macOS
    branch exists on purpose — neither platform has a DT_NEEDED step in
    its workflow today."""
    if platform == "android":
        return _dt_needed_android(artifact_dir, runner_temp, ndk_home, build_log)
    if platform == "linux":
        return _dt_needed_linux(artifact_dir, runner_temp)
    report.error(f"dt_needed: unsupported platform {platform!r} (only linux and android have a DT_NEEDED step)")
    return 2
