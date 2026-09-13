#!/usr/bin/env python3
"""Guard (h) -- folded-YAML reverse sentinel (parking-lot round, WI-55).

PROPERTY GUARDED (docs/logs/2026-09-13/pyci-ruling-G-guard-plan.md `(h)`):
an allowlist entry's stated REASON does not contradict the tree it
describes -- independent of whether `check_shell_prohibition.py`'s own
physical-line classifier is right, wrong, or later fixed.

THE DEFECT THIS CATCHES. `check_shell_prohibition.py`'s Rule-1 compliance
test (`PYTHON_BODY_RE` against `workflow_scan.code_lines()`) is a
PHYSICAL-LINE predicate: it counts non-comment source lines inside a
`run:` block, so a `run: >` FOLDED scalar spread across several physical
lines that YAML itself folds into a single logical `python3 ...` command
reads as "many code lines" to that classifier, even though the step is
already a compliant one-line python invocation. Blob `a7840ef6`
(`native/scripts/ci/allowlist.py:80,82,84,97,99`, see the ruling doc for
the full citation) carried exactly this shape: five entries whose reason
said "not yet migrated to native/scripts/ci/ ..." while their real,
YAML-parsed `run:` body was already a single `python3
native/scripts/ci.py dist-build ...` invocation.

This is NOT a cheaper version of "teach the classifier to fold" (a DESIGN
item, still open) and is not superseded by it -- see the ruling doc `(h)`
item 5. A correct classifier removes ONE CAUSE of a stale reason; this
sentinel catches the STALE REASON ITSELF, by comparing prose against the
real parsed tree, regardless of cause (including the ordinary case of a
human writing "not yet migrated" on a step someone else migrated by hand).

RULE. For every `MUST_STAY` entry whose `reason` contains the literal
substring `"not yet migrated"`:
  * Find the entry's step in the workflow files (an entry naming a step
    that no longer exists at all is `check_shell_prohibition.py`'s own
    `[stale-allowlist]` concern -- silently skipped here, not double-
    reported).
  * Parse that workflow file with a REAL YAML parser (never the
    physical-line scanner -- that IS the classifier this guard exists to
    be independent of) and read the step's actual `run:` value.
  * If that YAML-parsed value, once its sole trailing newline is
    stripped, contains no embedded newline AND matches
    `check_shell_prohibition.PYTHON_BODY_RE` -- the reason claims
    "not yet migrated" about a step whose real body is already exactly
    one python invocation -- this is a `[folded-yaml-reverse-sentinel]`
    ERROR.

Deliberately SILENT (no finding, of either polarity) on:
  * any entry whose reason does not contain "not yet migrated" at all
    (permanent/parked/blocked entries such as the `Force LF line
    endings...` "C-G14 #6" trio are never even examined against their
    body -- their body is genuinely non-compliant real bash, and their
    CURRENT reason does not make the claim this guard checks);
  * any entry naming a step no longer present in any workflow file.

Run with: python3 native/scripts/ci/check_folded_yaml_reverse_sentinel.py
"""

from __future__ import annotations

import sys
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[3]

sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))

# Dual-mode import -- see check_shell_prohibition.py's own comment for why
# both branches are required (bare-script run vs. package-member run).
if not __package__:
    import ci.allowlist as allowlist  # noqa: E402
    import ci.check_shell_prohibition as shell_prohibition  # noqa: E402
else:
    from . import allowlist  # noqa: E402
    from . import check_shell_prohibition as shell_prohibition  # noqa: E402

WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

REASON_SENTINEL = "not yet migrated"

# Reused, never re-implemented: this guard's whole point is comparing
# against the REAL parsed tree, not against a second copy of the
# classifier's own judgement of "single python invocation".
PYTHON_BODY_RE = shell_prohibition.PYTHON_BODY_RE


