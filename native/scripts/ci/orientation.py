"""Fused-orientation Halide-kernel capability gate ("Task 11 / G-E"): does
the just-built artifact actually contain the fused-orientation kernels, not
just the plain FFI export surface (which `verify_artifact.assert_exports`
already checks and cannot distinguish a fused build from a stale
pre-fusion one, since the generator's symbol NAMES never changed)?

Frozen interface: docs/logs/2026-09-13/pyci-plan.md WI-10 (amended after
this module's own finding falsified the plan's original premise -- see
below).

THREE ALGORITHMS BEHIND ONE COMMAND, not two (RULING, lead3-pyci-opus,
2026-09-13, superseding the plan's original C-G6 text): the plan as first
written asserted `_strings_scan` "reproduces linux_build.yml:728-770
exactly" across linux/windows/android, differing only by DATA
(`REQUIRED_SYMBOLS`, `targets.spec(...)["strings_tools"]`). That premise is
FALSE -- verified against all three real workflow steps before this module
was written. Android differs by ALGORITHM, not by data, so it is its own
function (`_android_scan`), not a branch inside `_strings_scan`:

  * `_strings_scan` (linux + windows ONLY): resolve one strings tool via
    `targets.spec(platform)["strings_tools"]` in preference order, scan its
    output for the required symbol(s) and the six coefficients, both with
    an EXACT WHOLE-LINE match (`grep -qx` semantics).
  * `_android_scan` (android ONLY): TWO tools (the NDK's own `llvm-nm -D`
    for the kernel-name symbol(s), plain `strings` for the coefficients),
    TWO separate dump files, and -- the load-bearing divergence -- a
    SUBSTRING match (`grep -q`, no `-x`) for the symbol check only; the
    coefficient check is still whole-line. No PATH-fallback branch exists
    for Android at all: the NDK's llvm-nm is a fixed, already-verified
    path, and `strings` runs unconditionally. None of this is "fixed"
    here: it is today's real, if inconsistent, gate. Changing Android's
    symbol match to whole-line, or giving it a PATH-fallback branch, is a
    deliberate behaviour change that must show up as a deliberate red in
    `test_android_symbol_check_is_substring_ported_as_is` --  not as a
    quiet "unification" of something that only looked alike.
  * `_compiled_probe` (macOS ONLY): dlopen+dlsym the real
    `orient_capability_probe` binary against the dylib -- valid only on
    macOS, which default-exports every symbol unless explicitly hidden.

WHY C-G6's letter was not enough to catch this: per-platform branching on
tool-count/dump-count/match-semantics inside one function would have
satisfied "one command, two code paths" while violating its actual intent
(don't unify things that only look alike) -- that shape is exactly how a
future reader "simplifies" Android's substring match into the whole-line
match sitting right next to it in the same function, silently strengthening
a check CI has never enforced that way. Three honestly-named functions make
the divergence visible in the function list instead of hiding it behind an
`if platform == "android"`.

THIS MODULE DOES NOT READ WORKFLOW-SUPPLIED ENVIRONMENT VARIABLES ITSELF
(a module that reaches into the environment is untestable and silently
couples itself to one caller): `assert_orientation`'s macOS/android
branches take `dylib_path`/`artifact_dir`/`ndk_home` as explicit
parameters instead -- the caller (ci.py's dispatch) is responsible for
reading the workflow's own NDK-toolchain-root env var and passing it in as
`ndk_home`. Acceptance criterion: this module's source contains no
reference to that variable's own name.

LOCKSTEP NOTE (moved here from the pre-migration YAML comments in
`linux_build.yml`, `windows_build.yml`, `android_build.yml`, and
`macos_build.yml` -- WI-10/11/12 handoff): the literal sets below
(`REQUIRED_SYMBOLS`, `REQUIRED_COEFFS`) are coupled to
`native/tests/orient_capability_probe.cpp`'s `kRequiredArgs`/`kChecks` by
construction (see that file's header comment). Any future kernel-signature
change must update all of these together -- now one Python dict instead of
four duplicated shell `for` loops. Android/Linux/Windows all link the
Vulkan SPLIT Stage4 kernel (`dng_render_stage4_split`, `DNG_STAGE4_SPLIT_
KERNEL=ON`); only macOS/Metal links `dng_render_stage4_scaled_preavg`
instead, which is why macOS uses the compiled probe rather than this
literal set at all. Android additionally checks the plain
`dng_render_stage4` literal (not just the `_split` variant) -- CI-red
Round 3 root cause: this literal was still `scaled_preavg`-shaped stale
data from before the split kernel existed, so both the base and the split
names are asserted on Android to catch that class of drift again.

RATIONALE FOR NOT USING A RUNTIME dlopen+dlsym PROBE ON linux/windows/
android (why `_strings_scan`/`_android_scan` exist at all instead of
reusing `orient_capability_probe` everywhere): Linux/Windows restrict their
dynamic export surface to the hand-written FFI symbols only (see
`verify_artifact.assert_exports`'s manifest-driven check for that
allowlist), so Halide's AOT-generated `<kernel>_metadata()` introspection
entry points are never in the dynamic symbol table for `dlsym` to find --
this was measured directly (CI-red diagnosis, round 2): the runtime probe
failed on a genuinely fused, correctly-loaded .so with
`PROBE_RESULT=missing:dng_render_stage4_metadata`. Do NOT widen either
platform's export surface to make a runtime probe work -- that would ship
a wider ABI purely for a test's convenience. Android has the same
restricted-export problem AND is cross-arch with no emulator in this
compile-only CI, so a literal scan is the only leg-honest option there
too. macOS is the only platform where the runtime probe is valid, because
it default-exports every symbol unless explicitly hidden -- an asymmetry
now proven by two independent platform failures, not merely assumed.

Output is always to a FILE, then read/grepped in Python -- never
`tool | grep -q` (a PRESENT match SIGPIPE-kills the producer under
pipefail, which reads as a false failure; G-9/R-16).
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
    "linux": "tmp/so_strings.txt",
    "windows": "tmp/dll_strings.txt",
}


def assert_orientation(
    platform: str,
    arch: str | None = None,
    dylib_path: str | None = None,
    artifact_dir: str | None = None,
    ndk_home: str | None = None,
) -> int:
    """Dispatches to the platform's real algorithm. `dylib_path` is
    required for macos; `artifact_dir`/`ndk_home` are required for android
    (workflow-supplied context, taken as parameters -- see module
    docstring on why this module never reads them from the environment
    itself)."""
    if platform == "macos":
        if not dylib_path:
            raise ValueError("assert_orientation(platform='macos') requires dylib_path")
        return _compiled_probe(platform, dylib_path)

    if platform == "android":
        if not artifact_dir:
            raise ValueError("assert_orientation(platform='android') requires artifact_dir")
        if not ndk_home:
            raise ValueError("assert_orientation(platform='android') requires ndk_home")
        matches = sorted(glob.glob(os.path.join(artifact_dir, "native", "libdng_decoder_native*.so")))
        if not matches:
            report.error(f"no libdng_decoder_native*.so found under {artifact_dir}/native")
            return 1
        return _android_scan(matches[0], "android_so_strings.txt", ndk_home)

    so = targets.spec(platform)["artifact_path"]
    return _strings_scan(platform, so, _OUT_FILES[platform])


def _resolve_strings_tool_name(platform: str) -> str | None:
    """Returns the first candidate NAME (not a resolved path) from
    `targets.spec(platform)["strings_tools"]` that `shutil.which` finds --
    the shell's `STRINGS_TOOL` marker is always the tried NAME
    ("llvm-strings"/"strings"), never a resolved absolute path."""
    for name in targets.spec(platform)["strings_tools"]:
        if shutil.which(name):
            return name
    return None


def _strings_scan(platform: str, binary_path: str, out_path: str) -> int:
    """linux + windows ONLY -- see module docstring for why Android is a
    separate function, not a third platform handled here."""
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


def _android_scan(binary_path: str, out_path: str, ndk_home: str) -> int:
    """ANDROID ONLY -- a genuinely different algorithm from `_strings_scan`,
    not a variant of it (see module docstring's RULING section): two tools,
    two dumps, and a SUBSTRING (not whole-line) match for the symbol check.
    PORTED AS-IS (pre-existing, d33cc607 android_build.yml, "Assert
    fused-orientation kernel signals present in Android .so"). See
    test_android_symbol_check_is_substring_ported_as_is."""
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
    step. Native leg only (a cross-arch host cannot dlopen the other leg's
    dylib) -- that gating is the caller's responsibility (WI-12/ci.py), not
    this function's."""
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
