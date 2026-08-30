"""Windows PE inspection: export table, import table, machine architecture.

Task #8 (R4): the inspection half of the Windows HEIF dist port, moved out of
``native/scripts/build_heif_dist_windows.sh`` lines 191-333 into the Python
carrier. Split from the build orchestration (``win_heif_dist.py``) because
every function here is either a pure string predicate or a single
``run.run()`` call, which makes the whole module unit-testable from macOS --
the host the Windows work is actually authored on (spec §8.1).

WHY THIS MODULE NEVER PIPES
---------------------------
The shell original had to write tool output to a FILE and then match it
separately, because under ``set -o pipefail`` a matcher that exits at its
first hit kills the still-writing producer with SIGPIPE (141), and the
pipeline is reported as failed *precisely because the symbol was found* --
an inverted verdict, and the contamination check (found = bad) would never
fire at all. Observed for real on 2026-08-28.

In this module that failure mode is structurally impossible rather than
merely avoided: ``run.run()`` rejects any argv element containing a pipe
character (run.py:120-124), so no pipeline can be constructed, and the
matching is done in Python against an already-complete string.

WHY AN ABSENT SYMBOL IS NOT AUTOMATICALLY A RED
-----------------------------------------------
Export-table capability checks are platform-dependent. On macOS/Linux every
non-static symbol is exported by default, so "symbol in the export table"
and "the feature was compiled in" are accidentally equivalent. On Windows
they are NOT: a PE exports nothing unless something explicitly says to.
A 2026-08-30 CI round burned two cycles "fixing" a perfectly good artefact
because a check of this shape went red for instrument reasons.

The assertion here is still valid for ``heif.dll`` specifically -- libheif
is built with ``WITH_REDUCED_VISIBILITY=ON`` and annotates its public API,
so its export table is genuinely populated and ``heif_decode_image`` genuinely
belongs in it. To keep that validity *checkable rather than assumed*,
:func:`assert_symbol_exported` reports the total export count alongside any
failure, so an empty/unreadable table (instrument error) is distinguishable
at a glance from a populated table that is missing the symbol (a real red).
Do not delete that count: it is the whole difference between the two
diagnoses.
"""
from __future__ import annotations

import re
from pathlib import Path
from typing import Optional, Sequence

try:  # pragma: no cover - import style depends on how the caller invokes us
    from .run import SubprocessError, run
except ImportError:  # pragma: no cover - fallback for direct script execution
    from run import SubprocessError, run  # type: ignore[no-redef]


class PeInspectionError(RuntimeError):
    """Raised when a PE file cannot be read with any available tool."""


class PeAssertionFailed(AssertionError):
    """Raised when a PE artefact genuinely lacks a required capability (or
    carries a forbidden one). Distinct from :class:`PeInspectionError`,
    which means the measurement itself did not happen."""


# Export ENTRY lines, matched structurally so that surrounding prose cannot be
# counted as symbols. A first attempt here counted "any identifier-ish token on
# any line", which scored dumpbin's own banner ("Microsoft", "Dump", "File",
# ...) as 10 exports for a DLL that exported nothing -- destroying the very
# empty-vs-populated distinction this count exists to draw. The unit test
# caught it; both formats are therefore anchored to their column layout:
#
#   dumpbin -exports :  "    3    2 0002B230 heif_decode_image"
#                        ordinal hint RVA      name
#   llvm-nm          :  "0002b230 T heif_decode_image"
_DUMPBIN_EXPORT_RE = re.compile(r"^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{6,}\s+(\S+)\s*$")
_LLVM_NM_EXPORT_RE = re.compile(r"^[0-9A-Fa-f]+\s+[A-Za-z]\s+(\S+)\s*$")


def _read_pe_text(dll: Path, out_path: Path, primary: Sequence[str], fallback: Sequence[str], what: str) -> str:
    """Run ``primary`` (dumpbin, present on a real MSVC runner); if it cannot
    run at all, fall back to ``fallback`` (the LLVM tool, always present
    alongside clang-cl). Output is written to ``out_path`` and returned as a
    string -- never streamed into a matcher (see module docstring).

    The exit status is read from ``CompletedProcess.returncode`` via
    ``run.run(check=False)``, never inferred from the output text.
    """
    out_path.parent.mkdir(parents=True, exist_ok=True)
    attempts: list[str] = []

    for argv in (list(primary), list(fallback)):
        try:
            result = run(argv + [str(dll)], check=False)
        except (OSError, SubprocessError) as exc:
            attempts.append(f"{argv[0]}: not runnable ({exc})")
            continue
        text = (result.stdout or "") + (result.stderr or "")
        if result.returncode == 0:
            out_path.write_text(text, encoding="utf-8")
            return text
        attempts.append(f"{argv[0]}: exit {result.returncode}")

    raise PeInspectionError(
        f"could not read {what} of {dll} with either tool "
        f"({'; '.join(attempts)}) -- the measurement did NOT happen; "
        "this is an instrument failure, not a capability failure"
    )


def read_exports(dll: Path, out_path: Path) -> str:
    """Return ``dll``'s export table as text (dumpbin, else llvm-nm)."""
    return _read_pe_text(
        Path(dll),
        Path(out_path),
        ["dumpbin", "-exports"],
        ["llvm-nm", "--extern-only", "--defined-only"],
        "the export table",
    )


