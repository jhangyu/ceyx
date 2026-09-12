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

# OBSERVABILITY markers: named, printed allowlist. These are markers whose
# VALUE can legitimately vary across a green run for reasons outside this
# repository (e.g. two-toolset drift in a linker's byte-for-byte output) --
# presence and occurrence COUNT are still asserted strictly; only the value
# is tolerated. Everything not in this set is an ASSERTION marker: full line
# (value included) is compared, no tolerance.
#
# Growing this list is a reviewable two-line diff with a reason string --
# same discipline as allowlist.py's MUST_STAY set (WI-4). No marker may ever
# be silently classified.
#
# DLL_SIZE_BYTES: push-1 AC-2 windows capture showed
#   -1 DLL_SIZE_BYTES=10035712 / +1 DLL_SIZE_BYTES=10049536
# while `git diff d33cc607..ef141b3a` touched zero CMake/C++/vcpkg/workflow
# files the Windows build reads, and EXPORTS_RESULT/EXPORTS_CHECKED/
# PROBE_CODECS_RC on the same run were unchanged -- the exported surface and
# codec capability are identical; only a linked DLL's byte count moved,
# consistent with MSVC toolset drift on the runner image. AC-2 as originally
# specified could never read all-green because of this marker.
#
# Rejected alternatives (do not re-propose):
#   - a tolerance band (e.g. +/-1%): arbitrary threshold, masks small real
#     regressions, invites endless argument about the number.
#   - per-run re-baselining: copying the candidate over the baseline to make
#     the diff pass, which is exactly the evidence-destroying move the test
#     plan forbids -- it would erase the baseline's value as a fixed point.
OBSERVABILITY_MARKERS: frozenset[str] = frozenset({"DLL_SIZE_BYTES"})

# EXPECTED_ADDITIONS ledger: a named, printed record of markers that a
# SPECIFIC push deliberately introduced (a new guard printing a new marker
# for the first time). AC-2's "zero deltas" contract would otherwise fail
# every push that adds a guard, for succeeding at its own job -- push 2
# introduced SHELL_ALLOWLIST_SIZE and SHELL_PROHIBITION_RESULT on the
# `nativetests` leg and neither exists in the r7/d33cc607 baseline, so both
# show up as `+1` with no corresponding baseline line.
#
# This is NOT the same relief as OBSERVABILITY_MARKERS: an observability
# entry tolerates the VALUE varying forever (nothing in this repo controls
# it). Here the repository controls the value completely -- each entry
# records the EXACT full normalized line a push introduced, and only that
# exact line is treated as an expected addition. A different value for the
# same key (e.g. a wrong/unratcheted SHELL_ALLOWLIST_SIZE) is NOT on the
# ledger and therefore still FAILS as an ordinary unlisted addition -- this
# is what keeps SHELL_ALLOWLIST_SIZE an ASSERTION marker with a deliberately
# updated expected value, not an observability marker: the allowlist ratchet
# is the entire point of the shell-prohibition guard (WI-4), and normalising
# its count away would let ten entries be added back unnoticed.
#
# Each ratchet updates its own ledger entry's `line` in the same commit that
# shrinks the allowlist, in principle -- coordinated through the leader per
# push, same discipline as the allowlist itself. In practice this entry's
# update has landed as its own follow-up commit both times so far, not in
# the same commit as the allowlist edit itself:
#   - WI-9 (push 3, 09756d35): 119 -> 113, ledger updated one commit later
#     after a leadership-rotation handoff gap.
#   - WI-12 (push 4, 2c491e9a): 113 -> 109 (four orientation MUST_STAY
#     entries retired as their steps became one-line Python calls),
#     ledger updated in a scheduled follow-up commit per the leader's
#     explicit sequencing (impl-6 -> impl-7's ratchet -> this entry ->
#     freeze), so no live mismatch window existed this time.
#   - WI-15 (push 5, 6f3880ba): 109 -> 106 (three codec-probe MUST_STAY
#     entries retired -- linux/macos/windows CI-T3/D4-R-7 -- as their
#     steps became one-line `codec-probe` calls), ledger updated in this
#     follow-up commit per the leader's sequencing (impl-7's linux YAML ->
#     impl-9's macOS+windows YAML + ratchet -> this entry). Value taken
#     from `check_shell_prohibition.py`'s own live emission, not from
#     impl-9's projection or the leader's report of it.
# All commits for a given push stay unpushed until the leader freezes and
# adjudicates, so the campaign-visible (pushed) state is always consistent
# -- noted here so the in-tree commit gap is not read as if the
# same-commit invariant held.
#
# A listed marker that later disappears is NOT specially exempted: a
# negative delta (`-N`) is ALWAYS a hard FAIL, for every marker, with no
# exception -- that is the one invariant AC-2 exists to hold, and this
# ledger only ever suppresses a `+N` match against its own EXACT recorded
# line, never a `-N`.
#
# Rejected alternatives (do not re-propose):
#   - re-baselining from the new green run: discards the d33cc607 anchor,
#     which is the only thing that makes "nothing pre-existing disappeared"
#     provable; the test plan forbids normalising a diff away this way.
#   - a blanket `+N` tolerance for any new marker: would hide a duplicated
#     `print`/`tee` defect (the exact shape C-G1 exists to catch) behind the
#     same relief meant for a single, named, reviewed addition.
class _ExpectedAddition:
    __slots__ = ("leg", "line", "source")

    def __init__(self, leg: str, line: str, source: str) -> None:
        self.leg = leg
        self.line = line
        self.source = source


