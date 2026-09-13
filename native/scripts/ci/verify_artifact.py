"""Linux artifact-verification gates: `linux_build.yml`'s L16 step (AC-L2/
AC-L3/AC-L4), the S-B3 import-closure gate, the S-F1 min-runtime
measure+drift pair, AC-L5's export-manifest check, and the portable-baseline
AVX-512 wrapper.

Frozen interface: docs/logs/2026-09-13/pyci-plan.md WI-7. Every emitted line
below is transcribed verbatim from `linux_build.yml` at `d33cc607` (the AC-2
baseline commit) -- this module changes WHERE the assertions run, never
WHAT they assert or print. Two pre-existing gaps port as-is on purpose
(C-G4 items; do not "fix" them here, see the two PORTED AS-IS comments
below and their pinning tests in test_verify_artifact.py):

  * the AC-L4 ldd gate branches on a substring match, never on ldd's own RC;
  * `READ_MIN_RUNTIME_RC` is printed but never gated (the min-runtime DRIFT
    check, not the read, is what fails the step).

Accumulate-all-failures does NOT apply inside `verify_artifact()`: the
original shell aborts at the first failing sub-check, and preserving that
exact short-circuit is itself part of AC-2 parity (a later sub-check's
`::error::` line must never appear on a run where an earlier one failed
today).
"""

from __future__ import annotations

import glob
import os
import re
import sys
from pathlib import Path

from . import report, run, targets

# native/scripts/assert_exports.py is a sibling top-level script (not part of
# this package) that already exposes a programmatic `run(manifest, platform,
# dump_text)` entry point -- imported directly rather than through
# `run.run()`, unlike the other sub-scripts below, all of which only expose
# an argparse `main()` and are therefore invoked as child processes.
_SCRIPTS_DIR = Path(__file__).resolve().parents[1]
if str(_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS_DIR))
import assert_exports as _assert_exports_script  # noqa: E402


def _artifact_path(platform: str) -> str:
    return targets.spec(platform)["artifact_path"]


def verify_artifact(platform: str, arch: str | None = None, *, dylib_path: str | None = None) -> int:
    """Linux: AC-L2 (file) + AC-L3 (nm -D vulkan symbol) + AC-L4 (ldd, no
    libvulkan). Replaces `linux_build.yml:507-540` -- UNCHANGED from before.

    macOS/windows added here (WI-22 follow-on): each replaces a completely
    DIFFERENT check under the same command name -- ``file`` exists is the
    only thing the three share. macOS asserts the produced dylib's
    architecture matches `arch` (`macos_build.yml:581-593`); windows only
    asserts the DLL exists and is non-empty (`windows_build.yml:494-511` --
    its exports check is a SEPARATE step, now `assert_exports()`, not this
    function). Every emitted line and every `::error::` message (including
    macOS's genuine absence of any stderr redirect on either of its two
    error lines, ported as-is) is transcribed verbatim per platform."""
    if platform == "linux":
        so = _artifact_path(platform)

        report.section("AC-L2: file")
        result = run.run(["file", so])
        if result.stdout:
            report.plain(result.stdout.rstrip("\n"))
        report.bare_rc(result.returncode)
        if result.returncode != 0:
            report.error(f"file '{so}' failed (rc={result.returncode}); the Linux .so is missing.")
            return 1

        report.section("AC-L3: nm -D halide_vulkan_device_interface")
        run.run_to_file(["nm", "-D", so], "nm_dynsyms.txt")
        dump_text = Path("nm_dynsyms.txt").read_text(errors="replace")
        matches = [line for line in dump_text.splitlines() if "halide_vulkan_device_interface" in line]
        for line in matches:
            report.plain(line)
        rc = 0 if matches else 1
        report.bare_rc(rc)
        if rc != 0:
            report.error(
                f"halide_vulkan_device_interface not found in {so} — "
                "Vulkan runtime absent from the binary (AC-L3)."
            )
            return 1

        report.section("AC-L4: ldd (expect NO libvulkan)")
        ldd_result = run.run_to_file(["ldd", so], "ldd_out.txt")
        ldd_text = Path("ldd_out.txt").read_text(errors="replace")
        if ldd_text:
            report.plain(ldd_text.rstrip("\n"))
        report.bare_rc(ldd_result.returncode)
        # PORTED AS-IS (pre-existing, d33cc607 linux_build.yml:534): the gate is
        # the substring test below, never ldd's own RC -- see
        # test_ldd_gate_is_substring_not_rc.
        if "libvulkan" in ldd_text:
            report.error("libvulkan appears in ldd output — link-time Vulkan dependency regressed (D4).")
            return 1

        return 0

    if platform == "macos":
        if not dylib_path:
            raise ValueError("verify_artifact(platform='macos') requires dylib_path")
        if not arch:
            raise ValueError("verify_artifact(platform='macos') requires arch")
        so = dylib_path
        if not Path(so).is_file():
            # PORTED AS-IS: no stderr redirect on this line in the source
            # shell (macos_build.yml:583-585), matching the same convention
            # already established for the staging/exports steps.
            report.error(f"Expected dylib not found at {so}", stream=sys.stdout)
            return 1
        result = run.run(["file", so])
        report.plain(result.stdout.rstrip("\n"))
        lipo_result = run.run(["lipo", "-archs", so])
        archs = lipo_result.stdout.strip()
        report.plain(f"lipo -archs => {archs}")
        if archs != arch:
            report.error(
                f"Expected architecture '{arch}' but the dylib reports '{archs}'",
                stream=sys.stdout,
            )
            return 1
        otool_result = run.run(["otool", "-L", so])
        report.plain(otool_result.stdout.rstrip("\n"))
        return 0

    if platform == "windows":
        so = _artifact_path(platform)
        if not Path(so).is_file():
            report.error(f"{so} not found — the Windows build did not emit the expected DLL.")
            return 1
        report.plain("== file ==")
        result = run.run(["file", so])
        if result.returncode != 0:
            report.notice("file(1) unavailable on this runner; size check below is the binding evidence.")
        else:
            report.plain(result.stdout.rstrip("\n"))
        report.plain("== size (bytes) ==")
        size = Path(so).stat().st_size
        report.marker("DLL_SIZE_BYTES", size)
        if size <= 0:
            report.error("DLL is zero bytes.")
            return 1
        return 0

    raise ValueError(f"verify_artifact: unsupported platform {platform!r}")


