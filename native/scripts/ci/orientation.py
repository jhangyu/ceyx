"""Fused-orientation Halide-kernel capability gate ("Task 11 / G-E"): does
the just-built artifact actually contain the fused-orientation kernels, not
just the plain FFI export surface (which `verify_artifact.assert_exports`
already checks and cannot distinguish a fused build from a stale
pre-fusion one, since the generator's symbol NAMES never changed)?

Frozen interface: docs/logs/2026-09-13/pyci-plan.md WI-10.

TWO ALGORITHMS BEHIND ONE COMMAND (C-G6) -- do not unify them:
  * `_strings_scan` (linux/windows/android): a static string-literal scan.
    No execution is possible/wanted here (Android is cross-arch with no
    emulator in this compile-only CI; Linux/Windows restrict their dynamic
    export surface to the hand-written FFI symbols, so Halide's AOT
    `<kernel>_metadata()` entry points are never in the dynamic symbol
    table for a runtime probe to dlsym).
  * `_compiled_probe` (macOS only): dlopen+dlsym the real
    `orient_capability_probe` binary against the dylib. Valid ONLY on
    macOS, which default-exports every symbol unless explicitly hidden.

LOCKSTEP NOTE (moved here from `linux_build.yml`'s pre-migration comment,
also duplicated in `android_build.yml`/`windows_build.yml`): the literal
sets below (`REQUIRED_SYMBOLS`, `REQUIRED_COEFFS`) are coupled to
`native/tests/orient_capability_probe.cpp`'s `kRequiredArgs`/`kChecks` by
construction (see that file's header comment). Any future kernel-signature
change must update both together -- and now only ONE Python dict instead of
three duplicated shell `for` loops.

ANDROID IS A GENUINE THIRD SHAPE, not a data-only variation of the
linux/windows strings scan, transcribed here exactly as measured against
`android_build.yml`'s real step (WI-10 implementer finding, escalated and
approved by the leader before this file was written -- see
docs/logs/2026-09-13 campaign log for the ruling): it reads TWO dumps (an
NDK `llvm-nm -D` dump for the kernel-name symbol(s), a plain `strings` dump
for the coefficients), not one; its symbol check is a SUBSTRING match
(`grep -q`, no `-x`), not the whole-line match the coefficient check (and
the entire linux/windows scan) uses; and it never probes for a missing
tool the way linux/windows do (no `command -v` gate at all -- the NDK's
llvm-nm is a fixed, already-verified-present path, and `strings` is
invoked unconditionally). None of this is "fixed" here: it is today's real,
if inconsistent, gate, and PORTED AS-IS is what makes a later real fix
possible to prove as a deliberate, reviewable red.
"""

from __future__ import annotations

import glob
import os
import shutil
from pathlib import Path

from . import report, run, targets

REQUIRED_SYMBOLS: dict = {
    "linux": ("dng_render_stage4_split",),
    "windows": ("dng_render_stage4_split",),
    "android": ("dng_render_stage4", "dng_render_stage4_split"),
}

REQUIRED_COEFFS: tuple = (
    "orient_a_x", "orient_b_x", "orient_c_x",
    "orient_a_y", "orient_b_y", "orient_c_y",
)

_NO_TOOL_ERROR = (
    "neither llvm-strings nor strings is on PATH; capability is UNVERIFIED, refusing to publish."
)

_OUT_FILES = {
    "linux": "so_strings.txt",
    "windows": "dll_strings.txt",
}


def assert_orientation(platform: str, arch: str | None = None) -> int:
    """Dispatches to the platform's real algorithm. macOS is the compiled
    probe; every other platform is a strings/nm literal scan."""
    if platform == "macos":
        dylib = os.environ["DYLIB"]
        return _compiled_probe(platform, dylib)

    if platform == "android":
        artifact_dir = os.environ["ARTIFACT_DIR"]
        matches = sorted(glob.glob(os.path.join(artifact_dir, "native", "libdng_decoder_native*.so")))
        if not matches:
            report.error(f"no libdng_decoder_native*.so found under {artifact_dir}/native")
            return 1
        return _strings_scan(platform, matches[0], "android_so_strings.txt")

    so = targets.spec(platform)["artifact_path"]
    return _strings_scan(platform, so, _OUT_FILES[platform])


def _strings_scan(platform: str, binary_path: str, out_path: str) -> int:
    """linux/windows/android: a static string-literal scan for the
    fused-orientation kernel-name symbol(s) plus the six affine
    coefficients. See module docstring: Android's shape genuinely differs
    from linux/windows's, ported as measured, not unified."""
    if platform == "android":
        return _android_scan(binary_path, out_path)
    return _strings_scan_common(platform, binary_path, out_path)


def _resolve_strings_tool_name(platform: str) -> str | None:
    """Returns the first candidate NAME (not a resolved path) from
    `targets.spec(platform)["strings_tools"]` that `shutil.which` finds --
    the shell's `STRINGS_TOOL` marker is always the tried NAME
    ("llvm-strings"/"strings"), never a resolved absolute path."""
    for name in targets.spec(platform)["strings_tools"]:
        if shutil.which(name):
            return name
    return None


