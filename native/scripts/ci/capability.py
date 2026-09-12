"""Codec/build capability-vector assertions: linux_build.yml's G1/S-E2
steps, windows_build.yml's G1/S-E2 steps, and macos_build.yml's native-leg
G1/S-E2 (dlopen probe) plus cross-leg G1/S-E2 (configure-log literal match).

Frozen interface: docs/logs/2026-09-13/pyci-plan.md WI-16 (the capability.py
half; `minruntime.py` is WI-16b, a disjoint file/test/golden set owned
separately this push -- D2 permits exactly one `targets.py` owner per push
and minruntime needs new `targets.py` keys, capability.py does not). Every
emitted line below is transcribed verbatim from the current-tip workflow
bodies -- this module changes WHERE the assertion runs, never WHAT it
asserts or prints.

ANDROID IS NOT A CAPABILITY-VECTOR LEG (verified 2026-09-13:
`grep -n codec_capability_probe.py .github/workflows/*.yml` returns ZERO
android hits, confirmed by two independent reads before this module was
written). `android_build.yml:216`'s "Assert build capability vector via
probe (S-E2)" step is a single `echo` of an honest, permanently-accepted
SKIP line -- it is NOT a call to `codec_capability_probe.py`, is out of
scope for this module, and is preserved byte-for-byte by another WI (do not
"unify" around it). `capability_vector(platform="android", ...)` raises
ValueError; no android golden exists; `--platform android` is not a
supported target of the `capability-vector` subcommand.

TWO SOURCES, not one function branching on data (plan's own framing,
pyci-plan.md:1682): `source="probe"` covers linux, macOS's NATIVE leg
(`matrix.cross == 'false'`) and windows -- all three dlopen the built
artifact via `codec_capability_probe.py`'s own
`ceyx_still_decode_supports`/`ceyx_encode_supports`/`ceyx_build_capabilities`
FFI surface (see that module's docstring for why a symbol-presence check has
zero discriminating power against a static codec archive). `source=
"configure-log"` covers ONLY macOS's CROSS leg (`matrix.cross == 'true'`): a
foreign-arch dylib cannot be dlopen'd from this host process, so that leg
instead greps a literal out of `cross_stage2_build.log` at configure time --
a genuinely different algorithm with its own marker vocabulary (no
`CODEC_CAPABILITY_PROBE_RC`/`BUILD_CAPABILITY_PROBE_RC` at all), ported
as-is rather than unified into the probe path.

TWO DIVERGENCES the plan's Behavior section does not name, found by a
three-leg body diff (file:line, current tip) before this module was
designed:

1. **`--json-out` is present on macOS's native leg (macos_build.yml:664,682)
   and windows (windows_build.yml:782,803), ABSENT on linux
   (linux_build.yml:560-604) for both kinds.** Modelled here as a plain
   optional `json_out` parameter, not a `targets.py` fact: which YAML
   caller passes it is a WI-17/WI-18 wiring decision, not a per-platform
   constant this module should hardcode. `capability_vector()` writes to
   whatever path it is given (creating the parent directory first, mirroring
   the `mkdir -p native/scripts/deps/probe` both callers do), or writes
   nothing when `json_out` is `None` -- exactly `codec_capability_probe.py`'s
   own `--json-out` contract, forwarded unmodified.
2. **Linux re-exports `LD_LIBRARY_PATH` before BOTH probe calls**
   (linux_build.yml:576,600) -- the same non-transitive `DT_RUNPATH`
   belt-and-braces gap `codec_probe.py`'s `_run_probe_linux` already
   documents for the compiled functional probe, ported as-is here for the
   dlopen-based capability probe. macOS/windows set no equivalent env var
   for these steps (macOS relies on `-Wl,-rpath` baked in at decoder link
   time; windows relies on `heif.dll`/`libde265.dll` already being staged
   beside the decoder DLL by an earlier step, so the loader's own
   same-directory search finds them). `_LINUX_HEIF_LIB_DIR_REL` below is a
   WI-16-local fact for the same reason `codec_probe.py`'s
   `_FIXED_DIST_DIR` is local rather than in `targets.py` (D2): no other
   module needs it, and `targets.py` has exactly one owner this push
   (impl-pyci-5-sonnet, WI-16b).

The macOS-native `dylib_path` requirement mirrors `orientation.py`'s own
`dylib_path` parameter: macOS's `artifact_path` is `None` in `targets.py`
because two per-arch matrix legs build into different paths resolved only
at runtime by the caller -- this module does not re-derive that path, the
caller (the workflow's `$DYLIB`, forwarded by `ci.py`) passes it in.

`arch` is accepted for CLI-shape uniformity with the other `ci.py`
subcommands (the C-G9 family) but is not read anywhere in this module:
neither leg's real `codec_capability_probe.py` invocation passes `--arch`
today (verified by grepping all three legs) -- macOS's per-arch identity is
already fully carried by which matrix job is running (a different `$DYLIB`
value, a different `matrix.cross`), not by an extra flag to this probe.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

from . import report, targets

# native/scripts/codec_capability_probe.py is a sibling top-level script
# (not part of this package) exposing a programmatic `main(argv)` entry
# point -- imported directly (like verify_artifact.py's `assert_exports`),
# not shelled out to, so this module reuses its exact parsing/printing/
# JSON logic in-process instead of re-implementing it (zero risk of the
# reimplementation silently drifting from the original's emitted text).
_SCRIPTS_DIR = Path(__file__).resolve().parents[1]
if str(_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS_DIR))
import codec_capability_probe as _probe  # noqa: E402

# WI-16-local fact, not a targets.py entry (D2: exactly one targets.py owner
# per push, and no other module needs this) -- mirrors codec_probe.py's own
# _FIXED_DIST_DIR precedent. linux_build.yml:576,600.
_LINUX_HEIF_LIB_DIR_REL = "native/third_party/heif-dist-linux/lib"

_RC_MARKER = {"codec": "CODEC_CAPABILITY_PROBE", "build": "BUILD_CAPABILITY_PROBE"}

# source="configure-log" literals (macos_build.yml:717 codec check,
# :755 build check), transcribed verbatim -- this leg cannot dlopen a
# foreign-arch dylib, so it proves the same thing at configure time via
# cmake's own success/status line instead of the runtime probe.
_CONFIGURE_LOG_CHECK = {
    "codec": {
        "literal": "-- JXL: static",
        "label": "JXL enabled",
        "error": (
            "cross_stage2_build.log does not show '-- JXL: static ...' from "
            "cmake/jxl.cmake's success branch; the x86_64 cross-compile "
            "configure may not have enabled JXL."
        ),
    },
    "build": {
        "literal": "[ceyx] LCMS2: disabled (OQ-N4 option Z)",
        "label": "ICC=0",
        "error": (
            "cross_stage2_build.log does not show the expected '[ceyx] "
            "LCMS2: disabled (OQ-N4 option Z)' line -- ENABLE_LCMS may not "
            "be forced OFF on this leg."
        ),
    },
}


def capability_vector(
    platform: str,
    kind: str,
    arch: str | None = None,
    source: str = "probe",
    dylib_path: str | None = None,
    expect: list[str] | None = None,
    expect_cap: list[str] | None = None,
    json_out: str | None = None,
    workspace: str = ".",
    log_path: str = "cross_stage2_build.log",
) -> int:
    """Dispatches to the platform's real algorithm. `kind` in {"codec",
    "build"}; `source` in {"probe", "configure-log"}. `dylib_path` is
    required for macOS when `source="probe"` (macOS's `artifact_path` is
    `None` in targets.py, see module docstring) and rejected everywhere
    else. `expect`/`expect_cap` are the raw `--expect`/`--expect-cap`
    tokens (C-G18: they stay in YAML, forwarded here verbatim, never
    re-derived). Returns the underlying check's exit code."""
    if kind not in ("codec", "build"):
        raise ValueError(f"capability_vector: unknown kind {kind!r}; want 'codec' or 'build'")
    if source not in ("probe", "configure-log"):
        raise ValueError(
            f"capability_vector: unknown source {source!r}; want 'probe' or 'configure-log'"
        )
    if platform == "android":
        raise ValueError(
            "capability_vector: android is not a capability-vector leg "
            "(android_build.yml's S-E2 step is an honest SKIP echo, not a "
            "probe call -- see module docstring)"
        )

    if source == "configure-log":
        if platform != "macos":
            raise ValueError(
                "capability_vector: source='configure-log' is only valid for "
                f"platform='macos' (the cross leg), got platform={platform!r}"
            )
        if dylib_path is not None:
            raise ValueError(
                "capability_vector: dylib_path is not accepted with source='configure-log'"
            )
        if json_out is not None:
            raise ValueError(
                "capability_vector: json_out is not accepted with source='configure-log' "
                "(the cross leg's configure-log check writes no JSON today)"
            )
        return _configure_log_check(kind, workspace, log_path)

    # source == "probe"
    if platform == "macos":
        if not dylib_path:
            raise ValueError(
                "capability_vector(platform='macos', source='probe') requires dylib_path"
            )
        lib_path = dylib_path
    else:
        if dylib_path is not None:
            raise ValueError(
                f"capability_vector: dylib_path is not accepted for platform={platform!r}"
            )
        lib_path = targets.spec(platform)["artifact_path"]

    return _probe_check(platform, kind, lib_path, expect or [], expect_cap or [], json_out)


