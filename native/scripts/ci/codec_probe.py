"""Compile + run the functional codec-capability probe (CI-T3 / D4/R-7).

Frozen interface: docs/logs/2026-09-13/pyci-plan.md WI-13. This is the ONLY
migrated CI step that invokes a compiler and the ONLY one that executes a
produced binary -- WI-5's `check_no_test_execution_in_ci.py` guard extension
carries the `probe_codecs` capability-probe carve-out (CLAUDE.md: "capability
probes ... are allowed as build-integrity checks") specifically so this
module's execution of its own freshly-compiled binary does not trip the
compile-only decree. That guard must already be on `main` before this lands.

CORRECTED 2026-09-13 (falsifying the plan's original premise): "one compile
shape differing only by compiler flags" is FALSE, verified by a direct read
of `macos_build.yml`, `windows_build.yml` and linux's step at the current
tip before writing a line of this module. Three real divergences:

1. **macOS's dist directory is workflow context, not a platform fact.**
   macOS reads its dist path from a `matrix.`-scoped YAML variable naming
   the per-arch HEIF dist directory (see `macos_build.yml`'s own matrix
   block for the exact key) -- a PER-ARCH MATRIX VALUE (native vs
   cross-arch legs get different dists). `targets.py` cannot hold this
   (per-leg workflow context, C-G14 rule 4), so macOS is the only platform
   this module accepts an explicit `dist_dir` argument for -- required
   there, rejected everywhere else, exactly as android's orientation call
   takes `--ndk-home`. This module's own source never spells out that
   YAML variable's name (see `test_macos_dist_dir_comes_from_flag_not_
   targets`, which greps this file for it and expects zero hits -- the
   flag value must come from the caller, never be re-derived here). Linux
   and Windows both use a fixed literal (`native/third_party/heif-dist-
   <os>`) derived from `workspace` alone.
2. **Windows emits no `file` line.** Linux and macOS both echo `file
   <probe>`'s output after compiling; Windows never has. Adding one would
   be an AC-2 delta (a marker that never existed in the baseline).
3. **Three runtime-resolution strategies, not one env var.** Linux carries
   an explicit `LD_LIBRARY_PATH` (belt-and-braces for a non-transitive
   RUNPATH -- see `_run_probe_linux`'s own docstring below for the full
   rationale, moved here verbatim from `linux_build.yml`'s comment).
   macOS carries `-Wl,-rpath` ONLY at compile time and adds **no** run-time
   env var at all -- `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH` do not appear on
   this leg today. Windows uses neither rpath nor an env var: it copies
   `heif.dll`/`libde265.dll` beside the compiled `.exe` before running it,
   because Windows resolves an imported DLL from the loading module's own
   directory first (same mechanism `dng_decoder_native`'s own staging
   already relies on).

ADDING `DYLD_LIBRARY_PATH` ON macOS "FOR SYMMETRY" IS A GATE-SEMANTICS
CHANGE REQUIRING A USER RULING (C-G4 class), not a tidy-up -- macOS ships no
such line today and `test_macos_run_env_has_no_library_path_var` pins that
absence so a future "symmetry" edit is a deliberate, visible red.

WINDOWS CLANG-CL PITFALLS (moved verbatim from `windows_build.yml`'s comment
above its probe-compile step, two Windows-specific hazards, R7):
  (1) clang-cl does not accept `-L`/`-l`; the import library is passed as a
      POSITIONAL `.lib` input instead (C-G7).
  (2) MSYS bash rewrites a leading `/X` argument into a Windows path (the
      same hazard `windows_build.yml`'s AC-W4 step documents) -- the
      windows compile command here uses only `-I` and `-o`, neither of
      which trips it.

LINUX/macOS TRANSITIVITY GAP (moved verbatim from `linux_build.yml`'s
comment above its probe-compile step): `-Wl,-rpath` at compile time only
covers `probe_codecs`'s OWN direct dependency (`libheif.so.1`), not
`libheif`'s own dependency on `libde265.so.0` under a non-transitive
`DT_RUNPATH` linker default -- belt-and-braces `LD_LIBRARY_PATH` closes
that gap on Linux. macOS ships no such belt-and-braces line (divergence 3
above); this asymmetry is a real, if inconsistent, fact about today's two
legs, ported as-is rather than "fixed" into false symmetry.

Output contract, all three legs identical: the probe's stdout+stderr is
captured to `<probe_dir>/probe_codecs.txt`, then `PROBE_CODECS_RC=<n>` is
BOTH appended to that file AND printed immediately (the `tee -a` behaviour),
then the whole file is printed again (the `cat` replay) -- this is why
`PROBE_CODECS_RC=<n>` appears in the emission list TWICE per run (C-G1);
`markerdiff.py`'s multiset comparison depends on this exact count. On
nonzero: `::error::probe_codecs exited <n> (nonzero bits = missing
capabilities)` on STDOUT, not stderr (the original has no `>&2`). On zero,
the verbatim tail line:
`A-CAP-HEVC-ENC / A-CAP-AV1-ENC / A-CAP-HEVC-DEC / A-CAP-AV1-DEC all present: RC=0`

The compiler is resolved through `tools.resolve("cc", platform)` with a
LOUD not-found naming the full search list -- round 5's CI red was a bare
`clang` command word that failed silently on a container missing it.
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

from . import report, run, targets, tools

_PROBE_SRC_REL = "native/scripts/deps/probe/probe_codecs.c"
_PROBE_DIR_REL = "native/scripts/deps/probe"

# Fixed dist-directory literals -- linux and windows only. macOS has none:
# its dist directory is a per-arch matrix value passed in as `dist_dir`
# (divergence 1 above), never a platform-fact constant like these.
_FIXED_DIST_DIR = {
    "linux": "native/third_party/heif-dist-linux",
    "windows": "native/third_party/heif-dist-windows",
}

_SUCCESS_TAIL = (
    "A-CAP-HEVC-ENC / A-CAP-AV1-ENC / A-CAP-HEVC-DEC / A-CAP-AV1-DEC all present: RC=0"
)


def _validate_dist_dir(platform: str, dist_dir: str | None) -> int | None:
    """C-G9 shape applied to `--dist-dir`: required for macOS, rejected
    everywhere else, checked in BOTH directions. Returns an exit code on
    rejection, or ``None`` when the argument is valid for `platform`."""
    if platform == "macos":
        if dist_dir is None:
            report.error("--dist-dir is required for --platform macos")
            return 2
    elif dist_dir is not None:
        report.error(f"--dist-dir is not accepted for --platform {platform!r}")
        return 2
    return None


def _resolve_dist(platform: str, workspace: str, dist_dir: str | None) -> str:
    if platform == "macos":
        return f"{workspace}/{dist_dir}"
    return f"{workspace}/{_FIXED_DIST_DIR[platform]}"


def _run_probe_linux(dist: str, probe_dir: Path) -> int:
    """Belt-and-braces `LD_LIBRARY_PATH`: `-Wl,-rpath` at compile time only
    covers `probe_codecs`'s own direct dependency on `libheif.so.1`, not
    `libheif`'s own dependency on `libde265.so.0` under a non-transitive
    `DT_RUNPATH` linker default -- this closes that gap. macOS carries no
    equivalent line (see module docstring, divergence 3). The probe path is
    inlined here (`str(probe_dir / "probe_codecs")`), not routed through a
    pre-assigned variable, so the compile-only guard's Python-side AST scan
    can statically resolve this argv[0] to the `probe_codecs`
    capability-probe carve-out -- the same exemption its YAML-side scan
    already grants, now provably present on the Python side too (WI-13's
    guard-evidence acceptance criterion)."""
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = f"{dist}/lib:" + env.get("LD_LIBRARY_PATH", "")
    result = run.run_to_file(
        [str(probe_dir / "probe_codecs")], probe_dir / "probe_codecs.txt", env=env
    )
    return _finish_probe_run(result, probe_dir)


def _run_probe_macos(probe_dir: Path) -> int:
    """No `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH` here -- macOS relies on
    `-Wl,-rpath` alone at compile time (see module docstring, divergence 3);
    `env=None` means the child inherits the parent's environment verbatim,
    with no library-path override of any kind. Probe path inlined for the
    same AST-resolvability reason documented on `_run_probe_linux`."""
    result = run.run_to_file(
        [str(probe_dir / "probe_codecs")], probe_dir / "probe_codecs.txt", env=None
    )
    return _finish_probe_run(result, probe_dir)


def _run_probe_windows(probe_dir: Path) -> int:
    """No rpath, no env var: `heif.dll`/`libde265.dll` are copied beside
    the compiled `.exe` before it runs, because Windows resolves an
    imported DLL from the loading module's own directory first. Unlike
    linux/macos, the `.exe`-suffixed path is built through a local
    variable rather than inlined: the compile-only guard's YAML-side scan
    strips a trailing `.exe` before matching the capability-probe
    allowlist, but its Python-side AST resolver does not (verified by
    reading `check_no_test_execution_in_ci.py`'s `_resolve_argv0_basename`
    -- it returns a literal's basename unstripped for both the plain-
    string and `Path(...)/"name"` shapes). An inlined
    `probe_dir / "probe_codecs.exe"` would therefore resolve to the exact
    string `"probe_codecs.exe"`, which is NOT in `ALLOWED_CAPABILITY_PROBES`
    (only the bare `"probe_codecs"` is) and would be misclassified as a
    FAILURE rather than the correct capability-probe ALLOWED case. Keeping
    this path in a variable makes argv[0] a `Name` node instead, which the
    resolver reports as `UNRESOLVED` (printed, non-failing -- the same
    treatment `verify_artifact.py` already gets for its own unresolved
    call sites) rather than a false failure. This asymmetry between the
    guard's YAML-side and Python-side `.exe` handling is a real gap,
    reported to the campaign lead rather than silently patched here (out
    of this module's ownership; `check_no_test_execution_in_ci.py` is not
    a WI-13 file)."""
    probe_bin = probe_dir / "probe_codecs.exe"
    result = run.run_to_file([str(probe_bin)], probe_dir / "probe_codecs.txt", env=None)
    return _finish_probe_run(result, probe_dir)


def _finish_probe_run(result, probe_dir: Path) -> int:
    rc = result.returncode
    probe_txt = probe_dir / "probe_codecs.txt"
    with probe_txt.open("a", encoding="utf-8") as fh:
        fh.write(f"PROBE_CODECS_RC={rc}\n")
    report.plain(f"PROBE_CODECS_RC={rc}")
    report.plain(probe_txt.read_text(encoding="utf-8").rstrip("\n"))
    if rc != 0:
        report.error(
            f"probe_codecs exited {rc} (nonzero bits = missing capabilities)",
            stream=sys.stdout,
        )
        return rc
    report.plain(_SUCCESS_TAIL)
    return 0


def codec_probe(platform: str, workspace: str, dist_dir: str | None = None) -> int:
    """Compile then run `native/scripts/deps/probe/probe_codecs.c` for
    `platform`, against the dist directory this leg's own earlier "Build
    HEIF stack" step already produced. Returns the probe's own exit code
    (nonzero bits = missing capabilities), or a validation/tool-resolution
    error code."""
    invalid = _validate_dist_dir(platform, dist_dir)
    if invalid is not None:
        return invalid

    try:
        cc = tools.resolve("cc", platform)
    except tools.ToolNotFound as exc:
        report.error(str(exc))
        return 2

    dist = _resolve_dist(platform, workspace, dist_dir)
    probe_dir = Path(workspace) / _PROBE_DIR_REL
    probe_src = Path(workspace) / _PROBE_SRC_REL
    spec = targets.spec(platform)

    if spec["probe_link_style"] == "clang-cl":
        probe_bin = probe_dir / "probe_codecs.exe"
        compile_argv = [
            cc,
            f"-I{dist}/include",
            str(probe_src),
            f"{dist}/lib/heif.lib",
            "-o",
            str(probe_bin),
        ]
    else:
        probe_bin = probe_dir / "probe_codecs"
        compile_argv = [
            cc,
            f"-I{dist}/include",
            f"-L{dist}/lib",
            f"-Wl,-rpath,{dist}/lib",
            "-o",
            str(probe_bin),
            str(probe_src),
            "-lheif",
        ]

    compile_result = run.run(compile_argv)
    if compile_result.returncode != 0:
        return compile_result.returncode

    if platform in ("linux", "macos"):
        file_result = run.run(["file", str(probe_bin)])
        report.plain(file_result.stdout.rstrip("\n"))
        if file_result.returncode != 0:
            return file_result.returncode

    if platform == "windows":
        shutil.copy2(Path(dist) / "bin" / "heif.dll", probe_dir / "heif.dll")
        shutil.copy2(Path(dist) / "bin" / "libde265.dll", probe_dir / "libde265.dll")
        return _run_probe_windows(probe_dir)
    if platform == "macos":
        return _run_probe_macos(probe_dir)
    return _run_probe_linux(dist, probe_dir)
