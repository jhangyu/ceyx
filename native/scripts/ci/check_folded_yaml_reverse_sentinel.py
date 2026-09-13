#!/usr/bin/env python3
r"""Guard (h) -- folded-YAML reverse sentinel (parking-lot round, WI-55).

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
  * Determine the step's real, FOLDED `run:` body -- see "NO THIRD-PARTY
    YAML PARSER" below for how, and why that is deliberate rather than a
    corner cut.
  * If that folded value, once its sole trailing newline is stripped,
    resolves to no embedded newline AND matches
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
  * any entry naming a step no longer present in any workflow file;
  * (new, see below) any folded body this module cannot confidently fold
    itself -- a blank line inside the body, or a line indented deeper
    than the body's own baseline. The false-negative risk this creates is
    ACCEPTED, not overlooked: see "NO THIRD-PARTY YAML PARSER".
    **STATED IN THE NEGATIVE, so it cannot be misread as coverage: bodies
    outside this module's implemented fold subset are NOT EXAMINED at
    all. Silence on such an entry means "this module declined to judge
    it", never "this module checked it and found it clean." A future
    reader auditing this guard's PASS output cannot distinguish "verified
    clean" from "outside the subset" without reading this paragraph --
    that asymmetry is the accepted cost of removing the PyYAML
    dependency, not an oversight.**

NO THIRD-PARTY YAML PARSER (2026-09-13 fix-forward, CI run `34762557520`).
The first cut of this guard used `import yaml` (PyYAML) to get a fully
correct parse of the folded scalar. That dependency is not installed on
this repo's CI runners (`ModuleNotFoundError: No module named 'yaml'`,
`ci.py selftest`, job "Build native test targets") -- nothing else in
`native/scripts/ci/` depends on a third-party package, and grepping the
package (`grep -rn "^import \|^from " native/scripts/ci/*.py`, run at fix
time) turned up none; adding a provisioning step for one guard would be
the exact "environment the test only passes because of" shape the round's
other three env-dependent test failures share. Rather than provision the
dependency, this module implements JUST ENOUGH of YAML's folded-scalar
(`>`) semantics to answer its own question -- using
`workflow_scan.iter_run_steps()` (already the single source of truth for
"where does this step's `run:` body start and what raw lines does it
own", reused, not re-implemented) plus `workflow_scan._RUN_KEY_RE` to read
back the block-scalar indicator character from the SAME line
`iter_run_steps` already anchored on.

The subset implemented: N physical lines, all at the SAME indentation,
with NO blank line among them, fold to one logical line (join with a
single space, per YAML's fold rule for equally-indented non-blank lines).
Every REAL folded body in this repo's workflows today is exactly this
shape. **SCOPE OF THAT CLAIM, stated precisely so it is not over-read:**
verified against (a) blob `a7840ef6`'s five historical phantom-migration
bodies (`heif_dist_android.yml`, `jxl_dist_android.yml`,
`jxl_dist_windows.yml`, `webp_dist_android.yml`, `webp_dist_windows.yml`
-- the exact bodies quoted in WI-55's test fixtures) and (b) every `run:
>` body live in `.github/workflows/*.yml` at the current tip (this
module's own GREEN test, `test_green_at_current_allowlist_zero_
contradictions`, exercises the real tree, not a fixture). **NOT verified
against every commit in between** -- no claim is made about any
intermediate blob's folded bodies, only these two points. Anything OUTSIDE that
subset (a blank line, which YAML folds to an embedded newline; a
more-indented continuation, which YAML keeps literal) is NOT folded by
this module -- it is reported as "not a single line", which means this
guard stays SILENT on it rather than guessing. This is the SAME direction
the ruling doc already accepts for this guard ("the reverse
false-negative risk is acknowledged by the user, not dismissed") --
`(d)`/`(h)`'s common family: when unsure, decline to flag, never invent a
flag. A guard that is occasionally silent where a full parser would fold
correctly is a smaller defect than a guard the CI pipeline cannot import
at all.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))

# Dual-mode import -- see check_shell_prohibition.py's own comment for why
# both branches are required (bare-script run vs. package-member run).
if not __package__:
    import ci.allowlist as allowlist  # noqa: E402
    import ci.check_shell_prohibition as shell_prohibition  # noqa: E402
    import ci.workflow_scan as workflow_scan  # noqa: E402
else:
    from . import allowlist  # noqa: E402
    from . import check_shell_prohibition as shell_prohibition  # noqa: E402
    from . import workflow_scan  # noqa: E402

WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

REASON_SENTINEL = "not yet migrated"

# Reused, never re-implemented: this guard's whole point is comparing
# against the REAL parsed tree, not against a second copy of the
# classifier's own judgement of "single python invocation".
PYTHON_BODY_RE = shell_prohibition.PYTHON_BODY_RE

# Reused from workflow_scan.py, not duplicated: the SAME regex that module
# already uses to recognize a `run:` key and its optional block-scalar
# indicator (`|`, `>`, `>-`, `>+`, ...). Re-matching it here against the
# already-anchored `start_line` is how this module learns "was this body
# FOLDED (`>...`)  or LITERAL (`|...`) or inline" without re-implementing
# `iter_run_steps`'s own step/line bookkeeping a second time.
_RUN_KEY_RE = workflow_scan._RUN_KEY_RE


def _block_indicator(all_lines: list, step) -> str:
    """Returns the block-scalar indicator character(s) ("", "|", ">",
    ">-", ">+", ...) for `step`, by re-reading `step.start_line`'s own
    text -- the exact line `workflow_scan.iter_run_steps` already
    anchored `step` on. "" means an inline `run: <command>` (no block
    scalar at all, i.e. already a single physical line)."""
    line = all_lines[step.start_line - 1]
    m = _RUN_KEY_RE.match(line.lstrip(" "))
    if not m:
        return ""
    return m.group(1) or ""


def _fold_body(indicator: str, body_lines: list) -> "str | None":
    """Folds `step.body_lines` ((line_no, text) pairs, as returned by
    `workflow_scan.iter_run_steps`) into a single logical line, IF this
    module can do so with confidence -- see the module docstring's "NO
    THIRD-PARTY YAML PARSER" section for exactly which shapes qualify.
    Returns `None` (not "can't tell, guess False") when the shape is
    outside that subset, so callers can tell "confidently not a single
    line" apart from "not confident either way" if they ever need to.
    """
    if not body_lines:
        return "" if indicator else None
    if not indicator:
        # Inline `run: <command>` -- iter_run_steps already yields exactly
        # one (start_line, inline_rest) pair for this case.
        return body_lines[0][1]
    if not indicator.startswith(">"):
        # Literal (`|`) block: YAML never folds these -- each physical
        # line stays its own logical line. Only a genuine one-line body
        # is a single line, and that shape is already visible to
        # check_shell_prohibition.py's OWN physical-line classifier
        # (workflow_scan.code_lines()), so this guard has nothing to add
        # for it; still handled here for completeness rather than a
        # silent assumption.
        if len(body_lines) == 1:
            return body_lines[0][1]
        return None
    texts = [text for _, text in body_lines]
    if any(t.strip() == "" for t in texts):
        # A blank line inside a folded scalar becomes an embedded newline
        # in YAML's real fold algorithm -- outside this module's
        # implemented subset (see docstring). Decline, don't guess.
        return None
    indents = [len(t) - len(t.lstrip(" ")) for t in texts]
    base_indent = indents[0]
    if any(i != base_indent for i in indents):
        # A more-indented continuation line stays literal (with its own
        # newline) in YAML's real fold algorithm -- also outside this
        # module's implemented subset. Decline, don't guess.
        return None
    return " ".join(t.strip() for t in texts)


def _parsed_run_bodies(workflow_files) -> dict:
    """(workflow_file_name, step_name) -> the step's real, FOLDED `run:`
    body (a single logical line, per `_fold_body`), or `None` if the step
    has no `run:` key at all (e.g. a `uses:` step) or its body falls
    outside the subset this module can confidently fold. Steps without a
    `name:` are skipped -- every step in this repo's workflows is named
    (workflow_scan.py's own module docstring records the same
    grep-verified fact) and an entry can only ever be keyed by a step
    name in the first place.
    """
    bodies: dict = {}
    for path in workflow_files:
        text = path.read_text()
        all_lines = text.splitlines()
        for step in workflow_scan.iter_run_steps(text, path.name):
            if not step.step_name:
                continue
            indicator = _block_indicator(all_lines, step)
            bodies[(step.workflow, step.step_name)] = _fold_body(indicator, step.body_lines)
    return bodies


def _is_single_python_invocation(run_value) -> bool:
    """True iff the folded `run:` value (see `_fold_body`) is, once its
    sole trailing newline (if any) is stripped, a single logical line
    matching the same python-invocation shape `check_shell_prohibition.py`
    requires of a compliant one-liner.

    A folded (`>`) scalar collapses line breaks between two non-blank,
    equally-indented lines into a single space; it does NOT collapse a
    blank line (folds to an embedded newline) or a more-indented
    continuation (kept literal, with its newline) -- `_fold_body` returns
    `None` rather than guess for either of those two shapes, and `None`
    is not a string, so it always answers `False` here (a false negative
    is accepted; a false positive is not). So checking for "no embedded
    newline left" is exactly "this step, however it was written in the
    YAML, resolves to one logical shell command" -- the property the
    reason string "not yet migrated" is being checked against.
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
