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


def verify_artifact(platform: str, arch: str | None = None) -> int:
    """AC-L2 (file) + AC-L3 (nm -D vulkan symbol) + AC-L4 (ldd, no
    libvulkan). Replaces `linux_build.yml:507-540`."""
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


def import_closure(platform: str) -> int:
    """S-B3: readelf DT_NEEDED import-closure gate. Replaces
    `linux_build.yml:548-564`."""
    so = _artifact_path(platform)
    spec = targets.spec(platform)

    report.section("S-B3: readelf DT_NEEDED import-closure gate")
    run.run_to_file(["readelf", "-d", so], "readelf_dynamic.txt")
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


def assert_exports(platform: str, arch: str | None = None) -> int:
    """AC-L5: required FFI exports present in the .so. Replaces
    `linux_build.yml:629-641`."""
    so = _artifact_path(platform)

    report.section("AC-L5: required FFI exports present in .so")
    dump_text = Path("nm_dynsyms.txt").read_text(errors="replace")
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
        report.error(
            f"assert_exports.py reported missing/absent symbol(s) in {so} — the .so does not "
            "export the FFI surface Dart looks up (check FFI_EXPORT on the definitions in "
            "native/src/ffi/)."
        )
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
