"""The mechanical instrument that makes AC-2 (output-parity) checkable
instead of a promise: a MULTISET diff of normalized marker lines between a
golden baseline and a candidate job log.

Frozen interface: docs/logs/2026-09-13/pyci-plan.md WI-3.

Why multiset, not set: `PROBE_CODECS_RC` legitimately prints TWICE per run
(a `tee -a` at `linux_build.yml:875` followed by a `cat` replay of the same
appended file at `:876`). A migration that prints it once must be a FAIL --
a set-based diff would treat one occurrence and two occurrences as "the same
marker is present" and pass it silently, which is exactly the failure mode
this module exists to prevent (plan C-G1).

Why normalization: a runner-assigned temp directory or workspace path
changing between two green runs of the SAME code is not a regression; the
digits that are part of a marker's actual VALUE (e.g. `EXPORTS_CHECKED=35`)
are not touched -- only whole path-shaped tokens are replaced.
"""

from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path

# Matches the marker line SHAPES this migration's callers ever print, per
# report.py's emitters: a GH Actions error/notice annotation, a `== title ==`
# section banner (but not a bare `===` divider -- `[^=]` excludes that), an
# `ALL_CAPS_NAME=` marker/rc line, or a bare `RC=` line.
MARKER_RE = re.compile(r"^(::error::|::notice::|==[^=].*==$|[A-Z][A-Z0-9_]*=|RC=)")

# A raw GH Actions job log line looks like (one or more tab-separated
# columns) + an ISO-8601 timestamp + a space + the actual text, e.g.:
#   Linux / x86_64 Vulkan\tUNKNOWN STEP\t2026-09-12T17:28:22.3888647Z PROBE_CODECS_RC=0
# `gh run view --log --job <id>` (single job already selected) omits the
# leading job-name column but keeps the same timestamp shape; this handles
# both by always taking the LAST tab-separated field before stripping the
# timestamp.
_TIMESTAMP_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+Z ?")

# Whole-token path shapes to normalize. Order matters: tempdir-shaped paths
# are classified before the generic workspace fallback so a temp file inside
# the checkout doesn't get miscounted as a workspace path.
_TEMPDIR_PATH_RE = re.compile(
    r"(?:/tmp/\S*"
    r"|/private/var/folders/\S*"
    r"|/var/folders/\S*"
    r"|[A-Za-z]:\\\\?[Uu]sers\\\\?[^\\]+\\\\?AppData\\\\?Local\\\\?Temp\S*"
    r"|[A-Za-z]:[\\/][Tt]emp\S*"
    r"|\$RUNNER_TEMP\S*)"
)
_WORKSPACE_PATH_RE = re.compile(
    r"(?:/[\w.\-]+(?:/[\w.\-]+)+"  # POSIX absolute path, 2+ components
    r"|[A-Za-z]:\\\\?(?:[\w.\-]+\\\\?)+)"  # Windows absolute path
)


def normalize(line: str) -> str:
    """Replaces tempdir-shaped absolute paths with ``<TMP>`` and other
    absolute paths with ``<WS>``; leaves everything else -- including
    digits that are part of a marker's own value -- untouched."""
    out = _TEMPDIR_PATH_RE.sub("<TMP>", line)
    out = _WORKSPACE_PATH_RE.sub("<WS>", out)
    return out


def extract(log_text: str) -> list[str]:
    """Returns the ordered list of normalized marker lines found in
    ``log_text``. Accepts either a raw multi-job GH Actions log (tab
    columns + timestamp) or an already-bare marker-per-line file (as
    produced by this module's own fixtures) -- both shapes pass through the
    same stripping logic without special-casing."""
    result = []
    for raw in log_text.splitlines():
        content = raw.split("\t")[-1]
        content = _TIMESTAMP_RE.sub("", content, count=1)
        content = content.lstrip("﻿")
        if MARKER_RE.match(content):
            result.append(normalize(content))
    return result


def counts(lines: list[str]) -> collections.Counter:
    return collections.Counter(lines)


def diff(baseline_text: str, candidate_text: str) -> tuple[int, list[str]]:
    """Multiset-diffs the marker lines extracted from ``baseline_text``
    against ``candidate_text``. Returns ``(rc, report_lines)`` where
    ``rc != 0`` iff any token's occurrence count differs. Each delta line is
    ``+N <token>`` (candidate has N more) or ``-N <token>`` (candidate has N
    fewer), sorted for deterministic output."""
    baseline_counts = counts(extract(baseline_text))
    candidate_counts = counts(extract(candidate_text))
    all_tokens = set(baseline_counts) | set(candidate_counts)
    deltas = []
    for token in sorted(all_tokens):
        delta = candidate_counts[token] - baseline_counts[token]
        if delta != 0:
            sign = "+" if delta > 0 else "-"
            deltas.append(f"{sign}{abs(delta)} {token}")
    rc = 1 if deltas else 0
    return rc, deltas


def main(argv=None) -> int:
    from . import report

    parser = argparse.ArgumentParser(prog="marker-diff")
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--leg", default="")
    args = parser.parse_args(argv)

    baseline_text = Path(args.baseline).read_text(encoding="utf-8")
    candidate_text = Path(args.candidate).read_text(encoding="utf-8")
    rc, deltas = diff(baseline_text, candidate_text)

    for line in deltas:
        report.plain(line)
    report.marker("MARKER_DIFF_RESULT", "FAIL" if rc else "PASS")
    report.marker("MARKER_DIFF_DELTAS", len(deltas))
    return rc


if __name__ == "__main__":
    sys.exit(main())
