"""Stage native artifacts + assert the atomic staged-group is complete.

Replaces linux_build.yml's three steps (d33cc607): "Stage native artifacts"
(:883-904), "Fail if no shared library was produced" (:906-924), and
"Assert the Linux shared-lib group is complete (atomic group)" (:925-959).
L23 and L24 fold into one command (``assert_staged_group``), per plan WI-8
(docs/logs/2026-09-13/pyci-plan.md:988-1054).

The round-6 shell this replaces (a bash-only glob-expansion builtin and a
bash array with a length expansion) died with a "builtin not found" exit
127 inside this leg's container, whose steps resolve to ``sh -e`` (dash),
AFTER the artifact had already been staged correctly -- that code path is
RETIRED here, not translated: no bash-only builtins, no arrays, no shell
glob anywhere in this module (see the guard grep in this WI's acceptance
criteria, which this docstring must not itself trip).

EXPECTED_SET comes from the single declaration (native/deps/shipped_files.toml,
via ``read_shipped_files.load_declaration`` -- imported, never re-spawned,
C-G3); STAGED_SET is enumerated from what actually landed in the staged
directory, filtered by the shared-object naming convention, never a second
hard-coded list -- a hard-coded STAGED_SET would make a staged-but-undeclared
extra file invisible (the 2026-09-01 "Linux HEIF staging silently absent"
incident's direction; this gate must also catch the opposite direction: an
UNDECLARED extra file staged alongside the declared set).
"""

from __future__ import annotations

import fnmatch
import re
import shutil
from pathlib import Path

from . import report, run, targets

# linux_build.yml:942's STAGED_SET filter, transcribed verbatim: a bare
# ``.so`` file or a versioned shared object (``.so.N``). This push is
# Linux-only (P1); a later platform push adds its own filter where it owns
# that module -- this is not a targets.py key because no push-3 WI declares
# ownership of targets.py (leader ruling).
#
# FORWARD CONDITION (leader ruling, WI-8 signoff): this hardcode is a
# deliberate Linux-only shortcut, not a pattern to copy sideways. The push
# that adds a second platform's staged-group filter (e.g. Windows' ``.dll``
# or macOS' ``.dylib``) must move this into targets.py as a per-platform
# key, under whichever WI that push designates as targets.py's owner --
# do not hardcode a second platform's pattern inline the way this one is.
_SO_NAME_RE = re.compile(r"\.so(\.[0-9]+)*$")


def declared_names(platform: str) -> list[str]:
    """decoder + companions, in declaration order. ``read_shipped_files`` is
    the single reader of shipped_files.toml (C-G3: imported, not
    re-spawned)."""
    import read_shipped_files

    entry = read_shipped_files.load_declaration()[platform]
    return [entry["decoder"], *entry["companions"]]


def _list_dir(directory: Path) -> None:
    """Prints the exact ``ls -la`` output of ``directory`` -- shelling out
    to the real ``ls`` rather than reimplementing its formatting keeps this
    byte-identical to the step it replaces without a second, drift-prone
    formatter."""
    result = run.run(["ls", "-la", str(directory)])
    report.plain(result.stdout.rstrip("\n"))


def stage(platform: str, artifact_dir: str, native_dir: str) -> int:
    """Replaces :899-904. Copies every declared file from
    ``<native_dir>/<dist_dir basename>`` into ``<artifact_dir>/native``. A
    missing declared source is a hard failure -- today's ``cp`` is
    deliberately NOT ``|| true``."""
    dest = Path(artifact_dir) / "native"
    dest.mkdir(parents=True, exist_ok=True)
    dist_dir_name = targets.spec(platform)["dist_dir"].split("/")[-1]
    src_root = Path(native_dir) / dist_dir_name
    for name in declared_names(platform):
        src = src_root / name
        if not src.exists():
            report.error(
                f"declared shipped file {name!r} is missing from {src_root} "
                "-- refusing to ship an incomplete group."
            )
            return 1
        shutil.copy2(src, dest / name)
    _list_dir(dest)
    return 0


def assert_staged_group(platform: str, artifact_dir: str) -> int:
    """Replaces :906-959 (L23 folded with L24). Emission order, exactly as
    today: ``ls -la`` -> ``SHARED_LIB_COUNT=<n>`` (on zero, error + exit 1)
    -> ``ls -la`` again (not deduplicated, AC-2) -> ``EXPECTED_SET:`` ->
    ``STAGED_SET:`` (on mismatch, error + exit 1) -> the atomic-group
    success line."""
    dest = Path(artifact_dir) / "native"
    _list_dir(dest)

    count = sum(
        1 for p in dest.iterdir() if fnmatch.fnmatch(p.name, "libdng_decoder_native*.so")
    )
    report.marker("SHARED_LIB_COUNT", count)
    if count == 0:
        report.error(
            f"No libdng_decoder_native*.so found in {dest}; the Linux build "
            "did not emit the expected artifact."
        )
        return 1

    _list_dir(dest)
    expected = sorted(declared_names(platform))
    staged = sorted(p.name for p in dest.iterdir() if _SO_NAME_RE.search(p.name))
    report.plain("EXPECTED_SET:")
    for name in expected:
        report.plain(name)
    report.plain("STAGED_SET:")
    for name in staged:
        report.plain(name)
    if expected != staged:
        report.error("Linux staged set does not match the shipped_files.toml declaration.")
        return 1
    report.plain("ATOMIC_GROUP_COMPLETE=1 (EXPECTED_SET == STAGED_SET)")
    return 0