def _probe_check(
    platform: str,
    kind: str,
    lib_path: str,
    expect: list[str],
    expect_cap: list[str],
    json_out: str | None,
) -> int:
    if json_out:
        Path(json_out).parent.mkdir(parents=True, exist_ok=True)

    if platform == "linux":
        # Belt-and-braces LD_LIBRARY_PATH (see module docstring, divergence
        # 2): mutating os.environ here (not a local dict) is deliberate --
        # glibc's dynamic loader re-consults LD_LIBRARY_PATH on every dlopen
        # call from this same process, so this takes effect for the ctypes
        # CDLL() call codec_capability_probe.main() is about to make.
        os.environ["LD_LIBRARY_PATH"] = (
            f"{_LINUX_HEIF_LIB_DIR_REL}:" + os.environ.get("LD_LIBRARY_PATH", "")
        )

    argv = [lib_path]
    for e in expect:
        argv += ["--expect", e]
    for e in expect_cap:
        argv += ["--expect-cap", e]
    if json_out:
        argv += ["--json-out", json_out]

    rc = _probe.main(argv)
    report.rc(_RC_MARKER[kind], rc)
    return rc


def _configure_log_check(kind: str, workspace: str, log_path: str) -> int:
    spec = _CONFIGURE_LOG_CHECK[kind]
    path = Path(workspace) / log_path
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        text = ""
    rc = 0 if spec["literal"] in text else 1
    report.plain(f"ASSERT cross-leg {spec['label']} RC={rc}")
    if rc != 0:
        report.error(spec["error"])
        return 1
    return 0
