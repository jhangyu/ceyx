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
     script producing two ledger keys (as `check_alias_table_convention.py`
     does today, for TABLE_COUNT and ALIAS_TABLE_FIRST_ELEMENT_ALL_AT) is
     only ever invoked once; its output is cached.
  3. A key that is neither classified as a build artifact NOR has a known
     local producer is a HARD FAIL, not a skip -- an unclassifiable entry
     means either this file's own classification tables are stale, or the
     ledger just grew an entry nobody taught this gate how to verify. A
     check that quietly does nothing for an input it does not recognise is
     the exact false-green shape this campaign keeps re-discovering.

WI-44 (root cause: CI run 34746593820 red on a false premise of mine).
TABLE_COUNT/ALIAS_TABLE_FIRST_ELEMENT_ALL_AT's producer
(`check_alias_table_convention.py`) takes a POSITIONAL PATH ARGUMENT
pointing into the vendored LibRaw tree (`native/third_party/libraw/...`),
which is fetched by a build.yml step that runs AFTER `ci.py selftest`.
This file's own dev machine has that tree checked out already (from a
previous local build), so the producer ran and passed here while failing
on every fresh CI runner -- the fifth appearance that day of "a check
passes locally because the machine has something the runner does not."

I originally classified these two markers as having "a real, identifiable
local producer" and explicitly rejected `_BUILD_ARTIFACT_KEYS` for them.
That classification is not wrong in general (the script IS a real,
deterministic, non-CI-only producer) -- what was missing is recognising
that a producer can have an ABSENT INPUT independently of whether it is a
"build artifact." Forcing this into `_BUILD_ARTIFACT_KEYS` would have been
a category error: `_BUILD_ARTIFACT_KEYS`'s contract is "never producible
on a laptop without a full compile," which is FALSE here -- a laptop with
the vendor tree fetched (as this one has) reproduces it exactly. The
correct fix is a THIRD, orthogonal outcome, not a stretch of an existing
one: if a producer's positional argv references a path that does not
exist on disk, print a DECLARED, NAMED skip (naming the entry and the
missing path) and move on -- never silent, and never conflated with
"cannot exist outside CI." See `_run_producer`'s precondition check below.

Run with: python3 native/scripts/ci/check_expected_additions.py
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
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
    import ci.workflow_scan as workflow_scan  # noqa: E402
else:
    from . import markerdiff  # noqa: E402
    from . import report  # noqa: E402
    from . import run  # noqa: E402
    from . import workflow_scan  # noqa: E402

# Marker KEY -> (the repo-root-relative bare-script path, its argv tuple)
# that locally produces it. Extend this table, never `markerdiff.py`, when
# a future push adds a new guard-emitted ledger entry.
#
# WI-43: the value used to be a bare `str` (script path only), which
# silently assumed every producer is argument-free -- true of the first two
# entries by accident, not by design. `check_alias_table_convention.py`
# requires a positional path argument (its own `usage:` line proves it),
# so the value became `(script, argv)`; `_run_producer` now passes argv
# through rather than invoking every script bare.
#
# This is the SAME literal argv `.github/workflows/build.yml`'s "Check
# alias-table convention" step passes (`native/third_party/libraw/src/
# metadata/normalize_model.cpp`) -- two independent statements of the same
# fact, deliberately not derived from each other (one reading the other at
# runtime could never disagree with it, which defeats the point).
# `test_check_expected_additions.py::test_producer_map_argv_matches_workflows`
# is the mechanical check that the two do not drift apart.
#
# WI-53 / guard (f)③: that binding used to cover exactly ONE of this map's
# two distinct producer invocations (the alias-table one, hardcoded by key
# name). The `check_shell_prohibition.py` pair had NO argv binding at all,
# and the blindness was mechanically demonstrated before it was fixed:
# perturbing either side -- the map's argv or `build.yml`'s real `run:`
# line -- left the entire 9-test suite green
# (`tmp/verify/wi53/red-A-before.txt`, `red-B-before.txt`). The binding is
# now driven BY this map rather than by a hardcoded key, so an entry added
# here in future is bound automatically instead of silently unbound.
_KEY_TO_PRODUCER_SCRIPT: dict[str, tuple[str, tuple[str, ...]]] = {
    "TABLE_COUNT": (
        "native/scripts/check_alias_table_convention.py",
        ("native/third_party/libraw/src/metadata/normalize_model.cpp",),
    ),
    "ALIAS_TABLE_FIRST_ELEMENT_ALL_AT": (
        "native/scripts/check_alias_table_convention.py",
        ("native/third_party/libraw/src/metadata/normalize_model.cpp",),
    ),
}

# Marker KEYs whose only producer is a real CI build job (not reproducible
# on a laptop without a full compile). Empty today -- every current ledger
# entry has a local producer. Add an entry here ONLY with a comment naming
# the actual CI step that emits it; an unnamed/uncommented addition is a
# review defect.
_BUILD_ARTIFACT_KEYS: frozenset[str] = frozenset()


WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

# Interpreter tokens that can precede a script path in a workflow `run:`
# body. Named rather than regex-guessed so a future `py -3` on a Windows
# leg is a reviewable one-line addition instead of a silent miss.
_INTERPRETER_TOKENS: frozenset[str] = frozenset({"python3", "python", "py"})


@dataclass(frozen=True)
class WorkflowInvocation:
    """One `python3 <script> [argv...]` invocation found in a workflow."""

    workflow: str  # file name, e.g. "build.yml"
    step_name: str
    line: int  # 1-based line of the `run:` key owning this invocation
    script: str  # the script path token exactly as the workflow writes it
    argv: tuple[str, ...]  # everything after the script token


# TRANSITIVE (container) INVOCATION DETECTION WAS REMOVED HERE, DELIBERATELY.
# `iter_container_invocations()` / `iter_all_invocations()` existed because
# Phase 1 moved ten guard steps into one `ci.py guards --docker` step, so a
# ledger producer could be invoked WITHOUT any `run:` line naming it. That
# shape no longer exists in the producer map: the only producer left
# (`check_alias_table_convention.py`, two ledger keys) is invoked by its own
# `run:` line in build.yml -- measured at the removal tip, static=1 /
# container=0 for that script, i.e. the machinery had no production consumer.
# IF A FUTURE LEDGER ENTRY'S PRODUCER IS A `guards.GUARDS` MEMBER WITH NO
# `run:` LINE OF ITS OWN, restore it rather than relaxing the "exactly one
# invocation" assertion in
# `test_check_expected_additions.py::test_producer_map_argv_matches_workflows`
# -- relaxing that assertion is the false-green route, since it would let a
# producer no workflow invokes at all pass silently. Recover the prior
# implementation (and its five ContainerInvocationTests, which pinned the
# both-conditions rule) from this file's history.


def iter_workflow_invocations(script_relpath: str, workflows_dir: Path | None = None):
    """Yields a `WorkflowInvocation` for every invocation of
    ``script_relpath`` across EVERY workflow file -- not just `build.yml`,
    because "which workflow wires this producer" is precisely the fact that
    must not be assumed (WI-53 / guard (f)③).

    WHY THIS LIVES HERE AND IS PUBLIC: guard (g) needs the same
    workflow-invocation extraction, and this repo's standing rule is *never
    write a second counter, call this one* -- the same rule
    `workflow_scan.code_lines()` carries. A second extractor is how the two
    disagree silently. Line-level parsing is delegated to
    `workflow_scan`; this function only adds command-token semantics.

    Continuation lines (`\\` at end of a shell line, as
    `assert_import_closure.py`'s invocation uses) are JOINED before
    tokenizing -- otherwise a multi-line invocation's argv would be read as
    empty, which is a false PASS shape, not a crash.
    """
    wf_dir = WORKFLOWS_DIR if workflows_dir is None else Path(workflows_dir)
    for path in sorted(wf_dir.glob("*.yml")):
        text = path.read_text(encoding="utf-8")
        for step in workflow_scan.iter_run_steps(text, path.name):
            for command in _joined_commands(workflow_scan.code_lines(step)):
                tokens = command.split()
                for idx, token in enumerate(tokens):
                    if token != script_relpath:
                        continue
                    if idx == 0 or tokens[idx - 1] not in _INTERPRETER_TOKENS:
                        # A bare mention (e.g. inside a string or as an
                        # argument to something else) is NOT an invocation.
                        continue
                    yield WorkflowInvocation(
                        workflow=path.name,
                        step_name=step.step_name,
                        line=step.start_line,
                        script=token,
                        argv=tuple(tokens[idx + 1:]),
                    )


def invoked_scripts(command: str) -> set[str]:
    """Script path tokens INVOKED by one joined command line -- i.e. each
    token immediately preceded by an interpreter token. A bare mention is
    not an invocation.

    Public and shared on purpose: `check_wiring_is_ledger.py` (guard (f)②)
    decides "is this step marker-emitting?" from the same token rule that
    `iter_workflow_invocations` uses, so the two can never disagree about
    what counts as invoking a script.
    """
    tokens = command.split()
    return {
        token
        for idx, token in enumerate(tokens)
        if idx > 0 and tokens[idx - 1] in _INTERPRETER_TOKENS
    }


def _joined_commands(code: list[str]) -> list[str]:
    """Joins shell line-continuations so one logical command is one string."""
    out: list[str] = []
    pending: list[str] = []
    for line in code:
        if line.endswith("\\"):
            pending.append(line[:-1].strip())
            continue
        pending.append(line)
        out.append(" ".join(p for p in pending if p))
        pending = []
    if pending:
        out.append(" ".join(p for p in pending if p))
    return out


def _marker_key(line: str) -> str:
    return line.split("=", 1)[0]


