#!/usr/bin/env python3
"""Render codec_capability_probe.py --expect vectors from the expectation
ledger, and check the ledger against what the CI workflows actually assert.

Spec: docs/logs/2026-08-31/plan-ci-codec-integration.md, Task 2 (CI-T2).

The ledger (native/deps/codec_expectations.toml) is the single reviewable
table of per-leg codec capability expectations. This script is the only
consumer that turns a ledger leg into the CLI vector
codec_capability_probe.py expects, and the only tool that mechanically
proves a workflow's hard-coded `--expect` tokens have not drifted from the
ledger (`--check`).

The ledger records what a leg SHOULD assert -- it is never edited to match a
red run. A `0` cell without a `reason`/`owner` is rejected at load time: an
honest zero must be traceable to an owning task, or it silently reads as an
accepted permanent state (round-4 ruling).
"""
import argparse
import pathlib
import re
import sys
import tomllib

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_LEDGER_PATH = REPO_ROOT / "native" / "deps" / "codec_expectations.toml"
DEFAULT_WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

# Matches the tokens codec_capability_probe.py's --expect flag accepts:
# <format>:<direction>=<0|1>, e.g. "heic:encode=1".
_EXPECT_TOKEN_RE = re.compile(r"\b([a-z0-9_]+:[a-z0-9_]+=[01])\b")


class LedgerError(Exception):
    """The ledger file is malformed or violates the reason/owner rule."""


def load_ledger(path=None):
    """Parse and validate the ledger. Returns {leg: {"instrument": str, "workflow": str, "expect": {pair: int}}}.

    Every `0` cell must be a table carrying non-empty `reason` and `owner`
    strings, or this raises LedgerError. The returned `expect` dict values
    are plain ints (0 or 1); reason/owner are validation-only metadata, not
    part of the CLI vector.

    `workflow` is the leg's own workflow filename under .github/workflows/
    (e.g. "windows_build.yml") -- required so `check()` can compare each
    leg's expectations against ONLY its own workflow file, not the union of
    every leg's tokens against the union of every workflow's tokens (round-2
    should-fix #3: a union comparison lets a leg wired with the wrong vector
    pass silently whenever some OTHER leg legitimately asserts the same
    token, in both directions -- a leg's wrong 0 hides behind another leg's
    honest 0, and that leg's missing 1 hides behind a third leg's honest 1).
    """
    path = pathlib.Path(path) if path is not None else DEFAULT_LEDGER_PATH
    with open(path, "rb") as f:
        raw = tomllib.load(f)

    legs = {}
    for leg, table in raw.items():
        if "instrument" not in table:
            raise LedgerError(f"leg {leg!r} is missing required key 'instrument'")
        if "workflow" not in table:
            raise LedgerError(f"leg {leg!r} is missing required key 'workflow'")
        expect_raw = table.get("expect", {})
        expect = {}
        for pair, entry in expect_raw.items():
            if isinstance(entry, bool):
                raise LedgerError(f"leg {leg!r} pair {pair!r}: boolean is not a valid expectation")
            if isinstance(entry, int):
                if entry == 1:
                    expect[pair] = 1
                elif entry == 0:
                    raise LedgerError(
                        f"leg {leg!r} pair {pair!r}: a bare 0 is rejected -- every 0 "
                        f"must carry a sibling reason and owner, e.g. "
                        f'{{ value = 0, reason = "...", owner = "..." }}'
                    )
                else:
                    raise LedgerError(f"leg {leg!r} pair {pair!r}: value must be 0 or 1, got {entry}")
            elif isinstance(entry, dict):
                value = entry.get("value")
                if value != 0:
                    raise LedgerError(
                        f"leg {leg!r} pair {pair!r}: table form is only valid for value = 0, "
                        f"got value = {value!r}"
                    )
                reason = entry.get("reason")
                owner = entry.get("owner")
                if not reason or not isinstance(reason, str):
                    raise LedgerError(f"leg {leg!r} pair {pair!r}: 0 entry missing non-empty 'reason'")
                if not owner or not isinstance(owner, str):
                    raise LedgerError(f"leg {leg!r} pair {pair!r}: 0 entry missing non-empty 'owner'")
                expect[pair] = 0
            else:
                raise LedgerError(f"leg {leg!r} pair {pair!r}: unrecognised entry type {type(entry)}")
        legs[leg] = {
            "instrument": table["instrument"],
            "workflow": table["workflow"],
            "step_anchor": table.get("step_anchor"),
            "expect": expect,
        }
    return legs


def render(leg, ledger=None):
    """Return the sorted list of '--expect', '<pair>=<val>' argv fragments for `leg`."""
    legs = ledger if ledger is not None else load_ledger()
    if leg not in legs:
        raise LedgerError(f"unknown leg {leg!r}; known legs: {', '.join(sorted(legs))}")
    fragment = []
    for pair in sorted(legs[leg]["expect"]):
        value = legs[leg]["expect"][pair]
        fragment.append("--expect")
        fragment.append(f"{pair}={value}")
    return fragment


# Matches a GitHub Actions step header line, e.g. "      - name: Foo bar".
# Used to slice a workflow file into per-step blocks when a leg needs a
# `step_anchor` discriminator (see _tokens_in_file below).
_STEP_HEADER_RE = re.compile(r"(?m)^([ \t]*)-\s*name:.*$")


