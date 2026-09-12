"""Resolve external CLI tools (strings, readelf, nm, objdump, dumpbin, cc)
per platform, with a LOUD not-found that names the full ordered search list.

Frozen interface: docs/logs/2026-09-13/pyci-plan.md WI-3.

Why this module exists: the round-5 CI red was a bare `clang` command word in
a shell step body that silently failed on a container image that only had
GCC -- the failure message named nothing a human could act on. Every lookup
here either returns a real, `shutil.which`-resolved path, or raises
`ToolNotFound` naming every candidate name it tried, in the declared
preference order. No fallback to "the first thing that looks close enough".

Per-platform preference order is DATA that lives in `targets.py` (this
module states no per-platform fact of its own, mirroring Halcyon's
targets/tools split) -- `tools.py` only resolves; it never probes a
candidate by executing it (`shutil.which` only).
"""

from __future__ import annotations

import shutil

from . import targets

# Maps a tool "kind" (as used by callers, e.g. codec_probe.py, orientation.py)
# to the key under which its ordered preference tuple (or single string, for
# `cc`) lives in a targets.py platform spec dict.
_KIND_TO_SPEC_KEY = {
    "strings": "strings_tools",
    "readelf": "readelf_tools",
    "nm": "nm_tools",
    "objdump": "objdump_tools",
    "dumpbin": "dumpbin_tools",
    "cc": "c_compiler",
}


class ToolNotFound(Exception):
    """Raised when no candidate in a kind's ordered search list resolves via
    ``shutil.which``. The message always names every candidate that was
    tried, in order -- never a bare "not found"."""


def _search_order(kind: str, platform: str) -> tuple[str, ...]:
    if kind not in _KIND_TO_SPEC_KEY:
        raise ToolNotFound(
            f"unknown tool kind {kind!r}; known kinds (in no particular "
            f"order): {', '.join(sorted(_KIND_TO_SPEC_KEY))}"
        )
    key = _KIND_TO_SPEC_KEY[kind]
    spec = targets.spec(platform)
    value = spec.get(key)
    if not value:
        raise ToolNotFound(
            f"platform {platform!r} has no {key!r} configured in targets.py "
            f"(kind={kind!r}); nothing to search for"
        )
    if isinstance(value, str):
        return (value,)
    return tuple(value)


def resolve(kind: str, platform: str) -> str:
    """Returns the resolved absolute path of the first candidate for
    ``kind`` on ``platform`` that ``shutil.which`` finds. Raises
    ``ToolNotFound`` naming the FULL ordered search list -- not just the
    first candidate -- when none resolve."""
    order = _search_order(kind, platform)
    for name in order:
        found = shutil.which(name)
        if found:
            return found
    raise ToolNotFound(
        f"no {kind!r} tool found for platform {platform!r}; searched (in "
        f"order): {', '.join(order)}"
    )


def resolve_or_report(kind: str, platform: str, error_text: str) -> str | None:
    """Like ``resolve``, but on failure reports ``error_text`` via
    ``report.error`` and returns ``None`` instead of raising -- for call
    sites that want to accumulate failures and exit once, rather than
    unwinding on the first missing tool."""
    from . import report

    try:
        return resolve(kind, platform)
    except ToolNotFound:
        report.error(error_text)
        return None