def import_closure(platform: str) -> int:
    """S-B3: readelf DT_NEEDED import-closure gate. Replaces
    `linux_build.yml:548-564`. Still Linux-only in practice (`_artifact_path`/
    `dist_dir` are both `None` for android in `targets.py` today, so this
    function cannot run for android without the same caller-supplied-
    artifact-dir redesign `stage.py` already went through -- flagged, not
    attempted here, per impl-18's report that this file's hardcoded
    `"readelf"` was likely wrong for android). Fixed here: the tool name now
    comes from `targets.spec(platform)["readelf_tools"]` instead of a bare
    hardcoded `"readelf"` -- linux's own declared value is identical
    (`("readelf",)`), so this is a dead-data-consumption fix with zero
    behaviour change on the only platform that reaches this function today."""
    so = _artifact_path(platform)
    spec = targets.spec(platform)
    readelf_tool = spec["readelf_tools"][0] if spec["readelf_tools"] else "readelf"

    report.section("S-B3: readelf DT_NEEDED import-closure gate")
    run.run_to_file([readelf_tool, "-d", so], "readelf_dynamic.txt")
    dynamic_text = Path("readelf_dynamic.txt").read_text(errors="replace")
    if dynamic_text:
        report.plain(dynamic_text.rstrip("\n"))

    rc, out = run.capture([
        sys.executable, "native/scripts/assert_import_closure.py",
        "--dump", "readelf_dynamic.txt",
        "--staged-dir", spec["dist_dir"],
        "--declaration", "native/deps/shipped_files.toml",
        "--platform", platform,
        "--format", "elf",
    ])
    if out:
        report.plain(out.rstrip("\n"))
    report.rc("IMPORT_CLOSURE", rc)
    if rc != 0:
        report.error(f"import-closure gate failed for {so} — see IMPORT ... -> MISSING lines above.")
    return rc


