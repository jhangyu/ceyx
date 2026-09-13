#!/usr/bin/env python3
"""Guard (a): the test suite's own output must never contain a line the
AC-2 instrument (`markerdiff.py`) would read as a CI marker.

PROPERTY GUARDED: `native/scripts/ci.py selftest` runs every unit test
IN-PROCESS, under `unittest`. Several of those tests exercise
`report.marker()`/`report.rc()`/`report.bare_rc()` call sites (e.g.
`dist_build.dist_build()`) through a mock -- the intent is that the
call site's real stdout write is redirected into an in-memory buffer by a
per-test capture helper (`_run_captured()` in the relevant test file) so
the assertion can inspect it without the line ever reaching the process's
real stdout. A test that invokes the captured function WITHOUT going
through that helper lets the marker line leak onto real stdout -- which,
inside a CI job, is the job log AC-2's `markerdiff.extract()` parses.
`MARKER_RE` cannot tell "a genuine workflow marker" from "a leaked test
artifact" apart; both are `NAME=value` shaped. A leak therefore either
manufactures a phantom marker AC-2 was never told to expect (immediate
`MARKER_DIFF_DELTAS` failure) or, worse, silently matches an existing
ledger key by coincidence and corrupts that key's count.

RED CASE (recorded, and independently reproduced live -- see
`tmp/verify/impl-a-marker-leak/wi50-red-green-evidence.txt` for the
reproduction transcript at both the named commit and the actual
defect-introducing commit): commit `81ee2889` introduced
`test_android_ndk_appended_only_when_given` in `test_dist_build.py`,
calling `dist_build.dist_build(..., rc_marker="T", ...)` directly instead
of through `_run_captured()`. `report.marker("T", 0)` printed `T=0`
straight to real stdout, reaching `ci.py selftest`'s output and, from
there, the push-8b round-1 CI job log -- `MARKER_DIFF_DELTAS=1`, `FAIL` on
the `nativetests` leg (campaign handover Sec.9, push-8b row). Commit
`a7840ef6` fixed the call site; this guard exists so no future call site
can regress it silently.

MECHANISM (revised after lead review -- see collision note below): runs
the SAME `unittest.TestLoader().discover()` invocation `ci.py`'s own
`_selftest()` uses, IN-PROCESS, wrapped in a single OUTER
`redirect_stdout`/`redirect_stderr`. This still catches the historical
leak: `print()` re-reads `sys.stdout` on every call rather than caching a
reference at import time, so a call site that bypasses its OWN test
file's per-test capture helper still lands inside THIS module's outer
capture instead of the terminal -- the leak signature (a marker line
appearing somewhere it should have been absorbed by an inner helper and
wasn't) is exactly as visible in-process as it would be via a subprocess.
An earlier revision of this module ran `ci.py selftest` as a real
subprocess on the (incorrect) theory that in-process capture could mask
the leak; `check_no_test_execution_in_ci.py`'s AST scan of
`native/scripts/ci/**.py` for subprocess-launch sites collided with that
mechanism (this file's own `subprocess.run()` call, argv[0] a variable,
reported `UNRESOLVED`) -- switching to in-process capture dissolves the
collision instead of requiring a declared exemption, and is strictly
simpler (no child-process spawn, no argv/cwd plumbing to get wrong).

SCOPE (parking-lot ORDERED-1, widened from AC-2's own comparison): this
checks for ANY line selftest's combined stdout+stderr emits that matches
the two VALUE-shaped alternatives of `markerdiff.MARKER_RE`
(`[A-Z][A-Z0-9_]*=` or bare `RC=`) -- not only keys AC-2 happens to
compare today. A leak of a marker key nobody put in the ledger is the
case that matters most, precisely because AC-2's own delta-diff would
never even notice a BRAND NEW key was introduced by a leak (it would
just look like a real new marker to ledger against, per
`check_expected_additions.py`'s job) -- catching it here, structurally,
is cheaper than teaching every future ledger entry to prove it isn't a
leak.

DELIBERATELY NOT FLAGGED: `::error::`/`::notice::`/`== title ==` lines.
Several existing tests intentionally exercise error-reporting paths
(`report.error(...)`) via `assertRaises`/direct call inside their OWN
`_run_captured`-equivalent redirect, and those are a normal, expected,
already-tolerated part of `ci.py selftest`'s output (see `a7840ef6`'s own
commit message: "15 pre-existing `::error::` from unrelated parked
tests"). Value-shaped markers (`NAME=value`, `RC=value`) are categorically
different: nothing about running `unittest` in-process should ever
require printing a bare `NAME=value`/`RC=value` line to real stdout --
production code paths only print those when a real workflow step runs
`ci.py <command>`, never when `ci.py selftest` merely imports and calls
functions under mocks. Their presence at all is the leak signal; no
threshold or baseline is needed.

Frozen interface depended on: `native/scripts/ci/markerdiff.py`'s
`MARKER_RE` (imported, not re-implemented, so the two files can never
silently drift out of sync on what counts as a marker shape).

Exit codes: 0 = no leaked value-marker lines found. 1 = at least one
leaked line found (printed, one per line, with its 1-based line number in
the captured output). 2 = the tests directory this module discovers
against could not be found (environment problem, distinct from a real
leak).
"""