def _strings_scan_common(platform: str, binary_path: str, out_path: str) -> int:
    tool_name = _resolve_strings_tool_name(platform)
    if tool_name is None:
        report.error(_NO_TOOL_ERROR)
        return 1

    result = run.run_to_file([tool_name, binary_path], out_path)
    text = Path(out_path).read_text(errors="replace")
    report.plain(f"STRINGS_TOOL={tool_name} STRINGS_RC={result.returncode}")
    if result.returncode != 0:
        report.error(
            f"{tool_name} failed reading {binary_path}; capability is UNVERIFIED, refusing to publish."
        )
        return 1

    lines = text.splitlines()
    missing: list[str] = []
    for sym in REQUIRED_SYMBOLS[platform]:
        found = sym in lines  # grep -qx: exact whole-line match
        rc = 0 if found else 1
        report.plain(f"RC={rc} (symbol literal: {sym})")
        if not found:
            missing.append(f"symbol:{sym}")
    for coeff in REQUIRED_COEFFS:
        found = coeff in lines  # grep -qx: exact whole-line match
        rc = 0 if found else 1
        report.plain(f"RC={rc} (string: {coeff})")
        if not found:
            missing.append(f"string:{coeff}")

    if missing:
        suffix = "".join(f" {m}" for m in missing)
        report.error(
            f"missing fused-orientation signal(s) in {binary_path}:{suffix} — this .so may be "
            "a stale pre-fusion build (the generator names never changed, so symbol presence "
            "alone does not prove the kernel was rebuilt with orientation fused in)."
        )
        return 1
    return 0


def _android_scan(binary_path: str, out_path: str) -> int:
    """PORTED AS-IS (pre-existing, d33cc607 android_build.yml, "Assert
    fused-orientation kernel signals present in Android .so"): a genuinely
    different algorithm from `_strings_scan_common` -- two tools, two dumps,
    and a SUBSTRING (not whole-line) match for the symbol check. See
    test_android_symbol_check_is_substring_not_wholeline."""
    ndk_home = os.environ["ANDROID_NDK_HOME"]
    llvm_nm = os.path.join(ndk_home, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin", "llvm-nm")

    nm_result = run.run_to_file([llvm_nm, "-D", binary_path], "android_so_dynsyms_orient.txt")
    strings_result = run.run_to_file(["strings", binary_path], out_path)
    report.plain(f"NM_RC={nm_result.returncode} STRINGS_RC={strings_result.returncode}")
    if nm_result.returncode != 0 or strings_result.returncode != 0:
        report.error(
            f"llvm-nm or strings failed reading {binary_path}; capability is UNVERIFIED, "
            "refusing to publish."
        )
        return 1

    nm_text = Path("android_so_dynsyms_orient.txt").read_text(errors="replace")
    strings_text = Path(out_path).read_text(errors="replace")
    strings_lines = strings_text.splitlines()

    missing: list[str] = []
    for sym in REQUIRED_SYMBOLS["android"]:
        found = sym in nm_text  # grep -q (no -x): SUBSTRING match, ported as-is
        rc = 0 if found else 1
        report.plain(f"RC={rc} (symbol: {sym})")
        if not found:
            missing.append(f"symbol:{sym}")
    for coeff in REQUIRED_COEFFS:
        found = coeff in strings_lines  # grep -qx: exact whole-line match
        rc = 0 if found else 1
        report.plain(f"RC={rc} (string: {coeff})")
        if not found:
            missing.append(f"string:{coeff}")

    if missing:
        suffix = "".join(f" {m}" for m in missing)
        report.error(
            f"missing fused-orientation signal(s) in {binary_path}:{suffix} — this .so may be "
            "a stale pre-fusion build (the generator names never changed, so symbol presence "
            "alone does not prove the kernel was rebuilt with orientation fused in)."
        )
        return 1
    return 0


def _compiled_probe(platform: str, dylib_path: str) -> int:
    """macOS only: dlopen+dlsym the real `orient_capability_probe` binary.
    Replaces `macos_build.yml`'s "Build + run orientation capability probe"
    step."""
    dylib_dir = os.path.dirname(dylib_path)

    build_result = run.run(["cmake", "--build", dylib_dir, "--target", "orient_capability_probe", "-j4"])
    combined = build_result.stdout + build_result.stderr
    if combined:
        report.plain(combined.rstrip("\n"))
    if build_result.returncode != 0:
        # No synthesized ::error:: line here: the original shell has no
        # `set +e`/error text around the `cmake --build` line either --
        # a build failure aborts the step on its own exit code.
        return build_result.returncode

    probe = os.path.join(dylib_dir, "orient_capability_probe")
    probe_result = run.run_to_file([probe, dylib_path], "orient_capability_probe_output.txt")
    output_text = Path("orient_capability_probe_output.txt").read_text(errors="replace")
    if output_text:
        report.plain(output_text.rstrip("\n"))
    report.marker("ORIENT_CAPABILITY_PROBE_RC", probe_result.returncode)
    if probe_result.returncode != 0:
        report.error(
            "orient_capability_probe reported the shipped dylib does not export the "
            "fused-orientation kernel(s); see output above for which symbol/capability is "
            "missing."
        )
        # PORTED AS-IS (pre-existing, d33cc607 macos_build.yml, "Build + run
        # orientation capability probe"): exit literal 1, never the probe's
        # own rc (C-G4 item 3). See test_macos_probe_failure_returns_1.
        return 1
    return 0