# P-10 (push 6): `min_runtime()` used to live here, Linux-only. It was
# orphaned when `ci.py`'s dispatch was rewired to the four-platform
# `ci/minruntime.py` generalisation and never called again in production --
# but it stayed invisible to every test for weeks because
# `ci.minruntime.min_runtime` and this module's former `min_runtime` shared
# the exact same name and signature, and on the only platform the stale
# dispatch could still reach (linux) they did the same readelf work. Deleted
# here, by this file's owner, in push 7 (deferred from push 6 deliberately --
# deleting a function in another WI's file mid-migration is the exact
# cross-ownership edit this campaign forbids; push 6 only fixed the dispatch).
# Its three direct-behaviour tests (`test_min_runtime_read_rc_is_not_gated`,
# `test_min_runtime_drift_failure_returns_nonzero`,
# `test_emission_matches_golden_min_runtime`) were deleted in the same
# commit -- this project's standing rule is that a superseded module's tests
# and docs go with it, no tombstones. Equivalent coverage lives in
# `ci/minruntime.py`'s own `_from_dump` and `test_minruntime.py`.
# `test_dispatch.py`'s `TestMinRuntimeDispatchGeneralised` no longer spies on
# this module's `min_runtime` attribute at all -- it asserts the attribute
# does not exist, which is a STRONGER regression guard than "was not called"
# (a future revert cannot recreate the function without the deletion itself
# being noticed).


