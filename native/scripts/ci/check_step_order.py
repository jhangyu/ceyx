#!/usr/bin/env python3
"""Guard (i) -- step-order dependency check.

PROPERTY GUARDED (USER RULING G, section (i)): within a job, no step
consumes a path before the step that produces it. Ordering is a
correctness property of the workflow, not a convention.

RED CASE THIS GUARD IS BUILT AGAINST -- `e7778cdb` (WI-41), CI run
`34744748058`, conclusion "failure". WI-26 wired "Guard -- vendored
LibRaw/RawSpeed provenance + licences" and "Guard -- normalize_model.cpp
alias-table '@'-prefix convention (G2-2)" BEFORE "Fetch vendored LibRaw
distribution", so both guards read a tree that does not exist yet on a
fresh runner. `e7778cdb^` (the parent, i.e. the state the fix corrected)
is the historical red: at that blob the two guard steps sit at lines
343/346 and the fetch step sits at line 391 -- guards precede their
producer. `e7778cdb` itself relocated them to sit after the fetch
(lines decay; re-derive at the tip). See
`tmp/verify/push9-provenance-guard-rootcause.md` for the original
diagnosis.

SCOPE (minimum viable per the ruling): a DECLARATIVE check over known
fetch -> consumer pairs, not a general data-flow analysis. `PRODUCER_PAIRS`
below is the declarative table. Two invariants are enforced by the same
table's shape, deliberately NOT collapsed into one:

  1. For every producer with one or more DECLARED consumers, each consumer
     step must appear strictly AFTER the producer step in the same
     workflow file (by `run:` step start line). A consumer step named in
     the table but ABSENT from the workflow is also a failure -- a
     declared dependency silently disappearing is exactly the kind of
     "unlisted addition" this campaign's other guards exist to catch, and
     staying silent about it here would let (i) rot the moment a consumer
     step is renamed or removed without anyone updating this table.

  2. Producers with ZERO declared consumers are not skipped over in
     silence -- `build_deps.py fetch halide` (`build.yml:374-375`) is
     exactly this shape today, a producer awaiting a consumer, per USER
     RULING G (i)3: "`build_deps.py fetch halide` (:375) is the same
     shape awaiting a consumer." A pair table that only lists satisfied
     pairs cannot show this gap; this one prints an explicit
     `NO_CONSUMER_DECLARED` line for it every run, pass or fail, so the
     gap stays visible rather than reading as "nothing to check here".

This guard is read-only over `.github/workflows/*.yml` -- it writes
nothing, wires nothing itself (its `build.yml` invocation line is a
separate, contended edit per the round's ownership map).

Usage:
  python3 native/scripts/ci/check_step_order.py
  python3 native/scripts/ci/check_step_order.py --workflow path/to/file.yml
Zero required args; workflow files are discovered under
`.github/workflows/` relative to the repo root, derived from this file's
own path so it runs the same locally and in CI regardless of cwd.
"""
from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))

# Dual-mode import -- same rationale and same discriminator as
# check_shell_prohibition.py (read that module's docstring for the full
# two-failure-mode argument): a bare-script run has no parent package, an
# in-process/`-m unittest discover` run needs a relative import instead.
if not __package__:
    import ci.workflow_scan as workflow_scan  # noqa: E402
else:
    from . import workflow_scan  # noqa: E402

WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"


@dataclass(frozen=True)
class ProducerPair:
    """One declarative fetch -> consumer(s) dependency."""
    label: str  # short id for report lines, e.g. "libraw-provenance"
    workflow: str  # workflow file name, e.g. "build.yml"
    producer_step_name: str  # exact `- name:` text of the producing step
    consumer_step_names: tuple  # exact `- name:` text of each consumer step
    # `()` is a DELIBERATE, declared-gap entry (rule 2 above), not an
    # omission -- see NO_CONSUMER_DECLARED reporting in main().


# Declarative table, per USER RULING G (i)3's two concrete pairs at the
# tip, plus the one named awaiting-a-consumer gap.
PRODUCER_PAIRS = (
    ProducerPair(
        label="libraw-provenance-and-alias-table",
        workflow="build.yml",
        producer_step_name="Fetch vendored LibRaw distribution",
        consumer_step_names=(
            "Guard — vendored LibRaw/RawSpeed provenance + licences",
            "Guard — normalize_model.cpp alias-table '@'-prefix convention (G2-2)",
        ),
    ),
    ProducerPair(
        label="halide-awaiting-consumer",
        workflow="build.yml",
        producer_step_name="Fetch vendored Halide v21 distribution",
        consumer_step_names=(),
    ),
)