EXPECTED_ADDITIONS: tuple[_ExpectedAddition, ...] = (
    _ExpectedAddition("nativetests", "SHELL_ALLOWLIST_SIZE=106", "push 5 / WI-15 (6f3880ba)"),
    _ExpectedAddition("nativetests", "SHELL_PROHIBITION_RESULT=PASS", "push 2 / b88c41a4"),
)

_KEY_RE = re.compile(r"^([A-Z][A-Z0-9_]*)=")


def _marker_key(line: str) -> str | None:
    """Returns the NAME in a normalized `NAME=value` marker line, or None
    for lines with no such key (e.g. `::error::...`, `== title ==`)."""
    m = _KEY_RE.match(line)
    return m.group(1) if m else None


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


def diff(baseline_text: str, candidate_text: str, leg: str | None = None) -> tuple[int, list[str]]:
    """Multiset-diffs the marker lines extracted from ``baseline_text``
    against ``candidate_text``. Returns ``(rc, report_lines)`` where
    ``rc != 0`` iff any token's occurrence count differs.

    Two comparison classes (see OBSERVABILITY_MARKERS above):
    - ASSERTION markers (the default): compared as full normalized lines,
      value/digits included -- unchanged behavior from before the split.
    - OBSERVABILITY markers: compared by KEY occurrence COUNT only -- a
      candidate with the same count of e.g. `DLL_SIZE_BYTES=...` lines as
      the baseline passes regardless of the value(s); a candidate missing
      the marker entirely, or with a different count, still FAILS. This is
      classification by marker NAME before values are looked at, not a
      value-level tolerance.

    Each delta line is ``+N <token>`` (candidate has N more) or
    ``-N <token>`` (candidate has N fewer), sorted for deterministic output.
    """
    baseline_lines = extract(baseline_text)
    candidate_lines = extract(candidate_text)

    def split(lines: list[str]) -> tuple[list[str], collections.Counter]:
        assertion_lines = [
            ln for ln in lines if _marker_key(ln) not in OBSERVABILITY_MARKERS
        ]
        observability_keys = collections.Counter(
            k for ln in lines if (k := _marker_key(ln)) in OBSERVABILITY_MARKERS
        )
        return assertion_lines, observability_keys

    baseline_assertion, baseline_obs = split(baseline_lines)
    candidate_assertion, candidate_obs = split(candidate_lines)

    baseline_counts = counts(baseline_assertion)
    candidate_counts = counts(candidate_assertion)
    ledger_lines_for_leg = {
        entry.line for entry in EXPECTED_ADDITIONS if leg is not None and entry.leg == leg
    }
    all_tokens = set(baseline_counts) | set(candidate_counts)
    deltas = []
    for token in sorted(all_tokens):
        delta = candidate_counts[token] - baseline_counts[token]
        if delta > 0 and token in ledger_lines_for_leg:
            # An expected addition: this exact line is on the ledger for
            # this leg, and it is a POSITIVE delta (a `-N` is never
            # suppressed, per the invariant above). Reported separately by
            # the caller via expected_addition_report_lines(), not counted
            # here.
            continue
        if delta != 0:
            sign = "+" if delta > 0 else "-"
            deltas.append(f"{sign}{abs(delta)} {token}")

    all_obs_keys = set(baseline_obs) | set(candidate_obs)
    for key in sorted(all_obs_keys):
        delta = candidate_obs[key] - baseline_obs[key]
        if delta != 0:
            sign = "+" if delta > 0 else "-"
            deltas.append(f"{sign}{abs(delta)} {key}=<OBSERVABILITY>")

    rc = 1 if deltas else 0
    return rc, deltas


def observability_report_lines() -> list[str]:
    """Returns one printable line per observability marker in force, for
    visibility in the marker-diff output (not just as an internal set)."""
    return [
        f"OBSERVABILITY_MARKER {name} -- count-only, value tolerated"
        for name in sorted(OBSERVABILITY_MARKERS)
    ]


def expected_addition_report_lines(leg: str | None) -> list[str]:
    """Returns one printable line per EXPECTED_ADDITIONS ledger entry scoped
    to ``leg``, for visibility in the marker-diff output -- printed on every
    run regardless of whether the addition is currently present, exactly
    like observability_report_lines()."""
    return [
        f"EXPECTED_ADDITION {entry.line} @ {entry.leg} -- {entry.source}"
        for entry in EXPECTED_ADDITIONS
        if leg is not None and entry.leg == leg
    ]


def main(argv=None) -> int:
    from . import report

    parser = argparse.ArgumentParser(prog="marker-diff")
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--leg", default=None)
    args = parser.parse_args(argv)

    baseline_text = Path(args.baseline).read_text(encoding="utf-8")
    candidate_text = Path(args.candidate).read_text(encoding="utf-8")
    rc, deltas = diff(baseline_text, candidate_text, leg=args.leg)

    for line in observability_report_lines():
        report.plain(line)
    for line in expected_addition_report_lines(args.leg):
        report.plain(line)
    for line in deltas:
        report.plain(line)
    report.marker("MARKER_DIFF_RESULT", "FAIL" if rc else "PASS")
    report.marker("MARKER_DIFF_DELTAS", len(deltas))
    return rc


if __name__ == "__main__":
    sys.exit(main())
