"""Execution layer: turn a validated manifest entry into a real built dist.

Spec: docs/logs/2026-08-30/Spec_build_rewrite.md §4.1; Plan_build_rewrite.md
D3 ("execution layer"). This is the module that finally makes
``build_deps.py`` *build* something instead of only printing the argv it
would have used.

Division of labour, unchanged from the layers below it:

  - :mod:`deps.manifest` loads + schema-validates (no I/O beyond reading
    the two TOML files, no subprocess).
  - :mod:`deps.render` turns (manifest, component, platform, arch, dist)
    into the exact ``-D...`` argv. Pure.
  - :mod:`deps.fetch` downloads/clones and verifies SHA-256.
  - :mod:`deps.run` is the ONLY place ``subprocess`` is called from this
    package: argv lists, ``shell=False``, exit code read from
    ``CompletedProcess.returncode``.

This module composes those four. It adds no new subprocess mechanism of its
own -- every external tool it invokes (``cmake``) goes through
:func:`deps.run.run`, and archive extraction uses stdlib :mod:`tarfile`
rather than an external ``tar``, so there is no argv for a shell to
re-interpret (handoff §A's four Windows path-mangling triggers stay
structurally impossible).

What this module deliberately does NOT do: decide anything HEIF-specific.
The vcpkg copy-out of libde265/aom, the ``.pins`` stamp, the symbol/licence
assertions and the dist layout all live in :mod:`deps.heif`, because they
are properties of that particular stack rather than of "building a
manifest component".
"""
from __future__ import annotations

import os
import shutil
import tarfile
from pathlib import Path
from typing import Any, Optional, Sequence

try:  # pragma: no cover - import style depends on how the caller invokes us
    from . import fetch as fetch_mod
    from . import render as render_mod
    from .run import run
except ImportError:  # pragma: no cover - fallback for direct script execution
    import fetch as fetch_mod  # type: ignore[no-redef]
    import render as render_mod  # type: ignore[no-redef]
    from run import run  # type: ignore[no-redef]


class ExecuteError(RuntimeError):
    """Raised when acquisition, configure, build, install or the output
    check fails for a manifest component. Always names the component and
    the concrete thing that was missing -- never a bare "build failed"."""


def default_jobs() -> int:
    """Parallelism for ``cmake --build --parallel``.

    ``os.cpu_count()`` is the interpreter's own view of the machine, not a
    shelled-out ``nproc``/``sysctl -n hw.ncpu`` -- one code path for both
    platforms instead of the shell script's two-branch detection.
    """
    return os.cpu_count() or 1


def resolve_source(loaded: dict[str, Any], component: str, platform: str) -> dict[str, Any]:
    """Return ``source.default`` overlaid with ``source.<platform>`` if the
    manifest declares one.

    The overlay is additive in the same sense as the cmake overlay
    (``manifest.merge_platform_overlay``): a platform block that changes
    only the acquisition mechanism (``override_only``, e.g. kvazaar on
    Windows switching tarball -> git) still inherits the default block's
    other keys, so a caller reading ``version`` off the merged result gets
    the component-level pin either way.
    """
    comp = loaded["manifest"].get("component", {}).get(component)
    if comp is None:
        raise ExecuteError(f"unknown component {component!r} (not in manifest.toml)")
    source = comp.get("source", {})
    merged = dict(source.get("default", {}))
    merged.update(source.get(platform, {}))
    return merged


def component_version(loaded: dict[str, Any], component: str) -> str:
    comp = loaded["manifest"].get("component", {}).get(component)
    if comp is None:
        raise ExecuteError(f"unknown component {component!r} (not in manifest.toml)")
    version = comp.get("version")
    if not version:
        raise ExecuteError(f"component.{component}: no 'version' declared")
    return str(version)


def _substitute_version(value: str, version: str) -> str:
    return value.replace("{version}", version)


def acquire(
    loaded: dict[str, Any],
    component: str,
    platform: str,
    stage: Path,
) -> Path:
    """Fetch + unpack ``component``'s sources into ``stage``; return the
    extracted/cloned source directory.

    ``kind = "tarball"`` downloads to ``stage/<name>-<version>.tar.gz``,
    verifies the manifest's SHA-256 *before* extraction (an unverified
    tarball must never reach a build whose output we ship under an LGPL
    source-availability obligation), and extracts to
    ``stage/<name>-<version>``.

    ``kind = "git"`` clones the pinned tag. ``kind = "registry"`` raises:
    a registry component is not built here at all, and silently returning
    a nonexistent path would turn "this component comes from vcpkg" into
    a confusing configure failure three steps later.
    """
    block = resolve_source(loaded, component, platform)
    kind = block.get("kind")
    version = component_version(loaded, component)
    stage = Path(stage)

    # Checked BEFORE any directory is created: a registry component is not
    # built here at all, so this call must have no side effect on the tree.
    if kind == "registry":
        raise ExecuteError(
            f"component.{component}: source kind is 'registry' on {platform} -- it is "
            f"supplied by the package manager and is not built by this script. "
            f"Copying it into the dist is the caller's job (see deps/heif.py)."
        )

    stage.mkdir(parents=True, exist_ok=True)

    if kind == "tarball":
        url = _substitute_version(str(block["url"]), version)
        sha256 = str(block["sha256"])
        tarball = stage / f"{component}-{version}.tar.gz"
        src_dir = stage / f"{component}-{version}"
        if tarball.exists():
            # Re-verify rather than trusting a cached file: a truncated or
            # tampered leftover from an interrupted earlier run is exactly
            # the case a hash check exists for.
            try:
                fetch_mod.verify_sha256(tarball, sha256)
            except fetch_mod.FetchError:
                tarball.unlink(missing_ok=True)
        if not tarball.exists():
            fetch_mod.fetch_tarball(url, sha256, tarball)
        print(f"[deps] verified {tarball.name} {sha256}")
        if src_dir.exists():
            shutil.rmtree(src_dir)
        extract_tarball(tarball, stage)
        if not src_dir.is_dir():
            raise ExecuteError(
                f"component.{component}: {tarball.name} did not extract to "
                f"{src_dir} (unexpected archive layout)"
            )
        return src_dir

    if kind in ("git", "git-multi"):
        repo = str(block["repo"])
        tag = _substitute_version(str(block["tag"]), version)
        src_dir = stage / f"{component}-{version}"
        fetch_mod.fetch_git(repo, tag, src_dir)
        return src_dir

    raise ExecuteError(f"component.{component}: unknown source kind {kind!r}")