def _steps_by_name(text: str, workflow_name: str) -> dict:
    """Maps `- name:` text -> its `run:` step's start_line (1-based).

    If a step name repeats in one workflow, the FIRST occurrence's line is
    kept and later occurrences are ignored for lookup purposes -- none of
    today's declared pairs have duplicate names (verified: every step name
    used in PRODUCER_PAIRS appears exactly once per file), so this is a
    conservative default rather than a load-bearing choice.
    """
    out = {}
    for step in workflow_scan.iter_run_steps(text, workflow_name):
        if step.step_name not in out:
            out[step.step_name] = step.start_line
    return out


def check_text(text: str, workflow_name: str, pairs=PRODUCER_PAIRS):
    """Runs every PRODUCER_PAIRS entry scoped to `workflow_name` against
    `text`. Returns (findings, audit_lines) -- findings is a list of
    (label, message) failures; audit_lines is printed every run
    regardless of pass/fail (rule 2: gaps are never silent).
    """
    findings = []
    audit_lines = []
    steps = _steps_by_name(text, workflow_name)

    for pair in pairs:
        if pair.workflow != workflow_name:
            continue

        producer_line = steps.get(pair.producer_step_name)
        if producer_line is None:
            findings.append((
                pair.label,
                f"declared producer step {pair.producer_step_name!r} not "
                f"found in {workflow_name} -- pair table is stale",
            ))
            continue

        if not pair.consumer_step_names:
            audit_lines.append(
                f"NO_CONSUMER_DECLARED [{pair.label}] producer "
                f"{pair.producer_step_name!r} at {workflow_name}:"
                f"{producer_line} has zero declared consumers -- this is "
                "a recorded gap (USER RULING G (i)3), not a pass"
            )
            continue

        for consumer_name in pair.consumer_step_names:
            consumer_line = steps.get(consumer_name)
            if consumer_line is None:
                findings.append((
                    pair.label,
                    f"declared consumer step {consumer_name!r} not found "
                    f"in {workflow_name} -- pair table is stale",
                ))
                continue
            if consumer_line <= producer_line:
                findings.append((
                    pair.label,
                    f"consumer {consumer_name!r} ({workflow_name}:"
                    f"{consumer_line}) is not ordered after its producer "
                    f"{pair.producer_step_name!r} ({workflow_name}:"
                    f"{producer_line})",
                ))
            else:
                audit_lines.append(
                    f"OK [{pair.label}] {workflow_name}:{producer_line} "
                    f"(producer) -> {workflow_name}:{consumer_line} "
                    f"(consumer {consumer_name!r})"
                )

    return findings, audit_lines


def _discover_workflow_names(pairs=PRODUCER_PAIRS):
    seen = []
    for pair in pairs:
        if pair.workflow not in seen:
            seen.append(pair.workflow)
    return seen


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--workflow",
        action="append",
        default=None,
        help="Path to a workflow YAML file to check (repeatable). "
             "Defaults to every workflow named in PRODUCER_PAIRS, resolved "
             "under .github/workflows/.",
    )
    args = parser.parse_args(argv)

    if args.workflow:
        targets = [Path(p) for p in args.workflow]
    else:
        targets = [WORKFLOWS_DIR / name for name in _discover_workflow_names()]

    all_findings = []
    all_audit = []
    checked = 0

    for path in targets:
        if not path.exists():
            all_findings.append((path.name, f"workflow file not found: {path}"))
            continue
        text = path.read_text(encoding="utf-8")
        findings, audit_lines = check_text(text, path.name)
        all_findings.extend(findings)
        all_audit.extend(audit_lines)
        checked += 1

    print(f"[check_step_order] checked {checked} workflow file(s) against "
          f"{len(PRODUCER_PAIRS)} declared pair(s).")
    for line in all_audit:
        print(f"  {line}")

    if all_findings:
        print(f"[FAIL] {len(all_findings)} step-order violation(s):")
        for label, message in all_findings:
            print(f"  [{label}] {message}")
        return 1

    print("[check_step_order] PASS -- every declared consumer is ordered "
          "after its producer; the halide gap is recorded above, not "
          "hidden.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
