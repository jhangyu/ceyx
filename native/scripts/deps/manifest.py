"""Load and validate native/deps/manifest.toml + native/deps/arch_map.toml.

Schema: docs/logs/2026-08-30/Spec_build_rewrite.md §4.2-4.4.
This module performs schema validation only -- it never builds anything and
never invokes a subprocess. `load()` is the only function here that performs
file I/O; `validate()` and everything else are pure functions of their
arguments so they can be exercised directly against synthetic in-memory
manifests in tests (Plan D1 acceptance A1.2-A1.6, "demonstrated red").
"""
from __future__ import annotations

import re
import tomllib
from pathlib import Path
from typing import Any

_REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_MANIFEST_PATH = _REPO_ROOT / "native" / "deps" / "manifest.toml"
DEFAULT_ARCH_MAP_PATH = _REPO_ROOT / "native" / "deps" / "arch_map.toml"

_KNOWN_PLATFORMS = ("default", "macos", "linux", "windows")
_ARCH_REF_RE = re.compile(r"\{arch\.([a-zA-Z0-9_]+)\}")

# R-1a's reason taxonomy (Spec §3.2): a self-built component's reason must
# name one of {registry absence, licence incompatibility, version
# unavailability, required patch} -- this is the mechanical (keyword-based)
# encoding of that check. It intentionally over-accepts English phrasing
# (e.g. both "licence" and "license") rather than under-accepting real
# reasons that happen to use different words; the trade-off matches A1.2's
# actual failure target, which is a MISSING or empty reason, not a
# stylistic variance in wording.
_VALID_SELF_BUILT_REASON_KEYWORDS = (
    "no vcpkg port",
    "no port exists",
    "not in vcpkg",
    "not in either registry",
    "absent from",
    "no registry",
    "licence",
    "license",
    "gpl",
    "downgrade",
    "below our",
    "older than",
    "version unavailable",
    "required patch",
    "patched",
    "patch set",
    "feature",
)


class ManifestError(ValueError):
    """Raised when a manifest (real or synthetic) fails schema validation."""


def _load_toml(path: Path) -> dict[str, Any]:
    with path.open("rb") as fh:
        return tomllib.load(fh)


def load(
    manifest_path: Path | str | None = None,
    arch_map_path: Path | str | None = None,
) -> dict[str, Any]:
    """Load + validate the manifest and arch map from disk. Raises
    ManifestError on any schema violation (A1.1: exits 0 / raises nothing on
    the real manifest.toml)."""
    m_path = Path(manifest_path) if manifest_path is not None else DEFAULT_MANIFEST_PATH
    a_path = Path(arch_map_path) if arch_map_path is not None else DEFAULT_ARCH_MAP_PATH
    manifest = _load_toml(m_path)
    arch_map = _load_toml(a_path)
    validate(manifest, arch_map)
    return {"manifest": manifest, "arch_map": arch_map}


def validate(manifest: dict[str, Any], arch_map: dict[str, Any]) -> None:
    """Validate `manifest` against arch_map. Pure function, no I/O. Raises
    ManifestError naming the first violation found."""
    components = manifest.get("component")
    if not components:
        raise ManifestError("manifest has no [component.*] tables")

    for name, comp in components.items():
        _validate_component(name, comp, arch_map)