def _parsed_run_bodies(workflow_files) -> dict:
    """(workflow_file_name, step_name) -> the step's real, YAML-parsed
    `run:` string value (or None if the step has no `run:` key at all,
    e.g. a `uses:` step). Steps without a `name:` are skipped -- every
    step in this repo's workflows is named (workflow_scan.py's own module
    docstring records the same grep-verified fact) and an entry can only
    ever be keyed by a step name in the first place.
    """
    bodies: dict = {}
    for path in workflow_files:
        doc = yaml.safe_load(path.read_text())
        jobs = (doc or {}).get("jobs") or {}
        for job in jobs.values():
            if not isinstance(job, dict):
                continue
            for step in job.get("steps") or []:
                if not isinstance(step, dict):
                    continue
                name = step.get("name")
                if not name:
                    continue
                bodies[(path.name, name)] = step.get("run")
    return bodies


def _is_single_python_invocation(run_value) -> bool:
    """True iff the REAL parsed `run:` value is, after YAML's own folding
    has already happened (that is what `yaml.safe_load` just did), a
    single logical line matching the same python-invocation shape
    `check_shell_prohibition.py` requires of a compliant one-liner.

    A folded (`>`) scalar collapses line breaks between two non-blank,
    equally-indented lines into a single space; it does NOT collapse a
    blank line (folds to an embedded newline) or a more-indented
    continuation (kept literal, with its newline). So checking for "no
    embedded newline left after stripping the sole trailing one" is
    exactly "this step, however it was written in the YAML, resolves to
    one logical shell command" -- the property the reason string
    "not yet migrated" is being checked against.
    """
    if not isinstance(run_value, str):
        return False
    body = run_value[:-1] if run_value.endswith("\n") else run_value
    if "\n" in body:
        return False
    return bool(PYTHON_BODY_RE.match(body.strip()))


def find_contradictions(entries, parsed_bodies: dict) -> list:
    """Returns a list of (entry, run_value) for every MUST_STAY entry
    whose reason claims non-migration while its real parsed body is
    already a single python invocation. Pure function, no I/O -- tests
    call this directly against constructed fixtures.
    """
    contradictions = []
    for entry in entries:
        if REASON_SENTINEL not in entry.reason:
            continue
        key = (entry.workflow, entry.step_name)
        if key not in parsed_bodies:
            # No such step in any workflow file today -- that is
            # check_shell_prohibition.py's [stale-allowlist] concern, not
            # this guard's; stay silent rather than double-report.
            continue
        run_value = parsed_bodies[key]
        if _is_single_python_invocation(run_value):
            contradictions.append((entry, run_value))
    return contradictions


def main(argv=None, repo_root: Path = REPO_ROOT, workflows_dir=None, entries=None) -> int:
    workflows_dir = workflows_dir if workflows_dir is not None else (repo_root / ".github" / "workflows")
    entries = entries if entries is not None else allowlist.MUST_STAY

    workflow_files = sorted(workflows_dir.glob("*.yml"))
    if not workflow_files:
        print(f"[FAIL] no workflow files found under {workflows_dir}")
        return 1

    parsed_bodies = _parsed_run_bodies(workflow_files)

    examined = [e for e in entries if REASON_SENTINEL in e.reason]
    print(
        f"[check_folded_yaml_reverse_sentinel] {len(examined)} allowlist "
        f"entr(y/ies) carry a \"{REASON_SENTINEL}\" reason; examining each "
        "against its REAL YAML-parsed run body (never the physical-line "
        "classifier)."
    )

    contradictions = find_contradictions(entries, parsed_bodies)

    if contradictions:
        print(
            f"[FAIL] {len(contradictions)} allowlist entr(y/ies) claim "
            "\"not yet migrated\" while their real, YAML-parsed run body "
            "is already a single python invocation -- the reason "
            "contradicts the tree:"
        )
        for entry, run_value in contradictions:
            single_line = run_value[:-1] if run_value.endswith("\n") else run_value
            print(
                f"  {entry.workflow} :: {entry.step_name}: "
                f"[folded-yaml-reverse-sentinel] reason says "
                f"\"{REASON_SENTINEL}\" but parsed body is a single "
                f"invocation: {single_line.strip()!r}"
            )
        return 1

    print(
        "[check_folded_yaml_reverse_sentinel] PASS -- 0 contradictions "
        "between a \"not yet migrated\" reason and its step's real "
        "YAML-parsed run body."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
