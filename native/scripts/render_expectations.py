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
    """Parse and validate the ledger. Returns {leg: {"instrument": str, "expect": {pair: int}}}.

    Every `0` cell must be a table carrying non-empty `reason` and `owner`
    strings, or this raises LedgerError. The returned `expect` dict values
    are plain ints (0 or 1); reason/owner are validation-only metadata, not
    part of the CLI vector.
    """
    path = pathlib.Path(path) if path is not None else DEFAULT_LEDGER_PATH
    with open(path, "rb") as f:
        raw = tomllib.load(f)

    legs = {}
    for leg, table in raw.items():
        if "instrument" not in table:
            raise LedgerError(f"leg {leg!r} is missing required key 'instrument'")
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
        legs[leg] = {"instrument": table["instrument"], "expect": expect}
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


def _tokens_in_workflows(workflows_dir):
    """Return {leg-agnostic token-set} of every FMT:DIR=VAL token found across all workflow files.

    Round-1 note: as of this ledger's introduction, no workflow has yet been
    rewired to call codec_capability_probe.py's multi-format vector syntax
    (that call-site replacement is CI-T1's separate, later task); this scan
    simply reports what it finds today.
    """
    tokens = set()
    for wf in sorted(pathlib.Path(workflows_dir).glob("*.yml")):
        text = wf.read_text()
        for match in _EXPECT_TOKEN_RE.finditer(text):
            tokens.add(match.group(1))
    return tokens


def check(ledger=None, workflows_dir=None):
    """Compare the ledger's full expectation set against tokens found in the workflows.

    Returns a list of human-readable disagreement strings (empty == ok).
    Disagreements are two-directional: a workflow token the ledger's union
    does not contain, or a ledger token no workflow asserts.

    Note: this compares the UNION of all legs' pairs (the ledger does not
    encode which workflow file corresponds to which leg id in a way this
    script can standalone-verify without a leg->file map, so the check
    proves the ledger's overall vocabulary of asserted pairs matches what is
    literally present as --expect tokens in .github/workflows/*.yml).
    """
    legs = ledger if ledger is not None else load_ledger()
    ledger_tokens = set()
    for leg, table in legs.items():
        for pair, value in table["expect"].items():
            ledger_tokens.add(f"{pair}={value}")

    workflow_tokens = _tokens_in_workflows(workflows_dir or DEFAULT_WORKFLOWS_DIR)

    disagreements = []
    for token in sorted(workflow_tokens - ledger_tokens):
        disagreements.append(f"workflow asserts {token!r} but no ledger leg claims it")
    for token in sorted(ledger_tokens - workflow_tokens):
        disagreements.append(f"ledger claims {token!r} but no workflow asserts it")
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