def _validate_component(name: str, comp: dict[str, Any], arch_map: dict[str, Any]) -> None:
    source = comp.get("source")
    if not source or "default" not in source:
        raise ManifestError(f"component.{name}: missing source.default block")

    for platform_key, block in source.items():
        if platform_key not in _KNOWN_PLATFORMS:
            raise ManifestError(
                f"component.{name}.source.{platform_key}: unknown platform key "
                f"(expected one of {_KNOWN_PLATFORMS})"
            )
        _validate_source_block(name, platform_key, block)

    cmake = comp.get("cmake", {})
    base = cmake.get("base", {})
    for platform_key, table in cmake.items():
        if platform_key == "base":
            continue
        if platform_key not in ("macos", "linux", "windows"):
            raise ManifestError(
                f"component.{name}.cmake.{platform_key}: unknown platform overlay key"
            )
        # A1.6 / spec §4.2 rule 1: overlays are additive-only. A manifest
        # cannot literally "delete" a TOML key, so the deletion attempt is
        # expressed as the sentinel value "!delete" -- reject it here rather
        # than at render time.
        for key, value in table.items():
            if value == "!delete":
                raise ManifestError(
                    f"component.{name}: cmake.{platform_key} attempts to delete base "
                    f"key {key!r}; platform overlays are additive-only (spec §4.2 rule 1)"
                )

    # A1.4: a manifest referencing an arch_map vocabulary key that does not
    # exist anywhere in arch_map.toml raises at LOAD time, not at build time.
    all_tables = [base] + [t for k, t in cmake.items() if k != "base"]
    for table in all_tables:
        for value in table.values():
            for raw in value if isinstance(value, list) else [value]:
                if not isinstance(raw, str):
                    continue
                for field in _ARCH_REF_RE.findall(raw):
                    if not _arch_field_exists(arch_map, field):
                        raise ManifestError(
                            f"component.{name}: references undefined arch_map vocabulary "
                            f"key 'arch.{field}' (A1.4)"
                        )

    path_keys = set(comp.get("path_keys", []))
    all_declared_keys = set(base.keys())
    for k, t in cmake.items():
        if k != "base":
            all_declared_keys |= set(t.keys())
    for k in path_keys:
        if k not in all_declared_keys:
            raise ManifestError(
                f"component.{name}: path_keys references undeclared cmake key {k!r}"
            )


def _arch_field_exists(arch_map: dict[str, Any], field: str) -> bool:
    for arch_entry in arch_map.values():
        if isinstance(arch_entry, dict) and field in arch_entry:
            return True
    return False


def _validate_source_block(component: str, platform_key: str, block: dict[str, Any]) -> None:
    kind = block.get("kind")
    if not kind:
        raise ManifestError(f"component.{component}.source.{platform_key}: missing 'kind'")

    reason = block.get("reason")
    if not reason or not str(reason).strip():
        raise ManifestError(
            f"component.{component}.source.{platform_key}: missing 'reason' (A1.2)"
        )

    if kind == "registry":
        # A1.3 / spec N30: a floating registry version is a silent
        # neutrality breach -- a manifest that lets vcpkg resolve "latest"
        # must fail validation, not merely be discouraged by convention.
        if not block.get("version"):
            raise ManifestError(
                f"component.{component}.source.{platform_key}: kind=registry requires an "
                f"explicit 'version' pin (A1.3 / spec N30)"
            )
        return

    if kind in ("tarball", "git", "git-multi"):
        if block.get("override_only") or block.get("migration_pending"):
            # override_only: this block changes only the acquisition
            # mechanism (e.g. tarball -> git), not the registry-vs-self-built
            # decision, so R-1a's taxonomy does not apply to its reason.
            # migration_pending: this component's self-built status TODAY is
            # a sequencing fact (the vcpkg migration for it has not landed
            # yet), not a claimed-permanent R-1a reason; still requires a
            # non-empty reason (checked above), just not the taxonomy match.
            return
        reason_lc = str(reason).lower()
        if not any(kw in reason_lc for kw in _VALID_SELF_BUILT_REASON_KEYWORDS):
            raise ManifestError(
                f"component.{component}.source.{platform_key}: kind={kind!r} reason does "
                f"not name a registry absence, licence incompatibility, version "
                f"unavailability, or required patch (A1.2): {reason!r}"
            )
        return

    raise ManifestError(f"component.{component}.source.{platform_key}: unknown kind {kind!r}")


def merge_platform_overlay(base: dict[str, Any], overlay: dict[str, Any]) -> dict[str, Any]:
    """Additive merge used by render.py: overlay keys are added or override a
    base key; `validate()` has already rejected any `"!delete"` sentinel, so
    this function only needs to perform the merge itself."""
    merged = dict(base)
    merged.update(overlay)
    return merged
