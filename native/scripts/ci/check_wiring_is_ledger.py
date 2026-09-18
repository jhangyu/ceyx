#!/usr/bin/env python3
"""Guard (f)② -- WIRING IS LEDGER: a commit that newly wires a
marker-emitting step into a workflow must carry the matching
`markerdiff.EXPECTED_ADDITIONS` entry IN THE SAME COMMIT RANGE.

WHY THIS EXISTS (the defect, not a hypothetical): WI-26 (`b4e44b84`) wired
`check_alias_table_convention.py` into `build.yml`. That step prints
`TABLE_COUNT` and `ALIAS_TABLE_FIRST_ELEMENT_ALL_AT`. The ledger was NOT
updated in that commit -- it stood at two entries before the wiring and two
entries after it -- so AC-2 on the `nativetests` leg saw two un-ledgered
additions and failed. It took until WI-43 (`bba2a752`) to add the entries,
which are attributed in the ledger to WI-26, the WI that wired the emitter.

**AND CI RUN `34745693241` WAS RUN-LEVEL `"success"` WHILE CARRYING THIS
DEFECT.** The job was green; AC-2 was red. That is simultaneously this
guard's justification and the standing proof that a green CI run can never
be this guard's evidence.

The ledger invariant had only ever been applied to a pinned literal (edit
the number, move the pin). WI-43's own commit message states the
generalization this file implements: "**wiring a step that prints is exactly
as much an emission change as editing a number.**"

WHAT COUNTS AS "MARKER-EMITTING" -- and why it is NOT a new detector.
A step is marker-emitting iff it invokes a script that
`check_expected_additions._KEY_TO_PRODUCER_SCRIPT` already maps to marker
keys. That map is the single existing statement of "which script prints
which marker", and WI-53's (f)③ just bound every one of its entries to the
workflows' real invocations. Deciding marker-emission by scanning script
source for marker-shaped `print`s was considered and is REJECTED: it would
be a second implementation of `markerdiff.MARKER_RE`'s judgement, against
this repo's standing "never write a second counter, call this one" rule,
and it would inherit MARKER_RE's documented both-directions failure (P-12).
Frozen by lead14: no script execution, no marker-vocabulary detection.

>>> SCOPE -- READ THIS BEFORE BELIEVING A PASS <<<
This guard implements OPTION 1, NEW-STEP-ONLY (user ruling ESC-2). It
enforces ONE HALF of guard (f)'s stated property.

    P-24 (OPEN, NEVER TO BE MARKED DONE): an ALREADY-WIRED step whose
    invoked SCRIPT gains a new marker emission in a later commit produces a
    ZERO workflow diff and is INVISIBLE to this guard. AC-2 still catches
    it, one CI round late, and that lag is accepted by ruling.

`P_24_NOT_COVERED` is printed on EVERY run, pass or fail. An unprinted
limitation is the exact failure mode this campaign exists to fight: a
reader of a clean output must not be able to conclude that (f)'s stated
property is enforced. NO DOCUMENT MAY DESCRIBE GUARD (f) AS ENFORCING ITS
STATED PROPERTY.

Run with:
  python3 native/scripts/ci/check_wiring_is_ledger.py
  python3 native/scripts/ci/check_wiring_is_ledger.py --base <rev> --head <rev>
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))

if not __package__:
    import ci.check_expected_additions as check_expected_additions  # noqa: E402
    import ci.report as report  # noqa: E402
    import ci.run as run  # noqa: E402
    import ci.workflow_scan as workflow_scan  # noqa: E402
else:
    from . import check_expected_additions  # noqa: E402
    from . import report  # noqa: E402
    from . import run  # noqa: E402
    from . import workflow_scan  # noqa: E402

_MARKERDIFF_RELPATH = "native/scripts/ci/markerdiff.py"
_WORKFLOW_DIR_RELPATH = ".github/workflows"

DEFAULT_BASE = "origin/main"
DEFAULT_HEAD = "HEAD"


class UnresolvableRevision(RuntimeError):
    """A REVISION named on the command line does not resolve to a commit.

    DELIBERATELY NOT A SUBCLASS of `GitReadError`. The two conditions look
    alike at the git-plumbing layer and are opposites at the semantic layer:

      * `GitReadError` from a PATH query at a VALID rev means "that path did
        not exist then" -- a legitimate, expected answer for a genuinely new
        file. Swallowing it into None/[] is correct, and making it fatal
        would fire this guard on every new workflow added, which gets the
        guard disabled by whoever meets it first.
      * THIS means the BASELINE ITSELF EVAPORATED. Every step then reads as
        new (or none do), and the verdict is computed against nothing.

    Measured, not hypothesised (lead17, `tmp/verify/lead17/
    b31-shallow-red-proof-ADJUDICATION.md`): in a clone with
    `refs/remotes/origin/main` deleted, `NEW_STEPS_EXAMINED` went 0 -> 152
    while RC stayed 0 and the result stayed PASS. The exit code could not
    distinguish a guard examining its real diff from one whose baseline had
    silently vanished. Inheriting from `GitReadError` would put this back:
    `_workflow_names_at_rev`'s `except GitReadError: return []` would catch
    it and restore the vacuous pass.
    """


class GitReadError(RuntimeError):
    """A read-only git query returned non-zero -- normally "this path or rev
    does not exist", which several callers treat as absence rather than as
    an error.

    WI-53 follow-up: this used to be `subprocess.CalledProcessError`, raised
    by `subprocess.run(..., check=True)`. Both had to go. `run.py` is the
    audited subprocess primitive and `check_no_test_execution_in_ci.py`
    AST-scans for **any** `subprocess.*` call outside it -- which includes
    CONSTRUCTING `subprocess.CalledProcessError`, not just running a
    command. So keeping the old exception type to avoid touching callers
    would have left the gate red for the same reason. The guard was
    dissolved, not exempted: adding this file to
    `GRANDFATHERED_SUBPROCESS_FILES` was explicitly barred, and weakening a
    gate is escalate-only.
    """


def _git(*args: str) -> str:
    """Read-only git, through the audited `run` primitive. Never mutates the
    shared working tree -- no checkout, no worktree, no stash (2026-07-06: a
    teammate's uncommitted work is always assumed live in this tree).

    `run.run` NEVER RAISES -- it reports failure as a returncode (and a
    missing executable as 127). The `check=True` behaviour the callers below
    depend on is therefore re-created explicitly here; dropping it would
    silently turn "this rev does not exist" into "this rev is empty", which
    reads as a clean PASS and is exactly the false-green this guard exists
    to reject.
    """
    result = run.run(["git", "-C", str(REPO_ROOT), *args])
    if result.returncode != 0:
        raise GitReadError(
            f"git {' '.join(args)} failed with rc={result.returncode}: "
            f"{result.stderr.strip()}"
        )
    return result.stdout


def _file_at_rev(rev: str, relpath: str) -> str | None:
    try:
        return _git("show", f"{rev}:{relpath}")
    except GitReadError:
        return None  # the path did not exist at that rev


def _resolve_rev(rev: str) -> str:
    """Resolve ``rev`` to a commit id, or raise `UnresolvableRevision`.

    This is a SEPARATE GIT QUERY from any path read, which is the whole
    point: it asks only "does this revision exist", so its failure carries
    exactly one meaning and cannot be confused with "this path is new". It
    must therefore NOT be implemented by noticing that a path read failed.
    """
    try:
        return _git("rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}").strip()
    except GitReadError as exc:
        raise UnresolvableRevision(
            f"revision {rev!r} does not resolve to a commit in this checkout. "
            "This guard compares two revisions; with one of them missing it "
            "would compute its verdict against an empty baseline and report a "
            "PASS that verified nothing. Commonest cause: a shallow or "
            "single-branch checkout that never fetched the base ref -- this "
            "guard requires `fetch-depth: 0` and an explicitly fetched base."
        ) from exc


def _workflow_names_at_rev(rev: str) -> list[str]:
    try:
        listing = _git("ls-tree", "--name-only", f"{rev}:{_WORKFLOW_DIR_RELPATH}")
    except GitReadError:
        return []
    return [n for n in listing.splitlines() if n.endswith((".yml", ".yaml"))]


def ledger_keys_at_rev(rev: str) -> set[str]:
    """Marker KEYS on `markerdiff.EXPECTED_ADDITIONS` as of ``rev``.

    The module source at that rev is EXECUTED in a throwaway namespace and
    its real `EXPECTED_ADDITIONS` is read, rather than regex-scraped -- so
    this is not a second implementation of the ledger's structure. It must
    be read at the RANGE'S HEAD, never from the working tree: reading the
    working tree would let today's ledger vouch for a historical commit and
    silently pass the very replay that proves this guard works.
    """
    source = _file_at_rev(rev, _MARKERDIFF_RELPATH)
    if source is None:
        return set()
    namespace: dict = {"__name__": "_markerdiff_at_rev", "__file__": _MARKERDIFF_RELPATH}
    exec(compile(source, f"{rev}:{_MARKERDIFF_RELPATH}", "exec"), namespace)
    return {
        entry.line.split("=", 1)[0]
        for entry in namespace.get("EXPECTED_ADDITIONS", ())
    }


def _steps_at_rev(rev: str) -> dict[tuple[str, str], list[str]]:
    """(workflow, step_name) -> its joined command lines, at ``rev``.

    Resolves ``rev`` FIRST and lets `UnresolvableRevision` propagate. An
    empty return from here is then a real statement about the revision's
    contents ("no workflows there") rather than the residue of a revision
    that was never found -- the two used to be indistinguishable.
    """
    _resolve_rev(rev)
    steps: dict[tuple[str, str], list[str]] = {}
    for name in _workflow_names_at_rev(rev):
        text = _file_at_rev(rev, f"{_WORKFLOW_DIR_RELPATH}/{name}")
        if text is None:
            continue
        for step in workflow_scan.iter_run_steps(text, name):
            commands = check_expected_additions._joined_commands(
                workflow_scan.code_lines(step)
            )
            steps[(name, step.step_name)] = commands
    return steps


def _producer_keys_for_commands(commands: list[str]) -> set[str]:
    """Marker keys a step would emit, decided SOLELY by the existing
    producer map -- no vocabulary detection (see module docstring)."""
    invoked: set[str] = set()
    for command in commands:
        invoked |= check_expected_additions.invoked_scripts(command)
    return {
        key
        for key, (script, _argv) in check_expected_additions._KEY_TO_PRODUCER_SCRIPT.items()
        if script in invoked
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="check-wiring-is-ledger")
    parser.add_argument("--base", default=DEFAULT_BASE)
    parser.add_argument("--head", default=DEFAULT_HEAD)
    args = parser.parse_args(argv)

    # Printed on EVERY run, before any verdict -- see the module docstring.
    print(
        "P_24_NOT_COVERED: this guard is OPTION 1, NEW-STEP-ONLY. A new marker "
        "emission added to the SCRIPT of an ALREADY-WIRED step leaves the workflow "
        "diff empty and is NOT detected here; AC-2 catches it one CI round later. "
        "Guard (f) does NOT enforce its full stated property."
    )
    print(f"WIRING_LEDGER_RANGE={args.base}..{args.head}")

    # FAIL CLOSED on an unresolvable revision. Before this, an absent
    # `origin/main` made `base_steps` empty, every wired step read as new,
    # and the run still printed PASS with RC=0 -- measured at 152 phantom
    # new steps (see `UnresolvableRevision`). A guard whose baseline can
    # evaporate without changing its exit code is not a guard.
    try:
        base_steps = _steps_at_rev(args.base)
        head_steps = _steps_at_rev(args.head)
    except UnresolvableRevision as exc:
        print("WIRING_LEDGER_RESULT=ERROR")
        report.error(f"[wiring-is-ledger] {exc}")
        return 2

    new_step_keys = [k for k in head_steps if k not in base_steps]

    offenders: list[tuple[str, str, set[str]]] = []
    marker_step_count = 0
    ledger = ledger_keys_at_rev(args.head)

    for workflow, step_name in sorted(new_step_keys):
        produced = _producer_keys_for_commands(head_steps[(workflow, step_name)])
        if not produced:
            continue
        marker_step_count += 1
        missing = produced - ledger
        print(
            f"NEW_MARKER_STEP {workflow} :: {step_name!r} emits "
            f"{sorted(produced)} -- ledger {'MISSING ' + str(sorted(missing)) if missing else 'OK'}"
        )
        if missing:
            offenders.append((workflow, step_name, missing))

    # Counts are printed so an RC=0 over a range with NOTHING to check is
    # distinguishable from an RC=0 that actually verified something. A
    # zero-examined pass is not acceptance evidence (f2-PREREG.md §3 V1).
    print(f"NEW_STEPS_EXAMINED={len(new_step_keys)}")
    print(f"NEW_MARKER_EMITTING_STEPS={marker_step_count}")

    for workflow, step_name, missing in offenders:
        report.error(
            f"[wiring-is-ledger] {workflow} :: {step_name!r} is NEWLY WIRED in "
            f"{args.base}..{args.head} and emits {sorted(missing)}, but "
            f"markerdiff.EXPECTED_ADDITIONS at {args.head} has no entry for "
            f"{'it' if len(missing) == 1 else 'them'}. Wiring a step that prints is "
            "exactly as much an emission change as editing a number -- add the ledger "
            "entry in the SAME COMMIT as the wiring, or AC-2 fails after the push on an "
            "otherwise-correct change."
        )

    if offenders:
        print("WIRING_LEDGER_RESULT=FAIL")
        return 1
    print("WIRING_LEDGER_RESULT=PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