def extract_tarball(tarball: Path, dest: Path) -> None:
    """Extract ``tarball`` under ``dest`` using stdlib :mod:`tarfile`.

    ``filter="data"`` (PEP 706) rejects absolute/parent-escaping member
    paths and drops setuid bits. It exists on CI's Python 3.11 only from
    3.11.4 onward, hence the ``TypeError`` fallback -- the fallback is not
    a security downgrade we chose, it is the pre-PEP-706 behaviour of the
    interpreter we were handed, and the archives are SHA-256-pinned
    upstream releases rather than untrusted input.
    """
    with tarfile.open(tarball, "r:gz") as archive:
        try:
            archive.extractall(path=dest, filter="data")  # noqa: S202 - hash-pinned archives
        except TypeError:
            archive.extractall(path=dest)  # noqa: S202 - see docstring


def configure_build_install(
    src_dir: Path,
    build_dir: Path,
    cmake_argv: Sequence[str],
    *,
    jobs: Optional[int] = None,
    cmake: str = "cmake",
) -> None:
    """Run the three cmake phases as three separate argv-list invocations.

    Each phase's exit status comes from its own
    ``CompletedProcess.returncode`` via :func:`deps.run.run` (which raises
    on non-zero) -- never from a pipeline, never inferred from output text
    (lesson 2026-08-23).
    """
    jobs = jobs if jobs is not None else default_jobs()
    configure = [cmake, "-S", str(src_dir), "-B", str(build_dir), *cmake_argv]
    run(configure, capture_output=False)
    run([cmake, "--build", str(build_dir), "--parallel", str(jobs)], capture_output=False)
    run([cmake, "--install", str(build_dir)], capture_output=False)


def verify_outputs(loaded: dict[str, Any], component: str, dist: Path) -> list[Path]:
    """Check the manifest's ``[component.<name>.outputs]`` block against the
    installed tree. Returns the list of artefacts that were found.

    A ``required = true`` output whose every candidate is absent is a hard
    failure naming all candidates: "cmake --install exited 0" and "the
    artefact we ship exists" are different facts, and the shell script
    learned that the hard way for libde265's two possible spellings.
    """
    comp = loaded["manifest"].get("component", {}).get(component, {})
    outputs = comp.get("outputs", {})
    dist = Path(dist)
    found: list[Path] = []
    for name, spec in outputs.items():
        candidates = [dist / c for c in spec.get("candidates", [])]
        hit = next((c for c in candidates if c.exists()), None)
        if hit is None:
            if spec.get("required"):
                raise ExecuteError(
                    f"component.{component}: required output {name!r} is absent from "
                    f"{dist} -- looked for "
                    + ", ".join(str(c) for c in candidates)
                )
            continue
        found.append(hit)
    return found


def build_component(
    loaded: dict[str, Any],
    component: str,
    platform: str,
    arch: str,
    dist: Path,
    stage: Path,
    *,
    jobs: Optional[int] = None,
    extra_args: Optional[Sequence[str]] = None,
    ndk: Optional[str] = None,
) -> list[Path]:
    """Acquire, configure, build, install and verify one manifest component.

    ``extra_args`` is appended AFTER the rendered argv so a caller can add
    a value the manifest cannot know statically (e.g. an absolute
    ``-DCMAKE_PREFIX_PATH`` into a per-run vcpkg prefix). It never replaces
    a rendered flag: the manifest stays the single declaration of what the
    build options ARE.

    ``ndk`` is the Android NDK root forwarded to ``render()`` for the "{ndk}"
    token in android overlays (cross-compile-only platform); it is passed
    explicitly, never read from the environment.
    """
    dist = Path(dist)
    stage = Path(stage)
    src_dir = acquire(loaded, component, platform, stage)
    cmake_argv = list(
        render_mod.render(loaded, component, platform, arch, dist=str(dist), ndk=ndk)
    )
    if extra_args:
        cmake_argv += list(extra_args)
    build_dir = stage / f"build-{component}"
    print(f"[deps] configuring {component} {component_version(loaded, component)} ({platform}/{arch})")
    configure_build_install(src_dir, build_dir, cmake_argv, jobs=jobs)
    return verify_outputs(loaded, component, dist)
