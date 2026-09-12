#!/usr/bin/env python3
"""Mechanically verifies `markerdiff.EXPECTED_ADDITIONS` against LOCAL
PRODUCERS -- the local half of the vigilance that used to rest entirely on
a leader reading the ledger by eye.

WHY THIS EXISTS: the ledger records the EXACT emitted line a push
deliberately introduced (e.g. `SHELL_ALLOWLIST_SIZE=109`), so AC-2's
zero-delta contract can tolerate a genuinely new marker without also
tolerating an unnoticed drift in its value. A ratchet that changes
109 -> 105 without updating its ledger entry makes AC-2 fail AFTER the
push, on an otherwise-correct change -- and it has already happened once
silently (a stale `=119` survived one ratchet, caught only because a
leader happened to read the file). This script turns "did anyone remember
to update the ledger" into a command with an exit code.

WHAT THIS DOES NOT DO: it does not run in CI, it prints no marker of its
own (a `NAME=value` line here would itself become a candidate line for
some FUTURE ledger to track, which is exactly the kind of self-reference
this campaign keeps finding defects in), and it does not touch
`markerdiff.py` -- that module is owned elsewhere; this script only
imports it.

ALGORITHM, per ledger entry:
  1. Extract the entry's marker KEY (the part before `=`).
  2. If the key is in `_BUILD_ARTIFACT_KEYS` (an explicit, named, commented
     allowlist of markers only ever emitted inside a real CI build job,
     never producible on a laptop without one) -- PRINT a named skip
     declaration and move on. This is never silent: an entry that reaches
     this branch without a comment justifying it is a code-review defect
     in this file, not a passing check.
  2. Otherwise, if the key is in `_KEY_TO_PRODUCER_SCRIPT`, run that
     script AS A BARE SCRIPT (a script "verified" only through imports has
     never actually been run -- push 2 lost a round to precisely that),
     normalize its emitted lines the same way `markerdiff.py` normalizes a
     real CI log, and check the ledger's EXACT line is among them. A
     script producing two ledger keys (as `check_shell_prohibition.py`
     does today) is only ever invoked once; its output is cached.
  3. A key that is neither classified as a build artifact NOR has a known
     local producer is a HARD FAIL, not a skip -- an unclassifiable entry
     means either this file's own classification tables are stale, or the
     ledger just grew an entry nobody taught this gate how to verify. A
     check that quietly does nothing for an input it does not recognise is
     the exact false-green shape this campaign keeps re-discovering.

Run with: python3 native/scripts/ci/check_expected_additions.py
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))

# Dual-mode import (see check_shell_prohibition.py's header comment for the
# full rationale this mirrors): a bare-script run has no parent package, so
# `from . import x` would raise; a package-member run (ci.py selftest /
# unittest discover) needs the relative form so mocks bind the identity the
# code under test actually imports.
if not __package__:
    import ci.markerdiff as markerdiff  # noqa: E402
    import ci.report as report  # noqa: E402
    import ci.run as run  # noqa: E402
else:
    from . import markerdiff  # noqa: E402
    from . import report  # noqa: E402
    from . import run  # noqa: E402

# Marker KEY -> the repo-root-relative bare-script path that locally
# produces it. Extend this table, never `markerdiff.py`, when a future push
# adds a new guard-emitted ledger entry.
_KEY_TO_PRODUCER_SCRIPT: dict[str, str] = {
    "SHELL_ALLOWLIST_SIZE": "native/scripts/ci/check_shell_prohibition.py",
    "SHELL_PROHIBITION_RESULT": "native/scripts/ci/check_shell_prohibition.py",
}

# Marker KEYs whose only producer is a real CI build job (not reproducible
# on a laptop without a full compile). Empty today -- every current ledger
# entry has a local producer. Add an entry here ONLY with a comment naming
# the actual CI step that emits it; an unnamed/uncommented addition is a
# review defect.
_BUILD_ARTIFACT_KEYS: frozenset[str] = frozenset()


def _marker_key(line: str) -> str:
    return line.split("=", 1)[0]


def _run_producer(script_relpath: str) -> list[str]:
    """Runs `script_relpath` as a bare script and returns its combined,
    normalized output lines -- the same normalization `markerdiff.py`
    applies to a real CI log, so a `<WS>`/`<TMP>`-shaped ledger line (none
    exist today, but the ledger's contract does not forbid one) compares
    correctly."""
    result = run.run([sys.executable, str(REPO_ROOT / script_relpath)])
    combined = result.stdout + result.stderr
    return [markerdiff.normalize(line) for line in combined.splitlines()]


def main() -> int:
    producer_cache: dict[str, list[str]] = {}
    stale: list[tuple] = []
    unclassified: list = []
    skipped = 0
    checked = 0

    for entry in markerdiff.EXPECTED_ADDITIONS:
        key = _marker_key(entry.line)

        if key in _BUILD_ARTIFACT_KEYS:
            print(f"SKIP (build-artifact, not locally producible): {entry.line} (source: {entry.source})")
            skipped += 1
            continue

        script = _KEY_TO_PRODUCER_SCRIPT.get(key)
        if script is None:
            unclassified.append(entry)
            continue

        if script not in producer_cache:
            producer_cache[script] = _run_producer(script)
        emitted = producer_cache[script]
        checked += 1
        if entry.line in emitted:
            continue
        observed = next((line for line in emitted if line.startswith(key + "=")), "<not emitted>")
        stale.append((entry, observed))

    for entry in unclassified:
        report.error(
            f"EXPECTED_ADDITIONS entry has no identifiable local producer and no build-artifact "
            f"classification: {entry.line!r} (source: {entry.source}) -- add a local producer to "
            "_KEY_TO_PRODUCER_SCRIPT or a named, commented build-artifact classification to "
            "_BUILD_ARTIFACT_KEYS in this file."
        )
    for entry, observed in stale:
        report.error(
            f"EXPECTED_ADDITIONS entry is stale: ledger says {entry.line!r} (source: {entry.source}) "
            f"but its local producer currently emits {observed!r} instead -- update the ledger "
            "entry in the same commit as whatever changed the emitted value."
        )

    if unclassified or stale:
        return 1

    plural = "y" if checked == 1 else "ies"
    print(f"expected_additions: OK ({checked} producible entr{plural} verified, {skipped} build-artifact skip(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
