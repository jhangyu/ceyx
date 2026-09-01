"""Pure argv renderer: (loaded manifest, component, platform, arch) -> argv list.

Plan D2 acceptance A2.1: `render()` performs NO I/O, reads no environment
variable, and calls no subprocess. It is a pure function of its arguments,
which is what lets a developer on macOS inspect and unit-test the exact
Windows configure command line (spec §8.1). Loading the manifest from disk is
`manifest.load()`'s job, not this module's -- callers (e.g. the future
`build_deps.py`, owned by D3) load once and pass the result in.
"""
from __future__ import annotations

import re
from pathlib import PurePosixPath, PureWindowsPath
from typing import Any

from . import manifest as manifest_mod

_ARCH_REF_RE = re.compile(r"\{arch\.([a-zA-Z0-9_]+)\}")
# android is a CROSS-COMPILE-ONLY platform: no host is Android, so
# build_deps.detect_platform() never resolves `auto` to it and every android
# render is an explicit caller decision. Its arch vocabulary is the Android
# ABI name ("arm64-v8a"), NOT the canonical host arch ("arm64") --
# arch_map.toml's [arm64-v8a] comment records why the two must not collapse:
# publish_release.py's asset naming consumes the ABI spelling verbatim.
_PLATFORMS = ("macos", "linux", "windows", "android")
_ANDROID_ARCHES = ("arm64-v8a",)

# CMAKE_*_FLAGS values are stored as a structured list in the manifest (spec
# §4.2 rule 2) and joined with a SPACE, because they render into a single
# `-D...=` argv element whose value is itself a space-separated flag string
# (e.g. `-DCMAKE_C_FLAGS=-DWIN32 -D_WINDOWS`). Every other list-valued key
# (e.g. CMAKE_IGNORE_PREFIX_PATH) is a CMake list and is joined with ';'.
_SPACE_JOINED_LIST_KEYS = frozenset({"CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS"})

# Fixed, symbolic install-prefix roots substituted for the manifest's
# "{dist}" token when a caller does not supply its own. These exist only so
# `render()` is total and its golden-file output is deterministic; a real
# build (`build_deps.py`, D3) passes its own absolute `dist` path.
DEFAULT_DIST = {
    "macos": "/Users/build/ceyx-dist",
    "linux": "/home/build/ceyx-dist",
    "windows": "C:/ceyx-dist",
    # android's default is the committed-dist convention itself rather than a
    # symbolic build root, because android dists are committed under
    # native/third_party/ with the ABI in the directory name (the naming
    # publish_release.py parses). "{component}" and "{arch.android_abi}" are
    # substituted by render() below; every other entry is a plain string, so
    # this is the only value that needs expansion.
    "android": "native/third_party/{component}-dist-android-{arch.android_abi}",
}

# Symbolic NDK root used when a caller renders android argv WITHOUT passing an
# explicit `ndk=` (inspection / golden output). A real build passes
# `--android-ndk` through to render(); the value is never read from the
# environment here -- render() stays pure (A2.1).
DEFAULT_NDK = "/opt/android-ndk"


class RenderError(ValueError):
    """Raised for a caller error (unknown component/platform/arch) or an
    arch_map substitution that cannot be resolved. `manifest.load()` already
    rejects any of these as a *schema* problem before render() ever runs
    (A1.4); this exception exists for callers that construct `loaded` by hand
    (e.g. tests) without going through `load()`."""


