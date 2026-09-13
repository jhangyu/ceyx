"""Configure-log literal/pattern assertions (WI-19a).

Replaces three identically-shaped `grep -q PATTERN LOGFILE` steps that each
today assert a single build-log line is present, then print
`ASSERT <label> RC=<rc>` and, on a miss, an `::error::` and a nonzero exit:

  * android_build.yml:174-190 "Assert HEIF was actually linked, not
    silently degraded (A-T8-FIX)" -- pattern has a genuine `.*` wildcard.
  * android_build.yml:192-203 "Assert JXL was explicitly disabled, not
    silently degraded (G1)" -- literal substring, parens included.
  * windows_build.yml:415-425 "Assert JXL was statically linked, not
    silently degraded (G1)" -- literal substring.

One generic function, not a per-platform dispatch: every value that differs
between the three (log filename, pattern, label, error text) is call-specific
data, not a platform fact -- none of it belongs in `targets.py` (R5), and
`--platform` itself is deliberately NOT a parameter here (R2: a flag this
module would not consume is a promise it does not keep).

Matching uses `re.search(pattern, text)` against the whole log file's text,
not `grep -q`'s BRE. **CORRECTION (caught by this module's own tests, not
assumed true): unescaped `(`/`)`/`+` in Python `re` are NOT literal the way
they are in POSIX BRE without `-E`.** An earlier draft of this docstring
claimed unescaped parens "only add a capture group without changing what
matches" -- that is false: `(CEYX_ENABLE_JXL=OFF)` unescaped is a capture
group whose DELIMITING parens are consumed as syntax and are NOT required to
appear in the text at all, so it searches for the substring
`CEYX_ENABLE_JXL=OFF` with no parens around it, not
`(CEYX_ENABLE_JXL=OFF)`. All three real call sites contain characters that
are literal in grep's BRE but metacharacters in Python `re` (`(`, `)`, and
`+` in the HEIF version-separator " + "). **Every `--pattern` value passed
to this command must backslash-escape any of `. * + ? | ( ) [ ] { } \\` that
was meant literally in the original shell pattern** -- this module does
`re.search()` verbatim, it does not auto-escape. The correct, verified
argument strings for the three real call sites (each proven by a red/green
pair in test_configure_log.py) are:

    android HEIF:  HEIF: libheif .* \\+ libde265 .* \\(dynamic\\)
    android JXL:   JXL: disabled \\(CEYX_ENABLE_JXL=OFF\\)
    windows JXL:   JXL: static          (no metacharacters, no escaping needed)

No pipefail trap here: every real call site runs `grep -q PATTERN FILE`
against a file argument, never through a pipe, so there is nothing for a
`grep` match to SIGPIPE-kill (contrast `verify_artifact.py`/`dt_needed.py`,
which do de-pipeline `tool | grep` shapes for exactly that reason).
"""

from __future__ import annotations

import re
from pathlib import Path

from . import report


def assert_configure_log(
    log_path: str, pattern: str, label: str, error_message: str, marker: str | None = None
) -> int:
    """Reads `log_path`, searches it for `pattern` (a Python regex), prints
    `ASSERT <label> RC=<rc>`, and on a miss prints `error_message` via
    `report.error` and returns 1. Returns 0 on a match.

    `marker` (AC-2 remediation, push 8): optional, because most real call
    sites never had one -- `ASSERT <label> RC=<rc>` itself does NOT match
    `markerdiff.MARKER_RE` (it starts with the word "ASSERT ", not a bare
    `NAME=`/`RC=`), and that was ALREADY TRUE of every real call site's
    original pre-migration shell (`echo "ASSERT ... RC=${rc}"`) -- so most
    callers lost nothing when they migrated here, because there was nothing
    markerdiff-visible to lose. Exactly one real call site
    (macos_build.yml's LCMS2 configure-log assert) had a SECOND, separate
    bare-marker echo after its ASSERT line (`echo
    "LCMS_CONFIGURE_RC=${LCMS_RC}"`) that this module's collapse silently
    dropped -- `marker` restores that second line for callers that need it,
    via `report.marker()` (emits a conforming `NAME=value`).

    Emission order matches the pre-migration shell exactly: ASSERT line,
    then the error line if any, then the marker line LAST -- and
    unconditionally on both the pass and fail path (`if marker:` runs
    regardless of `rc`), which is what makes the failure path safe from the
    `errexit`-skips-a-trailing-echo trap that caused the original loss: a
    caller wrapping this in `set +e; ...; RC=$?; set -e; echo
    "MARKER=${RC}"` no longer needs that wrapper at all -- one call, with
    `--marker NAME`, does the whole thing."""
    text = Path(log_path).read_text(errors="replace")
    found = re.search(pattern, text) is not None
    rc = 0 if found else 1
    report.plain(f"ASSERT {label} RC={rc}")
    if rc != 0:
        report.error(error_message)
    if marker:
        report.marker(marker, rc)
    return rc