from __future__ import annotations

import io
import re
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

# native/scripts/ci/check_test_marker_leak.py -> native/scripts/ci ->
# native/scripts -> native -> repo root.
REPO_ROOT = Path(__file__).resolve().parents[3]
_NATIVE_SCRIPTS_DIR = REPO_ROOT / "native" / "scripts"
_TESTS_DIR = _NATIVE_SCRIPTS_DIR / "ci" / "tests"

# The two VALUE-shaped alternatives of markerdiff.MARKER_RE, isolated:
# an ALL_CAPS marker/rc assignment, or a bare `RC=` line. Deliberately
# excludes the annotation/banner alternatives (`::error::`, `::notice::`,
# `== title ==`) -- see module docstring, "DELIBERATELY NOT FLAGGED".
_VALUE_MARKER_RE = re.compile(r"^([A-Z][A-Z0-9_]*=|RC=)")


def find_leaked_markers(text: str) -> list[tuple[int, str]]:
    """Returns (1-based line number, line text) for every line in `text`
    that matches the value-marker shape."""
    leaks = []
    for line_no, line in enumerate(text.splitlines(), start=1):
        if _VALUE_MARKER_RE.match(line):
            leaks.append((line_no, line))
    return leaks


def run_selftest() -> tuple[int, str]:
    """Runs the identical `unittest.TestLoader().discover()` invocation
    `ci.py`'s own `_selftest()` uses, IN-PROCESS, under one outer
    `redirect_stdout`/`redirect_stderr` pair. Returns (0-if-successful,
    combined stdout+stderr text) -- `1` mirrors `ci.py selftest`'s own
    convention of "0 if wasSuccessful() else 1"."""
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        loader = unittest.TestLoader()
        suite = loader.discover(str(_TESTS_DIR), top_level_dir=str(_NATIVE_SCRIPTS_DIR))
        result = unittest.TextTestRunner(verbosity=0).run(suite)
        print(
            f"CI-SELFTEST: tests={result.testsRun} "
            f"failures={len(result.failures)} errors={len(result.errors)}"
        )
    rc = 0 if result.wasSuccessful() else 1
    return rc, out.getvalue() + err.getvalue()


def main(argv=None) -> int:
    if not _TESTS_DIR.is_dir():
        print(f"[check_test_marker_leak] FAIL: {_TESTS_DIR} not found")
        return 2

    selftest_rc, output = run_selftest()
    leaks = find_leaked_markers(output)

    print(
        f"[check_test_marker_leak] selftest_rc={selftest_rc} "
        f"output_lines={len(output.splitlines())} leaked_markers={len(leaks)}"
    )
    if leaks:
        print("[check_test_marker_leak] FAIL -- value-marker line(s) leaked "
              "onto ci.py selftest's real stdout (a test bypassed its "
              "file's capture helper):")
        for line_no, line in leaks:
            print(f"  line {line_no}: {line}")
        return 1

    print("[check_test_marker_leak] PASS -- zero value-marker lines in "
          "selftest output")
    return 0


if __name__ == "__main__":
    sys.exit(main())
