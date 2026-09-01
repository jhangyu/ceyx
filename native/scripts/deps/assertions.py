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
assertions.toml but has NO check function implemented here yet -- there is
no ``run(id, ...)`` dispatcher in this module at all. Do not assume one
exists; a caller wiring the remaining methods in must add both the check
function and a dispatcher, never present a not-yet-wired assertion as if
it ran.

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


# ---------------------------------------------------------------------------
# ELF-flavoured helpers (A-T2). These serve the android dists, whose artefacts
# are ELF (static archives of ELF objects) and whose instruments are the NDK's
# own llvm-nm / llvm-readelf.
#
# CAPTURE THEN GREP, always. Never `llvm-nm x.a | grep -q SYM`: under
# `set -o pipefail`, grep -q exits at the first match, nm dies of SIGPIPE and
# the pipeline reports 141 -- the check fails BECAUSE the symbol is present
# (2026-08-28 lesson). These helpers make that shape unavailable: the tool's
# stdout is written to a file, the file is read back, and the search runs in
# Python. `deps.run.run()` additionally refuses any argv element containing
# '|', so no pipeline can be constructed here at all.


def ndk_tool(ndk: Path | str, tool: str) -> Path:
    """Resolve an NDK-bundled LLVM binary (e.g. "llvm-nm", "llvm-readelf").

    The NDK ships exactly one prebuilt host directory per host OS, and its
    name is not derivable from ``platform.system()`` alone (darwin-x86_64 is
    used on Apple silicon too, via Rosetta or a universal binary), so the
    single existing directory is discovered rather than guessed.
    """
    ndk = Path(ndk)
    prebuilt = ndk / "toolchains" / "llvm" / "prebuilt"
    if not prebuilt.is_dir():
        raise AssertionFailed(
            f"NDK layout not recognised: {prebuilt} does not exist (is "
            f"{ndk} really an NDK root?)"
        )
    for host_dir in sorted(prebuilt.iterdir()):
        candidate = host_dir / "bin" / tool
        if candidate.is_file():
            return candidate
    raise AssertionFailed(
        f"{tool} not found under {prebuilt}/*/bin -- cannot run the ELF "
        f"assertions with this NDK"
    )


def capture_tool_output(argv: list, out_path: Path | str) -> Path:
    """Run ``argv``, write its stdout+stderr to ``out_path``, return the path.

    The artefact is written BEFORE anything reads it, so a failing assertion
    always leaves the evidence it judged on disk (and CI can upload it).
    """
    from . import run as run_mod  # local import: keeps this module import-light

    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    result = run_mod.run([str(a) for a in argv], check=False)
    out_path.write_text((result.stdout or "") + (result.stderr or ""), encoding="utf-8")
    if result.returncode != 0:
        raise AssertionFailed(
            f"{argv[0]} exited {result.returncode} (output captured to {out_path})"
        )
    return out_path


def assert_symbols_present(symbol_dump: Path | str, symbols: list, *, label: str) -> None:
    """Every name in ``symbols`` must appear in the captured symbol dump."""
    text = Path(symbol_dump).read_text(encoding="utf-8", errors="replace")
    missing = [s for s in symbols if s not in text]
    if missing:
        raise AssertionFailed(
            f"{label}: symbol(s) {missing} absent from {symbol_dump} -- the "
            f"capability they stand for is genuinely not in the archive"
        )


def assert_elf_machine(header_dump: Path | str, expected_machine: str, *, label: str) -> None:
    """The captured ELF header dump must report ``expected_machine`` (e.g.
    "AArch64"). Reads the file the tool wrote; performs no piping."""
    text = Path(header_dump).read_text(encoding="utf-8", errors="replace")
    if expected_machine not in text:
        raise AssertionFailed(
            f"{label}: {header_dump} does not report Machine {expected_machine!r} "
            f"-- the artefact was built for the wrong architecture"
        )


def assert_dir_non_empty(directory: Path | str, *, label: str) -> None:
    """A-LICENCE flavour: the directory exists and contains at least one file."""
    d = Path(directory)
    if not d.is_dir() or not any(p.is_file() for p in d.iterdir()):
        raise AssertionFailed(f"{label}: {d} is missing or contains no files")