def assert_exports(
    platform: str,
    arch: str | None = None,
    *,
    dylib_path: str | None = None,
    artifact_dir: str | None = None,
    ndk_home: str | None = None,
) -> int:
    """AC-L5/G3/AC-W4/G6: required FFI exports present in the built
    artifact. Replaces `linux_build.yml:629-641` (unchanged from before --
    linux still reads `nm_dynsyms.txt`, produced as a side effect of
    `verify_artifact()` earlier in the same job), `macos_build.yml:741-760`,
    `windows_build.yml:544-585`, `android_build.yml:295-326`.

    macOS/windows/android could NOT be collapsed onto linux's shape: the
    shell-prohibition guard's compliance test
    (`check_shell_prohibition.py:95,168`) requires exactly one code line
    starting with a python/pwsh-python invocation, so a `tool > file` dump
    step can never itself be a compliant one-liner -- the dump has to move
    INSIDE this module for every platform whose `ci.py verify-artifact`
    equivalent does not already produce a dump as a side effect (only linux
    does). One module change serves all three non-linux legs (they share
    an identical dump-then-assert shape) rather than three near-duplicate
    per-leg scripts -- the exact P-10 failure class this campaign keeps
    re-finding.

    Every dump-phase RC marker name and every `::error::` message below is
    transcribed VERBATIM per platform, including macOS's genuine ABSENCE of
    any `::error::` line on a dump failure (`exit "${RC}"` with no error
    text in the source shell -- ported as-is, not an omission here)."""
    if platform == "linux":
        so = _artifact_path(platform)
        report.section("AC-L5: required FFI exports present in .so")
        dump_text = Path("nm_dynsyms.txt").read_text(errors="replace")
    elif platform == "macos":
        if not dylib_path:
            raise ValueError("assert_exports(platform='macos') requires dylib_path")
        so = dylib_path
        result = run.run_to_file(["nm", "-gU", so], "dylib_exports.txt")
        report.marker("NM_EXPORTS_RC", result.returncode)
        if result.returncode != 0:
            report.error(
                f"nm failed on {so} (rc={result.returncode}); export presence is "
                "UNVERIFIED, refusing to publish."
            )
            text = Path("dylib_exports.txt").read_text(errors="replace")
            if text:
                report.plain(text.rstrip("\n"))
            return 1
        dump_text = Path("dylib_exports.txt").read_text(errors="replace")
    elif platform == "windows":
        so = _artifact_path(platform)
        report.plain("== AC-W4: exported FFI symbols ==")
        result = run.run(["dumpbin", "-exports", so])
        Path("dll_exports.txt").write_text(result.stdout + result.stderr)
        rc = result.returncode
        report.marker("DUMPBIN_RC", rc)
        if rc != 0:
            report.notice(f"dumpbin unavailable or failed (rc={rc}); falling back to llvm-nm.")
            result = run.run(["llvm-nm", "--extern-only", "--defined-only", so])
            Path("dll_exports.txt").write_text(result.stdout + result.stderr)
            rc = result.returncode
            report.marker("LLVM_NM_RC", rc)
        if rc != 0:
            report.error(
                f"could not read the export table of {so} with either dumpbin or "
                f"llvm-nm (rc={rc}); export presence is UNVERIFIED, refusing to publish."
            )
            text = Path("dll_exports.txt").read_text(errors="replace")
            if text:
                report.plain(text.rstrip("\n"))
            return 1
        dump_text = Path("dll_exports.txt").read_text(errors="replace")
        report.plain("-- first 60 lines of the export listing --")
        report.plain("\n".join(dump_text.splitlines()[:60]))
    elif platform == "android":
        if not artifact_dir or not ndk_home:
            raise ValueError(
                "assert_exports(platform='android') requires artifact_dir and ndk_home"
            )
        matches = sorted(
            glob.glob(os.path.join(artifact_dir, "native", "libdng_decoder_native*.so"))
        )
        if not matches:
            report.error(f"no libdng_decoder_native*.so found under {artifact_dir}/native")
            return 1
        so = matches[0]
        llvm_nm = os.path.join(
            ndk_home, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin", "llvm-nm"
        )
        if not os.access(llvm_nm, os.X_OK):
            report.error(
                f"llvm-nm not found at {llvm_nm} — NDK layout may have changed (r27c expected)."
            )
            return 1
        result = run.run_to_file([llvm_nm, "-D", so], "android_so_dynsyms.txt")
        report.marker("NM_RC", result.returncode)
        if result.returncode != 0:
            report.error(
                f"llvm-nm -D failed on {so} (rc={result.returncode}); export presence is "
                "UNVERIFIED, refusing to publish."
            )
            text = Path("android_so_dynsyms.txt").read_text(errors="replace")
            if text:
                report.plain(text.rstrip("\n"))
            return 1
        dump_text = Path("android_so_dynsyms.txt").read_text(errors="replace")
    else:
        raise ValueError(f"assert_exports: unsupported platform {platform!r}")

    # Keyword call, not positional: `native/scripts/deps/test_no_shell_lint.py`
    # flags any call named `run` whose FIRST POSITIONAL argument is a bare
    # string (the subprocess-argv shape it exists to catch) -- this is a
    # direct in-process call to assert_exports.py's own `run()`, not a
    # subprocess invocation, so it is a false positive under that lint's
    # name-only matching. Keyword args are not `node.args`, so this call
    # shape is correctly outside the lint's scope without touching the lint
    # itself (an un-owned, frozen file this campaign does not modify).
    rc = _assert_exports_script.run(
        manifest_path="native/deps/export_manifest.toml", platform=platform, dump_text=dump_text
    )
    report.rc("ASSERT_EXPORTS", rc)
    if rc != 0:
        if platform == "windows":
            report.error(
                f"assert_exports.py reported missing/absent symbol(s) in {so} — the DLL does "
                "not export the FFI surface Dart looks up. On Windows this needs "
                "__declspec(dllexport) via FFI_EXPORT on the definitions in native/src/ffi/ "
                "(see dng_ffi_api.cpp and heif_ffi_api.cpp)."
            )
        elif platform == "android":
            report.error(
                f"assert_exports.py reported missing/absent symbol(s) in {so} — the .so does "
                "not export the FFI surface Dart looks up."
            )
        elif platform == "linux":
            report.error(
                f"assert_exports.py reported missing/absent symbol(s) in {so} — the .so does "
                "not export the FFI surface Dart looks up (check FFI_EXPORT on the definitions "
                "in native/src/ffi/)."
            )
        # macOS: PORTED AS-IS -- the original shell has no `::error::` line
        # here at all, only `exit "${RC}"`. Do not add one.
    return rc


