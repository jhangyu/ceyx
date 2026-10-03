#!/usr/bin/env python3
"""Platform-parity guard (PARITY.md, repo root).

Fails when a tracked in-scope file contains a platform conditional that the
registry does not list, when a listed file's conditional count changes, when a
listed file no longer has any conditional, when an entry carries an illegal
role, or -- once parity_registry.REQUIRE_CLOSED is True -- while any campaign
fork id is still open. Granularity is per file; the pinned count makes a new
conditional inside a registered file a visible registry diff.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from parity_registry import REGISTRY, REQUIRE_CLOSED  # noqa: E402
from run import run as _run  # noqa: E402

NATIVE_GUARD = re.compile(
    r"^\s*#\s*(?:if|elif|ifdef|ifndef)\b.*\b(?:__APPLE__|_WIN32|_WIN64|__ANDROID__"
    r"|__linux__|DNG_FORCE_VULKAN|TARGET_OS_[A-Z_]+)\b")
DART_GUARD = re.compile(
    r"Platform\.is(?:Windows|MacOS|Linux|Android|IOS|Fuchsia)\b"
    r"|defaultTargetPlatform|kIsWeb|dart\.library\.")
SCOPES = (
    ("native/src/", NATIVE_GUARD, (".c", ".cc", ".cpp", ".h", ".hpp", ".mm")),
    ("native/include/", NATIVE_GUARD, (".h", ".hpp")),
    ("plugin/lib/", DART_GUARD, (".dart",)),
)
LEGAL_ROLES = frozenset({"adapter", "accelerator", "parked", "fork-host"})


def _git_ls_files(root: Path, prefix: str) -> list[str]:
    result = _run(["git", "-C", str(root), "ls-files", "--", prefix])
    if result.returncode != 0:
        raise RuntimeError(f"git ls-files failed rc={result.returncode}: {result.stderr.strip()}")
    return [line for line in result.stdout.splitlines() if line]


def scan(root: Path, lister=None) -> dict[str, int]:
    lister = lister or _git_ls_files
    found: dict[str, int] = {}
    for prefix, pattern, suffixes in SCOPES:
        for rel in lister(root, prefix):
            if not rel.endswith(suffixes):
                continue
            text = (root / rel).read_text(encoding="utf-8", errors="replace")
            count = sum(1 for line in text.splitlines() if pattern.search(line))
            if count:
                found[rel] = count
    return found


def check(found: dict[str, int], registry: dict, require_closed: bool):
    problems: list[str] = []
    for rel, count in sorted(found.items()):
        entry = registry.get(rel)
        if entry is None:
            problems.append(f"UNREGISTERED platform guard: {rel} ({count} lines). "
                            "Register it per PARITY.md or remove the fork.")
            continue
        if entry["guard_lines"] != count:
            problems.append(f"GUARD COUNT CHANGED: {rel} registry={entry['guard_lines']} "
                            f"actual={count}. Re-review against PARITY.md; update the entry "
                            "in the same commit.")
    for rel, entry in sorted(registry.items()):
        if rel not in found:
            problems.append(f"STALE ENTRY: {rel} has no platform guard; delete its entry.")
        if entry.get("role") not in LEGAL_ROLES:
            problems.append(f"ILLEGAL ROLE: {rel} role={entry.get('role')!r}; "
                            f"must be one of {sorted(LEGAL_ROLES)}.")
    open_ids = sorted({fid for e in registry.values() for fid in e.get("open_forks", ())})
    if require_closed and open_ids:
        problems.append(f"OPEN FORKS remain: {','.join(open_ids)}")
    return problems, open_ids


def main() -> int:
    found = scan(REPO_ROOT)
    problems, open_ids = check(found, REGISTRY, REQUIRE_CLOSED)
    for problem in problems:
        print(f"parity-guard: FAIL {problem}")
    if problems:
        return 1
    print(f"parity-guard: OK files={len(found)} open_forks={','.join(open_ids) or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
