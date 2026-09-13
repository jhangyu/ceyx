"""Stage native artifacts + assert the atomic staged-group is complete, for
all four platforms.

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

Push 7 (WI-19/20/21, lead6 rulings 1-3, docs/logs/2026-09-13): this module
now also stages Windows, macOS and Android, transcribed directly from the
live YAML (windows_build.yml:805-872, macos_build.yml:823-861,
android_build.yml:221-263), never from plan prose (two premises in the
original push-7 brief were already falsified by the tree: windows'
``dist_dir`` is NOT ``None``, and android's completeness check is NOT
symmetric like linux's).

Two rulings that shape every function below:

* RULING 1 (stage.py stages, it does not verify): macOS's YAML fuses staging
  with three extra binary-inspection gates (arch/reachability/rpath-
  convention). Those gates are NOT ported here -- they are
  ``verify_artifact.py``'s subject (impl-16-owned) and must keep executing
  in the same relative order as today for AC-2. ``stage_macos`` below is
  copy-only, matching the "Stage native artifact" step's copy lines, not its
  gate lines.
* RULING 2 (source dirs are CLI flags, not targets.py keys): macOS resolves
  its source dir from ``dirname($DYLIB)`` (a CMake-exported env var) and
  android from a hardcoded ``${NATIVE_DIR}/build-android/android-arm64`` --
  both are per-leg workflow context, not a platform fact, per the
  ``json_out``/``--dist-dir`` precedent. Every non-linux stage function below
  takes its source directory as an explicit parameter; nothing here reads
  ``targets.spec(...)["dist_dir"]`` except the pre-existing linux path.
* RULING 3 (preserve each platform's algorithm exactly, do not harmonise):
  windows excludes the optional ``.lib`` import library from its
  completeness check; android's completeness check is deliberately
  asymmetric (required-companions-present only, no undeclared-extra
  detection -- ``libc++_shared.so``'s conditional presence is legitimate);
  macOS has no completeness line at all. Marker tokens
  (``ATOMIC_GROUP_COMPLETE`` vs ``ANDROID_COMPANION_GROUP_COMPLETE``) are
  different strings by design and are not unified.
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
    """INCLUDES the decoder: returns decoder + companions, in declaration
    order. Used by linux/windows, whose completeness checks are symmetric
    over the whole group. For a companions-ONLY list (android's asymmetric
    check), use ``declared_companions`` below instead -- do not slice this
    list's ``[1:]`` as a substitute, since that silently assumes the decoder
    is always index 0. ``read_shipped_files`` is the single reader of
    shipped_files.toml (C-G3: imported, not re-spawned)."""
    import read_shipped_files

    entry = read_shipped_files.load_declaration()[platform]
    return [entry["decoder"], *entry["companions"]]


def declared_companions(platform: str) -> list[str]:
    """EXCLUDES the decoder: companions only. android's completeness check
    (unlike linux/windows, see ``declared_names`` above) is asymmetric and
    only ever checks companions, never the decoder itself, by this list
    (android_build.yml:238 uses ``read_shipped_files.py --platform android
    --companions``, not ``--all``)."""
    import read_shipped_files

    return list(read_shipped_files.load_declaration()[platform]["companions"])


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


# --- Windows -----------------------------------------------------------
#
# windows_build.yml:805-834. Four files are hard-required (the decoder plus
# three companions from shipped_files.toml); a fifth, the import library
# (``dng_decoder_native.lib``), is copied ONLY IF PRESENT and is deliberately
# excluded from the completeness check (windows_build.yml:823-825: "The
# import library is only produced when the linker sees exports; copy it when
# present"). ``source_dir`` is a caller-supplied path (ruling 2: it is
# ``native/build-windows``, i.e. ``targets.spec("windows")["dist_dir"]``, but
# this module does not read targets.py itself -- the caller resolves it).

_WINDOWS_OPTIONAL_LIB = "dng_decoder_native.lib"


def stage_windows(source_dir: str, artifact_dir: str) -> int:
    """Replaces windows_build.yml:805-833's copy lines (not its own step's
    trailing ``ls -la``, which callers still get via ``_list_dir`` below)."""
    dest = Path(artifact_dir) / "native"
    dest.mkdir(parents=True, exist_ok=True)
    src_root = Path(source_dir)
    for name in declared_names("windows"):
        src = src_root / name
        if not src.exists():
            report.error(
                f"declared shipped file {name!r} is missing from {src_root} "
                "-- refusing to ship an incomplete group."
            )
            return 1
        shutil.copy2(src, dest / name)
    optional_src = src_root / _WINDOWS_OPTIONAL_LIB
    if optional_src.exists():
        shutil.copy2(optional_src, dest / _WINDOWS_OPTIONAL_LIB)
    else:
        report.notice(
            f"{_WINDOWS_OPTIONAL_LIB} not present (import library not emitted); "
            "DLL-only artifact."
        )
    _list_dir(dest)
    return 0


def _dll_set_marker_value(names: list[str]) -> str:
    """windows_build.yml:867/869 pipe both sides through
    ``tr '\\n' ' '`` on ``sort``'s output. ``sort`` always terminates a
    non-empty list with a trailing newline, so ``tr`` turns THAT into a
    TRAILING SPACE too -- not just a separator between items. Verified
    against the real shell pipeline byte-for-byte with ``od -c``
    (impl-pyci-15-sonnet, lead6 ruling on the wire format). This is
    deliberate and must not be "tidied" back to a plain ``" ".join(...)``,
    which silently drops the trailing byte AC-2 diffs against.
    ``" ".join(names) + " "`` only for a non-empty list -- an empty list
    never reaches ``sort`` with a trailing newline to convert."""
    return " ".join(names) + " " if names else ""


def assert_staged_group_windows(artifact_dir: str) -> int:
    """Replaces windows_build.yml:862-872. Symmetric ``*.dll``-only compare
    (the ``.lib`` is never part of either set, matching the shell's
    ``ls *.dll`` glob) -- unlike linux, no ``SHARED_LIB_COUNT``-style marker
    and no atomic-group success line; the shell step is silent on match.

    The EMITTED marker value carries the shell's trailing space (see
    ``_dll_set_marker_value``); the COMPARISON below is on the plain Python
    lists, which is whitespace-insensitive by construction -- exactly
    matching windows_build.yml:871's ``echo … | xargs`` normalisation before
    its own ``!=``. Do not compare the marker STRINGS instead of the lists:
    that would make the comparison stricter than the shell's, which is a
    migration-fidelity violation in the other direction (tightening)."""
    dest = Path(artifact_dir) / "native"
    _list_dir(dest)
    expected = sorted(n for n in declared_names("windows") if n.endswith(".dll"))
    staged = sorted(p.name for p in dest.iterdir() if p.name.endswith(".dll"))
    report.marker("EXPECTED_DLL_SET", _dll_set_marker_value(expected))
    report.marker("STAGED_DLL_SET", _dll_set_marker_value(staged))
    if expected != staged:
        report.error(
            "staged Windows DLL set does not match native/deps/shipped_files.toml's "
            f"declaration — expected [{' '.join(expected)}], found [{' '.join(staged)}]."
        )
        return 1
    return 0


# --- macOS ---------------------------------------------------------------
#
# macos_build.yml:823-834's copy lines ONLY (ruling 1): the same YAML step
# also runs three verification gates (arch/reachability/rpath-convention)
# that stay out of this module -- they belong to verify_artifact.py
# (impl-16-owned) and must keep running, in the same order relative to this
# copy, for AC-2. ``dylib_path`` and ``companions`` are caller-supplied
# (ruling 2): the source dir is ``dirname(dylib_path)``, resolved at runtime
# from a CMake-exported env var this module cannot know about, and
# ``companions`` is the caller's ``read_shipped_files.py --platform macos
# --companions`` result (macos_build.yml:833).


def stage_macos(dylib_path: str, companions: list[str], artifact_dir: str) -> int:
    """Replaces macos_build.yml:824-834. Hard-fails (matching the shell's
    unguarded ``exit 1``) if a declared companion is missing next to the
    decoder."""
    dest = Path(artifact_dir) / "native"
    dest.mkdir(parents=True, exist_ok=True)
    dylib = Path(dylib_path)
    shutil.copy2(dylib, dest / dylib.name)
    src_dir = dylib.parent
    for name in companions:
        src = src_dir / name
        if not src.exists():
            report.error(
                f"expected companion dylib {name} not found in {src_dir} — the release "
                "asset would ship the decoder without a dependency the podspec vendors."
            )
            return 1
        shutil.copy2(src, dest / name)
    _list_dir(dest)
    return 0


# --- Android ---------------------------------------------------------------
#
# android_build.yml:221-263. Two steps, both preserved as separate functions
# because their failure modes are genuinely different: the copy step is
# soft-fail (``|| true`` on both the ``find`` and the trailing ``ls``), and
# the completeness check is deliberately ASYMMETRIC -- required companions
# present, no undeclared-extra detection -- because ``libc++_shared.so``'s
# presence is legitimately conditional on the STL type
# (android_build.yml:241-251; ruled on separately, not this module's
# concern). Do not make this symmetric like linux/windows: that would fail a
# legitimate c++_static build for shipping one file fewer than a c++_shared
# build, which is not an incomplete group. ``source_dir`` is caller-supplied
# (ruling 2): ``${NATIVE_DIR}/build-android/android-arm64`` is workflow
# context, not a targets.py fact.


def stage_android(source_dir: str, artifact_dir: str) -> int:
    """Replaces android_build.yml:221-233. Never fails (matching the
    shell's ``|| true`` on both the copy and the listing) -- absence is
    caught downstream by ``assert_staged_group_android``, not here."""
    dest = Path(artifact_dir) / "native"
    dest.mkdir(parents=True, exist_ok=True)
    src_root = Path(source_dir)
    if src_root.is_dir():
        for src in sorted(src_root.glob("*.so")):
            try:
                shutil.copy2(src, dest / src.name)
            except OSError:
                pass
    _list_dir(dest)
    return 0


def assert_staged_group_android(artifact_dir: str) -> int:
    """Replaces android_build.yml:235-263. Two checks, in the shell's own
    order: (1) at least one ``libdng_decoder_native*.so`` exists; (2) every
    DECLARED COMPANION (not the decoder itself, and never checking for an
    undeclared extra) is present. Success marker is
    ``ANDROID_COMPANION_GROUP_COMPLETE=1`` -- a different token from linux's
    ``ATOMIC_GROUP_COMPLETE``, by design (ruling 3)."""
    dest = Path(artifact_dir) / "native"
    libs = sorted(dest.glob("libdng_decoder_native*.so")) if dest.is_dir() else []
    if not libs:
        report.error(
            f"No libdng_decoder_native*.so found in {dest}; Stage 2 build did not emit "
            "the expected artifact."
        )
        return 1
    expected = sorted(declared_companions("android"))
    report.plain("EXPECTED_SET (companions):")
    for name in expected:
        report.plain(name)
    missing = [name for name in expected if not (dest / name).exists()]
    if missing:
        report.error(
            f"missing required companion .so(s) in {dest}:" + "".join(f" {n}" for n in missing)
            + " — the Android atomic group (decoder + HEIF companions) is incomplete."
        )
        return 1
    report.marker("ANDROID_COMPANION_GROUP_COMPLETE", 1)
    return 0
