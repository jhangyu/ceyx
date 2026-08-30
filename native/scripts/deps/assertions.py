"""Capability-assertion suite: schema loader + a first runnable check.

Spec: docs/logs/2026-08-30/Spec_build_rewrite.md §5 (D4). Plan_build_rewrite.md
D4 splits this deliverable into "written" (schema + declarations + at least
one runnable check, this round) and "signed" (every assertion executed
against a built dist with its red state demonstrated and recorded to
docs/logs/<date>/assertion_red_states.md, A4.3/A4.6 -- gated on a dist that
D2/D3 produce, deferred and tracked as an explicit outstanding item, never
silently skipped per A4.5's spirit).

This module owns:
  - ``load()``: reads native/deps/assertions.toml and schema-validates every
    record (A4.1: rejects any record missing measures/valid_on/why_valid/
    red_state).
  - ``assert_src_hash()``: the one assertion (A-SRC-HASH) that is fully
    runnable today without a built dist -- it only needs the downloaded
    source archive's bytes and the manifest's pin, both of which
    ``deps/fetch.py`` already produces. This is the round's "at least one
    runnable assertion, locally green" slice.

Everything else in the §5.3 table (A-CAP-*, A-PUBAPI, A-NO-GPL, A-DEPS,
A-LINK, A-ARCH, A-CRT, A-LICENCE, A-KVZ-SIMD) is declared in
assertions.toml but its check function is NOT implemented here yet --
calling ``run(id, ...)`` for one of those ids raises ``NotImplementedError``
naming it explicitly, rather than silently no-op'ing (never present a
not-yet-wired assertion as if it ran).

A4.2 (no assertion may branch on ``source.kind``): this module's functions
take already-resolved bytes/paths, never a manifest ``source`` block, so
there is structurally nothing to branch on; ``test_assertions.py`` also
greps this file's source for the literal string to keep that mechanical.
"""
from __future__ import annotations

import hashlib
import tomllib
from pathlib import Path
from typing import Any

_REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_ASSERTIONS_PATH = _REPO_ROOT / "native" / "deps" / "assertions.toml"

_REQUIRED_FIELDS = ("id", "tests", "method", "measures", "valid_on", "why_valid", "red_state")


class AssertionSchemaError(ValueError):
    """Raised when an assertion record (real or synthetic) fails schema
    validation (A4.1)."""


class AssertionFailed(AssertionError):
    """Raised by a check function (e.g. ``assert_src_hash``) when the
    capability it measures is genuinely absent -- this is the "red" outcome,
    distinct from ``AssertionSchemaError`` (a malformed declaration)."""


def load(path: Path | str | None = None) -> list[dict[str, Any]]:
    """Load + schema-validate every ``[[assertion]]`` record. Raises
    ``AssertionSchemaError`` naming the first violation found (A4.1)."""
    p = Path(path) if path is not None else DEFAULT_ASSERTIONS_PATH
    with p.open("rb") as fh:
        data = tomllib.load(fh)

    records = data.get("assertion")
    if not records:
        raise AssertionSchemaError(f"{p}: no [[assertion]] records found")

    for record in records:
        validate(record)
    return records


def validate(record: dict[str, Any]) -> None:
    """Pure function: raises ``AssertionSchemaError`` if ``record`` is
    missing any required field (A4.1's mechanical encoding of spec §5.2)."""
    record_id = record.get("id", "<missing id>")
    missing = [field for field in _REQUIRED_FIELDS if not record.get(field)]
    if missing:
        raise AssertionSchemaError(
            f"assertion {record_id!r}: missing required field(s) {missing}"
        )


def assert_src_hash(archive_bytes: bytes, expected_sha256: str) -> None:
    """A-SRC-HASH: the one fully runnable-today assertion (see module
    docstring). Raises ``AssertionFailed`` if the archive's digest does not
    match the manifest's pin; returns normally (green) otherwise. Pure: no
    I/O, no subprocess -- callers pass already-read bytes."""
    actual = hashlib.sha256(archive_bytes).hexdigest()
    expected = expected_sha256.lower()
    if actual != expected:
        raise AssertionFailed(
            f"A-SRC-HASH: sha256 mismatch (expected {expected}, got {actual})"
        )