def read_dependents(dll: Path, out_path: Path) -> str:
    """Return ``dll``'s import table as text (dumpbin, else llvm-objdump)."""
    return _read_pe_text(
        Path(dll),
        Path(out_path),
        ["dumpbin", "-dependents"],
        ["llvm-objdump", "-p"],
        "the import table",
    )


def parse_exported_symbols(exports_text: str) -> list[str]:
    """Return the export-entry names parsed out of ``exports_text``.

    Structural, not token-scraping: only lines matching either tool's export
    column layout contribute, so banner prose contributes nothing.
    """
    names: list[str] = []
    for line in exports_text.splitlines():
        match = _DUMPBIN_EXPORT_RE.match(line) or _LLVM_NM_EXPORT_RE.match(line)
        if match:
            names.append(match.group(1))
    return names


def count_exported_symbols(exports_text: str) -> int:
    """Diagnostic only: how many entries the export table holds.

    Used solely to tell an EMPTY export table (instrument error / nothing was
    exported at all) apart from a POPULATED one that lacks a specific symbol
    (a real capability red). Never a pass/fail criterion by itself -- a
    threshold here would be a made-up number.
    """
    return len(parse_exported_symbols(exports_text))


def symbol_present(text: str, symbol: str) -> bool:
    """Whole-word match for ``symbol`` in ``text``.

    Whole-word, mirroring the shell original's word-boundary matching:
    a substring match would let ``heif_decode_image_handle`` satisfy a check
    for ``heif_decode_image``.
    """
    return re.search(rf"\b{re.escape(symbol)}\b", text) is not None


def token_present_ci(text: str, token: str) -> bool:
    """Case-insensitive substring match, for import-table library names and
    contamination scans where the exact decoration is not known in advance
    (``de265`` matching ``libde265.dll``, ``x265`` matching any spelling)."""
    return token.lower() in text.lower()


def assert_symbol_exported(exports_text: str, symbol: str, *, dll_name: str) -> None:
    """Assert ``symbol`` is in ``dll_name``'s export table.

    On failure the message carries the export count so an instrument problem
    (count 0 -- nothing exported, so the check proves nothing) is not misread
    as a capability problem (count high, symbol truly missing). See the module
    docstring's platform-dependence note.
    """
    if symbol_present(exports_text, symbol):
        return
    total = count_exported_symbols(exports_text)
    if total == 0:
        raise PeAssertionFailed(
            f"{dll_name} exports NOTHING readable ({symbol!r} not found, and the "
            "export table parsed to 0 symbols). Treat this as an INSTRUMENT "
            "failure first: a PE exports nothing by default, so verify the "
            "table was actually read before concluding the capability is absent."
        )
    raise PeAssertionFailed(
        f"{dll_name} does not export {symbol!r}, although its export table "
        f"holds {total} other symbols -- the table WAS read, so this is a "
        "genuine capability failure, not an instrument failure."
    )


def assert_depends_on(deps_text: str, token: str, *, dll_name: str, consequence: str) -> None:
    """Assert ``dll_name``'s import table references ``token``."""
    if token_present_ci(deps_text, token):
        return
    raise PeAssertionFailed(
        f"{dll_name} has no {token} dependency -- {consequence}\n"
        f"import table as read:\n{deps_text}"
    )


def assert_absent(text: str, token: str, *, where: str, why: str) -> None:
    """Assert ``token`` does NOT appear in ``text`` (contamination scan).

    In the shell original this direction was the more dangerous one: the
    SIGPIPE inversion meant the contamination check could never fire at all,
    so it read green forever. Here it is a plain string test over complete
    text and fires on presence.
    """
    if token_present_ci(text, token):
        raise PeAssertionFailed(f"{token} found in {where}: {why}")


def resolve_existing(root: Path, candidates: Sequence[str], *, what: str) -> str:
    """Return the first of ``candidates`` (relative to ``root``) that actually
    exists on disk, as a relative POSIX-ish string.

    Resolution is by PRESENCE, never by guess: the installed spelling differs
    by toolchain and generator (``de265.lib`` vs ``libde265.lib``,
    ``bin/libde265.dll`` vs ``bin/de265.dll``), and guessing wrong yields a
    silent ``LIBDE265_FOUND=false`` rather than an error (source script
    lines 128-143, 196-213).
    """
    root = Path(root)
    for candidate in candidates:
        if (root / candidate).is_file():
            return candidate
    listing = sorted(str(p.relative_to(root)) for p in root.rglob("*") if p.is_file())
    raise PeInspectionError(
        f"no {what} found under {root} (tried: {', '.join(candidates)}).\n"
        "complete listing of what WAS installed:\n" + "\n".join(listing)
    )


def assert_machine_x86_64(file_output: Optional[str], *, dll_names: str) -> bool:
    """Architecture proof, best-effort, mirroring the source script's use of
    ``file``. Returns True when the check actually ran and passed, False when
    the tool was unavailable.

    A False return must be reported as SKIPPED, never as passed -- the source
    script is explicit about this ("NOTICE: ... architecture check SKIPPED
    (not passed)"), and a silently-skipped check that looks identical to a
    green one is exactly the failure this project has recorded before.
    """
    if file_output is None:
        return False
    if "x86-64" in file_output or "x86_64" in file_output:
        return True
    raise PeAssertionFailed(f"{dll_names} are not x86-64:\n{file_output}")