def _tokens_in_file(path, step_anchor=None):
    """Return the FMT:DIR=VAL token set found in a single workflow file.

    If `step_anchor` is given, scanning is restricted to the text of the ONE
    GitHub Actions step whose `- name:` line contains that substring (from
    that step header up to, but not including, the next step header at the
    same or shallower indentation, or EOF). This is the SF2 fix (round-3
    review): macos_build.yml runs TWO legs (macos-arm64, macos-x86_64) out of
    the SAME workflow file via a matrix, each asserting a different vector
    for the same token (e.g. avif:encode=1 vs avif:encode=0) in different
    steps gated by `if: matrix.cross`. Without a discriminator, a whole-file
    scan sees BOTH legs' tokens as one set, and a leg's correct assertion
    collides with the other leg's honest, differently-valued assertion --
    `check()` would report a false disagreement for both legs. A leg with no
    `step_anchor` keeps the original whole-file scan (every other leg has an
    exclusive workflow file, so there is nothing to disambiguate).
    """
    text = pathlib.Path(path).read_text()
    if step_anchor is not None:
        headers = list(_STEP_HEADER_RE.finditer(text))
        start = None
        indent = ""
        start_idx = -1
        for i, h in enumerate(headers):
            if step_anchor in h.group(0):
                start = h.start()
                indent = h.group(1)
                start_idx = i
                break
        if start is None:
            return set()
        end = len(text)
        for h in headers[start_idx + 1:]:
            if len(h.group(1)) <= len(indent):
                end = h.start()
                break
        text = text[start:end]
    tokens = set()
    for match in _EXPECT_TOKEN_RE.finditer(text):
        tokens.add(match.group(1))
    return tokens


def check(ledger=None, workflows_dir=None):
    """Compare each leg's own expectation set against tokens found ONLY in
    that leg's own workflow file (per `leg["workflow"]`).

    Returns a list of human-readable disagreement strings (empty == ok).
    Disagreements are two-directional per leg: a token the leg's workflow
    asserts that the leg's ledger entry does not claim, or a token the leg's
    ledger claims that the leg's workflow does not assert.

    This is deliberately PER LEG, not a union of all legs' tokens against a
    union of all workflow files' tokens: a leg-agnostic union lets a leg
    wired with the wrong vector pass silently whenever some OTHER leg
    legitimately asserts the identical token -- in both directions (a leg's
    wrong 0 hides behind a different leg's honest 0; that leg's missing
    correct 1 hides behind a third leg's honest 1). See
    test_check_catches_cross_leg_drift_union_missed in
    native/scripts/tests/test_render_expectations.py for the reproduction.

    Round-1 note: as of this ledger's introduction, no workflow has yet been
    rewired to call codec_capability_probe.py's multi-format vector syntax
    (that call-site replacement is CI-T1's separate, later task), so --check
    stays red against the real tree until that wiring lands.
    """
    legs = ledger if ledger is not None else load_ledger()
    workflows_dir = pathlib.Path(workflows_dir or DEFAULT_WORKFLOWS_DIR)

    disagreements = []
    for leg in sorted(legs):
        table = legs[leg]
        workflow_name = table.get("workflow")
        if not workflow_name:
            disagreements.append(f"leg {leg!r} has no 'workflow' mapping to check against")
            continue
        leg_tokens = {f"{pair}={value}" for pair, value in table["expect"].items()}
        wf_path = workflows_dir / workflow_name
        if not wf_path.exists():
            disagreements.append(
                f"leg {leg!r} declares workflow {workflow_name!r} but that file "
                f"does not exist under {workflows_dir}"
            )
            continue
        wf_tokens = _tokens_in_file(wf_path, step_anchor=table.get("step_anchor"))
        for token in sorted(wf_tokens - leg_tokens):
            disagreements.append(
                f"leg {leg!r} workflow {workflow_name!r} asserts {token!r} but the ledger leg does not claim it"
            )
        for token in sorted(leg_tokens - wf_tokens):
            disagreements.append(
                f"leg {leg!r} ledger claims {token!r} but its workflow {workflow_name!r} does not assert it"
            )
    return disagreements


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--leg", metavar="LEG_ID", help="Print the --expect argv fragment for this leg")
    ap.add_argument("--check", action="store_true",
                    help="Compare the ledger against .github/workflows/*.yml and exit 1 on disagreement")
    ap.add_argument("--ledger", metavar="PATH", default=None, help="Ledger path (default: %(default)s)")
    ap.add_argument("--workflows-dir", metavar="DIR", default=None,
                    help="Workflows directory for --check (default: .github/workflows)")
    args = ap.parse_args(argv)

    if not args.leg and not args.check:
        ap.error("one of --leg or --check is required")

    try:
        legs = load_ledger(args.ledger)
    except (LedgerError, FileNotFoundError, tomllib.TOMLDecodeError) as exc:
        print(f"::error::{exc}", file=sys.stderr)
        return 1

    rc = 0
    if args.leg:
        try:
            fragment = render(args.leg, ledger=legs)
        except LedgerError as exc:
            print(f"::error::{exc}", file=sys.stderr)
            return 1
        print(" ".join(fragment))

    if args.check:
        disagreements = check(ledger=legs, workflows_dir=args.workflows_dir)
        if disagreements:
            for d in disagreements:
                print(f"::error::{d}", file=sys.stderr)
            rc = 1
        else:
            print("ledger and workflow --expect tokens agree")

    return rc


if __name__ == "__main__":
    sys.exit(main())