def _missing_producer_inputs(argv: tuple[str, ...]) -> list[str]:
    """WI-44: a producer's positional `argv` can name a path this file must
    verify exists BEFORE running the producer -- `check_alias_table_convention.py`'s
    single positional argument is exactly this shape (a file inside the
    vendored LibRaw tree, absent until build.yml's fetch step runs).
    Returns the argv entries (repo-root-relative) that do not exist on
    disk, in argv order. A producer with no path-shaped argv at all (like
    `check_shell_prohibition.py`'s `()`) always returns an empty list,
    matching the pre-WI-44 always-run behaviour exactly."""
    return [a for a in argv if not (REPO_ROOT / a).exists()]


def _run_producer(script_relpath: str, argv: tuple[str, ...] = ()) -> list[str]:
    """Runs `script_relpath` (plus any positional `argv` the producer's own
    CLI requires -- WI-43: not every producer is argument-free) and returns
    its combined, normalized output lines -- the same normalization
    `markerdiff.py` applies to a real CI log, so a `<WS>`/`<TMP>`-shaped
    ledger line (none exist today, but the ledger's contract does not
    forbid one) compares correctly. Caller (`main`) is responsible for
    calling `_missing_producer_inputs` first -- this function does not
    re-check on its own, so calling it with a missing input still runs the
    script (and lets it fail on its own terms), which is deliberate: only
    `main`'s loop decides what "missing input" means for the ledger check,
    this function stays a bare, unconditional runner.

    cwd IS EXPLICITLY PINNED TO `REPO_ROOT` (lead15's cwd-dependence
    finding): `_missing_producer_inputs` above (line ~254) checks EVERY argv
    entry (no path-vs-flag discrimination exists in that function -- it
    resolves `REPO_ROOT / a` and asks whether the result exists on disk for
    each entry unconditionally; today's only non-empty argv happens to be a
    single positional path, which is why the blind spot has never been
    exercised) against `REPO_ROOT`. A subprocess launched with the default
    `cwd=None`, in contrast, inherits the CALLER's cwd, not `REPO_ROOT` --
    two code points disagreeing about what a relative argv entry is
    relative TO. From the repo root the two happen to coincide (an accident
    of where every prior green was launched from); from any other cwd
    (e.g. `native/scripts`) the precondition sees the input as present
    while the producer -- resolving the SAME positional path against the
    wrong base -- cannot find it, so this gate blames a stale LEDGER for a
    cwd bug. Pinning `cwd=REPO_ROOT` here makes both resolutions agree,
    which is the only property this file promises."""
    result = run.run([sys.executable, str(REPO_ROOT / script_relpath), *argv], cwd=REPO_ROOT)
    combined = result.stdout + result.stderr
    return [markerdiff.normalize(line) for line in combined.splitlines()]


def main() -> int:
    producer_cache: dict[tuple[str, tuple[str, ...]], list[str]] = {}
    stale: list[tuple] = []
    unclassified: list = []
    # WI-44: two DIFFERENT skip reasons, counted separately so the summary
    # line never conflates them -- "cannot exist outside CI" and "this
    # producer's input happens to be absent right now" are different facts
    # about the world and must read differently to the next person tuning
    # this file's classification tables.
    build_artifact_skipped = 0
    input_missing_skipped = 0
    checked = 0

    for entry in markerdiff.EXPECTED_ADDITIONS:
        key = _marker_key(entry.line)

        if key in _BUILD_ARTIFACT_KEYS:
            print(f"SKIP (build-artifact, not locally producible): {entry.line} (source: {entry.source})")
            build_artifact_skipped += 1
            continue

        producer = _KEY_TO_PRODUCER_SCRIPT.get(key)
        if producer is None:
            unclassified.append(entry)
            continue
        script, argv = producer

        # WI-44: a producer's own positional argv can name a path (e.g.
        # inside the vendored LibRaw tree) that this dev machine happens to
        # have and a fresh CI runner does not yet, because the fetch step
        # runs AFTER `ci.py selftest`. Declared, named skip -- never a
        # silent one, and never conflated with `_BUILD_ARTIFACT_KEYS`
        # (whose contract is "no laptop can ever produce this," which is
        # false here: this exact laptop just did, moments ago).
        missing_inputs = _missing_producer_inputs(argv)
        if missing_inputs:
            print(
                f"SKIP (producer input not present locally: {missing_inputs[0]!r}): "
                f"{entry.line} (source: {entry.source}) -- {script} needs this path, which "
                "a real CI runner only has after its fetch step; unverifiable before that "
                "step runs, not a build-artifact."
            )
            input_missing_skipped += 1
            continue

        if producer not in producer_cache:
            producer_cache[producer] = _run_producer(script, argv)
        emitted = producer_cache[producer]
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
    print(
        f"expected_additions: OK ({checked} producible entr{plural} verified, "
        f"{build_artifact_skipped} build-artifact skip(s), "
        f"{input_missing_skipped} producer-input-missing skip(s))"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
