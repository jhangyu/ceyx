"""Minimum-runtime floor measurement + drift assertion (S-F1/S-F3),
generalised from WI-7's Linux-only version to all four platforms.

Frozen interface: docs/logs/2026-09-13/pyci-plan.md WI-16.

THREE SOURCE KINDS, dispatched as DATA never as a per-platform-name branch
(`targets.spec(platform)["min_runtime_source"]`, one of `dump` | `binary` |
`declaration`) -- this module never spells out a literal comparison
against the Android platform name at all, by construction (see
test_min_runtime_source_kinds_are_data_not_branches):

  * `dump`        (linux)   -- a `readelf --dyn-syms` CAPTURE, never the
                                binary itself.
  * `binary`      (windows, macos) -- the artifact(s) directly: Windows a
                                single staged `.dll`; macOS every staged
                                `.dylib` (the floor of the WHOLE staged
                                group, since the OpenMP runtime -- not the
                                decoder -- is what actually sets the macOS
                                floor).
  * `declaration` (android) -- NOT a binary property at all: android's
                                floor is minSdk, DECLARED in
                                `plugin/android/build.gradle`. Never
                                flattened into "an artifact path" -- there
                                is no artifact here, by design.

Every path shares the same two-step shape: `read_min_runtime.py` measures
and writes `MIN_RUNTIME_<platform>=<value>` to a file (its own
`READ_MIN_RUNTIME_RC` is PORTED AS-IS ungated, C-G4 item 2 -- the read can
fail loudly on its own stderr while this module still proceeds to the
drift check, exactly as today's shell does); then
`assert_min_runtime_matches_declared.py` asserts the measured value
against `native/deps/min_runtime_expected.toml`. No `::error::` line exists
in any of the four original shell steps for this gate -- do not add one.

`--arch` is per-arch DATA, not a special case: only macOS's declaration in
`min_runtime_expected.toml` is keyed per-arch (arm64/x86_64 measure
different OpenMP runtimes), so `min_runtime()` requires `arch` for macOS
and rejects it everywhere else (C-G9) -- `ci.py`'s argparse layer already
enforces this from `targets.spec(...)["requires_arch"]`, but a runtime
assert lives here too so a future direct call (a test, a script) cannot
skip it.

Every artifact path this module touches is derived entirely from
`targets.spec(...)` (`staged_dir`, `artifact_path`) -- no environment
variable is read here for that purpose (the real workflow's `$ARTIFACT_DIR`
is always `<repo root>/artifacts`, i.e. one level above `staged_dir`
("artifacts/native"), so `Path(staged_dir).parent` reconstructs it without
any env coupling; see orientation.py's docstring for why a module reaching
into workflow-supplied environment variables is the wrong shape).
"""

from __future__ import annotations

import sys
from pathlib import Path

from . import report, run, targets

_DECLARED_TOML = "native/deps/min_runtime_expected.toml"


def min_runtime(platform: str, arch: str | None = None) -> int:
    source = targets.spec(platform)["min_runtime_source"]
    if source == "dump":
        _reject_arch(platform, arch)
        return _from_dump(platform)
    if source == "binary":
        return _from_binary(platform, arch)
    if source == "declaration":
        _reject_arch(platform, arch)
        return _from_declaration(platform)
    raise ValueError(f"platform {platform!r} has unknown min_runtime_source {source!r}")


def _reject_arch(platform: str, arch: str | None) -> None:
    if arch is not None:
        raise ValueError(f"--arch is not accepted for platform {platform!r}")


def _require_arch(platform: str, arch: str | None) -> None:
    if arch is None:
        raise ValueError(f"--arch is required for platform {platform!r}")


def _measure_and_assert(read_args: list, out_path: str, platform: str, arch: str | None) -> int:
    """The shared two-step shape every source kind uses: measure (ported
    as-is, ungated RC), cat the emitted file, then assert drift."""
    rc, out = run.capture([sys.executable, "native/scripts/read_min_runtime.py", *read_args])
    if out:
        report.plain(out.rstrip("\n"))
    # PORTED AS-IS (C-G4 item 2): this RC is echoed but never gated -- the
    # drift check below is the only sub-check that can fail this function.
    report.marker("READ_MIN_RUNTIME_RC", rc)

    out_file = Path(out_path)
    if out_file.exists():
        text = out_file.read_text(errors="replace")
        if text:
            report.plain(text.rstrip("\n"))

    arch_args = ["--arch", arch] if arch else []
    drift_rc, drift_out = run.capture([
        sys.executable, "native/scripts/assert_min_runtime_matches_declared.py",
        "--emitted", out_path,
        "--declared", _DECLARED_TOML,
        "--platform", platform,
        *arch_args,
    ])
    if drift_out:
        report.plain(drift_out.rstrip("\n"))
    report.marker("MIN_RUNTIME_DRIFT_RC", drift_rc)
    return drift_rc


def _from_dump(platform: str) -> int:
    """linux: a `readelf --dyn-syms` capture, never the binary itself."""
    so = targets.spec(platform)["artifact_path"]
    run.run_to_file(["readelf", "--dyn-syms", so], "readelf_dynsyms.txt")
    return _measure_and_assert(
        ["--artifact", "readelf_dynsyms.txt", "--platform", platform, "--out", "min_runtime.txt"],
        "min_runtime.txt", platform, None,
    )


def _from_binary(platform: str, arch: str | None) -> int:
    """windows: the single staged DLL. macos: every staged dylib (the
    floor of the WHOLE staged group, arch-keyed)."""
    staged_dir = Path(targets.spec(platform)["staged_dir"])
    out_path = str(staged_dir.parent / "min_runtime.txt")

    if platform == "macos":
        _require_arch(platform, arch)
        dylib_paths = sorted(staged_dir.glob("*.dylib"))
        artifact_args = []
        for p in dylib_paths:
            artifact_args += ["--artifact", str(p)]
    else:  # windows
        _reject_arch(platform, arch)
        decoder_name = Path(targets.spec(platform)["artifact_path"]).name
        artifact_args = ["--artifact", str(staged_dir / decoder_name)]

    return _measure_and_assert(
        [*artifact_args, "--platform", platform, "--out", out_path],
        out_path, platform, arch,
    )


def _from_declaration(platform: str) -> int:
    """android: NOT a binary property. The floor is minSdk, DECLARED in
    plugin/android/build.gradle -- never flattened into an artifact path."""
    return _measure_and_assert(
        ["--gradle", "plugin/android/build.gradle", "--platform", platform, "--out", "min_runtime.txt"],
        "min_runtime.txt", platform, None,
    )
