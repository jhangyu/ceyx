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


def min_runtime(platform: str, arch: str | None = None) -> int:
    """S-F1: measure the artifact's min-runtime floor, then assert it
    matches the declared value. Replaces `linux_build.yml:571-594`. No
    `== ... ==` banner and no `::error::` line exist in the original for
    this block -- do not add either."""
    so = _artifact_path(platform)

    run.run_to_file(["readelf", "--dyn-syms", so], "readelf_dynsyms.txt")

    rc, out = run.capture([
        sys.executable, "native/scripts/read_min_runtime.py",
        "--artifact", "readelf_dynsyms.txt",
        "--platform", platform,
        "--out", "min_runtime.txt",
    ])
    if out:
        report.plain(out.rstrip("\n"))
    # PORTED AS-IS (pre-existing, d33cc607 linux_build.yml:571-572 /
    # C-G4 item 2): this RC is echoed but never gated -- the drift check
    # below is the only sub-check of this function that can fail it. See
    # test_min_runtime_read_rc_is_not_gated.
    report.marker("READ_MIN_RUNTIME_RC", rc)

    min_runtime_path = Path("min_runtime.txt")
    if min_runtime_path.exists():
        text = min_runtime_path.read_text(errors="replace")
        if text:
            report.plain(text.rstrip("\n"))

    arch_args = ["--arch", arch] if arch else []
    drift_rc, drift_out = run.capture([
        sys.executable, "native/scripts/assert_min_runtime_matches_declared.py",
        "--emitted", "min_runtime.txt",
        "--declared", "native/deps/min_runtime_expected.toml",
        "--platform", platform,
        *arch_args,
    ])
    if drift_out:
        report.plain(drift_out.rstrip("\n"))
    report.marker("MIN_RUNTIME_DRIFT_RC", drift_rc)
    return drift_rc


def assert_exports(platform: str, arch: str | None = None) -> int:
    """AC-L5: required FFI exports present in the .so. Replaces
    `linux_build.yml:629-641`."""
    so = _artifact_path(platform)

    report.section("AC-L5: required FFI exports present in .so")
    dump_text = Path("nm_dynsyms.txt").read_text(errors="replace")
    rc = _assert_exports_script.run("native/deps/export_manifest.toml", platform, dump_text)
    report.rc("ASSERT_EXPORTS", rc)
    if rc != 0:
        report.error(
            f"assert_exports.py reported missing/absent symbol(s) in {so} — the .so does not "
            "export the FFI surface Dart looks up (check FFI_EXPORT on the definitions in "
            "native/src/ffi/)."
        )
    return rc


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