def verify_staged_companions(platform: str, dylib_path: str, artifact_dir: str, arch: str) -> int:
    """macOS's three staged-companion gates (macos_build.yml:790-861), split
    out of the "Stage native artifact" step per leader ruling: `stage.py`
    stages, this module verifies. THREE INDEPENDENT GATES, none implies the
    others (each ported verbatim, see the YAML comment this replaces for the
    real defect each one alone was proven to catch):

      1. Architecture: `lipo -archs` on each STAGED companion (from
         `<artifact_dir>/native/`, i.e. after `stage.py` has already copied
         it there -- not the pre-staged source).
      2. Reachability: the companion's basename appears in the decoder's OWN
         `otool -L` dependency listing (read from `dylib_path`, the ORIGINAL
         path -- not the staged copy; ported as-is, this asymmetry is
         intentional in the source step).
      3. Path convention: that dependency line must start `@rpath/<companion>`.

    Self-reference handling: `otool -L`'s line 2 is the inspected file's own
    LC_ID_DYLIB entry, stripped by NAME (`@rpath/<dylib basename>`), never by
    a positional offset (measured directly against a real decoder: a
    positional `tail -n +2` alone does not strip it).

    Every emitted line has NO `>&2` redirect in the original shell (unlike
    every other error line in this module) -- ported as-is via
    `report.error(..., stream=sys.stdout)`, matching report.py's documented
    exception list.
    """
    import read_shipped_files

    _SCRIPTS_DIR2 = Path(__file__).resolve().parents[1]
    if str(_SCRIPTS_DIR2) not in sys.path:
        sys.path.insert(0, str(_SCRIPTS_DIR2))

    companions = read_shipped_files.load_declaration()[platform]["companions"]
    staged_dir = Path(artifact_dir) / "native"
    dylib_basename = os.path.basename(dylib_path)

    report.plain(f"--- Gate 1: architecture (every staged companion reports {arch}) ---")
    for companion in companions:
        result = run.run(["lipo", "-archs", str(staged_dir / companion)])
        archs = (result.stdout + result.stderr).strip()
        report.plain(f"arch({companion}): {archs}")
        if not re.search(rf"\b{re.escape(arch)}\b", archs):
            report.error(
                f"{companion} archs '{archs}' do not include {arch}", stream=sys.stdout
            )
            return 1

    report.plain("--- Gates 2+3: reachability + path convention (decoder's own dependency graph) ---")
    run.run_to_file(["otool", "-L", dylib_path], "otool_dylib_deps.txt")
    all_lines = Path("otool_dylib_deps.txt").read_text(errors="replace").splitlines()
    # tail -n +2, then strip the self-reference (LC_ID_DYLIB) line by NAME,
    # not by position -- see docstring.
    dylib_dep_lines = [
        line for line in all_lines[1:] if f"@rpath/{dylib_basename}" not in line
    ]
    report.plain("\n".join(dylib_dep_lines))
    for companion in companions:
        matching = [line for line in dylib_dep_lines if companion in line]
        companion_line = "\n".join(matching)
        if not companion_line:
            report.error(
                f"{dylib_path} does not depend on {companion} at all — staged but "
                "unlinked (dead file).",
                stream=sys.stdout,
            )
            return 1
        report.plain(f"dependency line for {companion}: {companion_line}")
        if not any(re.match(rf"^\s*@rpath/{re.escape(companion)} ", line) for line in matching):
            report.error(
                f"{companion} is depended upon via a non-relative reference "
                f"({companion_line}) — expected the line to start with @rpath/{companion}. "
                "This would fail to load on any machine but the build host (the exact "
                "class of defect fixed in b0a0573).",
                stream=sys.stdout,
            )
            return 1
    return 0


def assert_no_avx512(platform: str) -> int:
    """Portable-baseline gate: the published .so must contain no AVX-512
    codepath. Replaces `linux_build.yml:671-681`. C-G3: `assert_no_avx512.py`
    keeps its logic inline in its own `main()` -- called through `run.run()`,
    never refactored into this module."""
    so = _artifact_path(platform)

    rc, out = run.capture([
        sys.executable, "native/scripts/assert_no_avx512.py",
        so,
        "--dump", "native/build-linux/avx512-gate-objdump.txt",
    ])
    if out:
        report.plain(out.rstrip("\n"))
    report.bare_rc(rc, "avx512 gate")
    if rc != 0:
        report.error(
            f"AVX-512 gate failed (rc={rc}) for libdng_decoder_native.so — refusing to publish "
            "an artifact that SIGILLs on non-AVX-512 CPUs. rc=2 means the check could not run at "
            "all, which is equally disqualifying."
        )
        # PORTED AS-IS (pre-existing, d33cc607 linux_build.yml:678-682): the
        # shell always `exit 1` here regardless of the child's actual rc
        # (2 for "could not run" collapses to the same 1 as an assertion
        # failure) -- see test_avx512_failure_returns_1_not_rc.
        return 1
    return 0
