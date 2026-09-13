"""The carrier invocation and the dist listing (WI-29, push 8b, the dist-
workflow python-ization -- user ruling P-3=(a) put the six third-party dist
workflows in scope).

Frozen interface (docs/logs/2026-09-13/pyci-plan.md WI-29; argv shapes are
the interface WI-31/WI-32/the ratchet WI consume -- this module writes
nothing else):

    dist-build  --component {heif-stack,jxl-stack,webp-stack,libjxl,libwebp}
                --platform {android,windows} --arch <arch> --dist <path>
                [--android-ndk <path>] --rc-marker <TOKEN>
    dist-list   --dist <path>

Twin-diff finding (the plan's precondition, verified against the tree
before writing a line, per WI-29's brief):

* `dist-build` carries no provisioning logic of its own -- provisioning
  (Ninja / apt / clang-cl) is a SEPARATE concern and lives in `provision.py`
  (Ninja, apt) and `windows_toolchain.py` (clang-cl, already ported by
  WI-24). See `provision.py`'s module docstring for the three-case finding:
  Ninja and apt were NOT migrated anywhere before this push (their bodies
  are still raw shell in `windows_build.yml`/`android_build.yml` themselves,
  so there was no twin to reuse -- case "push 8 did not migrate a twin");
  `locate-clang-cl` WAS already migrated, but only in the parent
  (`windows_build.yml:127`) via `windows_toolchain.locate_clang_cl` -- the
  three Windows dist twins (heif/jxl/webp_dist_windows.yml) still carry the
  pre-migration raw if/elif/else shell body verbatim, so the dist twins
  reuse that existing function rather than gaining a second implementation.
* `dist-build` itself (the `build_deps.py build <component> ...` invocation)
  and `dist-list` (the `find <dist> -type f | sort` replacement) have no
  prior implementation anywhere in the tree; both are new in this module.

Component argv per the tree (`build_deps.py build <component>`,
native/scripts/build_deps.py:17-35): Windows carriers pass the `*-stack`
spelling (heif_dist_windows.yml:221 `heif-stack`, jxl_dist_windows.yml:119
`jxl-stack`, webp_dist_windows.yml:97 `webp-stack`); the two Android carriers
that build a single library rather than the whole HEIF assembly pass the
bare library name instead (jxl_dist_android.yml:77 `libjxl`,
webp_dist_android.yml:78 `libwebp`); `heif_dist_android.yml:96` uses
`heif-stack` on both platforms. `dist_build()` takes `component` as a plain
string and does not validate it against this list -- `build_deps.py` is the
sole owner of what a valid component name is (R13: this module is a CLI
pass-through, not a second source of truth for that set).
"""

from __future__ import annotations

import sys
from pathlib import Path

from . import report, run

# native/scripts/ci/dist_build.py -> native/scripts/ci -> native/scripts ->
# native -> repo root.
_REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent
_BUILD_DEPS = _REPO_ROOT / "native" / "scripts" / "build_deps.py"


def dist_build(
    component: str,
    platform: str,
    arch: str,
    dist: str,
    rc_marker: str,
    android_ndk: str | None = None,
) -> int:
    """Invokes `build_deps.py build <component> --platform P --arch A --dist D
    [--android-ndk H]`, emits `<rc_marker>=<rc>` through `report.marker`
    (the token is supplied by the caller, per-workflow -- this function
    never derives it from `component`), and returns that same rc.

    The child's return code is read from `result.returncode`, the field on
    the `RunResult` `run.run()` returns -- adjacent to the call that
    produced it, never from a shell variable or a pipeline (this campaign's
    own R2/`run.py` discipline)."""
    argv = [
        sys.executable, str(_BUILD_DEPS), "build", component,
        "--platform", platform, "--arch", arch, "--dist", dist,
    ]
    if android_ndk:
        argv += ["--android-ndk", android_ndk]

    result = run.run(argv)
    rc = result.returncode
    if result.stdout:
        report.plain(result.stdout.rstrip("\n"))
    if result.stderr:
        report.plain(result.stderr.rstrip("\n"))
    report.marker(rc_marker, rc)
    return rc


def dist_list(dist: str) -> int:
    """Replaces `find <dist> -type f | sort` (all six *_dist_*.yml's "List
    the produced dist (complete)" step, deliberately unfiltered -- a
    filtered listing looks like a full inventory while silently omitting
    an artifact class, per every one of those steps' own comment).

    Sorted, one path per line via `report.plain`, dotfiles included
    (`Path.rglob("*")` does not skip them, unlike shell globbing without
    `dotglob` -- the artifact upload depends on a `.pins` file surviving
    this listing). Refuses to succeed silently: a missing directory or an
    empty tree exits 1 with an `::error::` line rather than printing
    nothing and returning 0 -- the exact "ran, did nothing, still green"
    shape this campaign keeps finding (report.github_env_append's sibling
    discipline)."""
    root = Path(dist)
    if not root.is_dir():
        report.error(f"dist-list: {dist} is not a directory")
        return 1

    files = sorted(str(p) for p in root.rglob("*") if p.is_file())
    if not files:
        report.error(f"dist-list: no files found under {dist}")
        return 1

    for path in files:
        report.plain(path)
    return 0