def render(
    loaded: dict[str, Any],
    component: str,
    platform: str,
    arch: str,
    dist: str | None = None,
    ndk: str | None = None,
) -> list[str]:
    """Return the exact `cmake -S ... -B ...` configure argv (the `-D...`
    portion plus a leading `-G <generator>` pair when the manifest declares
    one) for `component` on `platform`/`arch`. Pure: no subprocess, no
    environment reads, no filesystem access beyond the already-loaded
    `loaded` dict (see module docstring).

    `ndk` is the Android NDK root substituted for the manifest's "{ndk}" token
    (android overlays only). It is an explicit parameter rather than an
    ANDROID_NDK_HOME lookup precisely because this function reads no
    environment variable."""
    if platform not in _PLATFORMS:
        raise RenderError(f"unknown platform {platform!r} (expected one of {_PLATFORMS})")

    manifest = loaded["manifest"]
    arch_map = loaded["arch_map"]

    if platform == "android" and arch not in _ANDROID_ARCHES:
        raise RenderError(
            f"android builds use the Android ABI vocabulary; got arch {arch!r}, "
            f"expected one of {_ANDROID_ARCHES} (see native/deps/arch_map.toml "
            f"[arm64-v8a]) -- the ABI spelling is what publish_release.py parses "
            f"out of the dist/asset name, so 'arm64' must not be accepted here"
        )

    if arch not in arch_map:
        raise RenderError(f"unknown arch {arch!r} (expected one of {sorted(arch_map)})")

    comp = manifest.get("component", {}).get(component)
    if comp is None:
        raise RenderError(f"unknown component {component!r}")

    if dist is not None:
        dist_value = dist
    else:
        dist_value = DEFAULT_DIST[platform].replace("{component}", component)
        dist_value = _substitute(dist_value, arch_map, arch)

    ndk_value = ndk if ndk is not None else DEFAULT_NDK

    cmake = comp.get("cmake", {})
    base = cmake.get("base", {})
    overlay = cmake.get(platform, {})
    merged = manifest_mod.merge_platform_overlay(base, overlay)

    path_keys = set(comp.get("path_keys", []))
    path_list_keys = set(comp.get("path_list_keys", []))

    argv: list[str] = []
    generator = merged.pop("_generator", None)
    if generator is not None:
        argv += ["-G", _substitute(str(generator), arch_map, arch)]

    for key, raw_value in merged.items():
        if key.startswith("_"):
            continue  # reserved metadata key, not a -D flag

        if isinstance(raw_value, list):
            resolved_items = [_substitute(str(v), arch_map, arch) for v in raw_value]
            joiner = " " if key in _SPACE_JOINED_LIST_KEYS else ";"
            value = joiner.join(resolved_items)
        else:
            value = _substitute(str(raw_value), arch_map, arch)

        value = value.replace("{dist}", dist_value)
        # "{ndk}" only ever appears in an android overlay. It is substituted
        # from the explicit `ndk` argument (build_deps.py's --android-ndk),
        # never from ANDROID_NDK_HOME in the environment: render() is pure.
        value = value.replace("{ndk}", ndk_value)

        if key in path_keys:
            value = _to_platform_path(value, platform)
        elif key in path_list_keys:
            value = ";".join(_to_platform_path(v, platform) for v in value.split(";"))

        argv.append(f"-D{key}={value}")

    return argv


def _substitute(value: str, arch_map: dict[str, Any], arch: str) -> str:
    def repl(m: "re.Match[str]") -> str:
        field = m.group(1)
        entry = arch_map[arch]
        if field not in entry:
            raise RenderError(f"arch_map[{arch!r}] has no field {field!r}")
        resolved = entry[field]
        if isinstance(resolved, dict):
            raise RenderError(
                f"arch_map[{arch!r}].{field} is a nested table, not a scalar -- "
                f"not resolvable by render() without a platform key"
            )
        return str(resolved)

    return _ARCH_REF_RE.sub(repl, value)


def _to_platform_path(value: str, platform: str) -> str:
    """Resolve a manifest path value through the TARGET platform's own path
    type (never the host's) -- PureWindowsPath/PurePosixPath perform no
    filesystem access and read no cwd, so this stays inside A2.1's purity
    contract even when rendering Windows argv on a macOS host (spec §8.1,
    §8.2's drive-letter lint)."""
    if platform == "windows":
        return str(PureWindowsPath(value))
    return str(PurePosixPath(value))
