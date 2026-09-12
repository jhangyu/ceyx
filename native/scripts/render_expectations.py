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

# WI-5: matches the tokens codec_capability_probe.py's --expect-cap flag
# accepts: <NAME>=<0|1>, e.g. "ICC=0". Restricted to the KNOWN capability
# names (not a bare "any uppercase identifier" pattern) -- workflow files are
# full of unrelated uppercase shell VAR=VAL assignments (RC=0, RC=1, and
# other steps' own env vars), and a generic pattern collides with all of
# them the moment a leg's capability step has no unique step_anchor to
# scope the scan to.
_EXPECT_CAP_TOKEN_RE = re.compile(r"\b(ICC|OPENMP|HEIF|WEBP|JXL|RAW)=([01])\b")


class LedgerError(Exception):
    """The ledger file is malformed or violates the reason/owner rule."""


def _load_expect_table(leg, table, key):
    """Shared validator for both `[<leg>.expect]` and `[<leg>.capabilities]`.

    Same schema rules for both (codec_expectations.toml's header, WI-5 plan
    step 5.3): a bare 1 needs no annotation, every 0 must be a table with
    non-empty `reason` and `owner`, rejected at load time otherwise.
    """
    raw = table.get(key, {})
    parsed = {}
    for pair, entry in raw.items():
        if isinstance(entry, bool):
            raise LedgerError(f"leg {leg!r} {key} {pair!r}: boolean is not a valid expectation")
        if isinstance(entry, int):
            if entry == 1:
                parsed[pair] = 1
            elif entry == 0:
                raise LedgerError(
                    f"leg {leg!r} {key} {pair!r}: a bare 0 is rejected -- every 0 "
                    f"must carry a sibling reason and owner, e.g. "
                    f'{{ value = 0, reason = "...", owner = "..." }}'
                )
            else:
                raise LedgerError(f"leg {leg!r} {key} {pair!r}: value must be 0 or 1, got {entry}")
        elif isinstance(entry, dict):
            value = entry.get("value")
            if value != 0:
                raise LedgerError(
                    f"leg {leg!r} {key} {pair!r}: table form is only valid for value = 0, "
                    f"got value = {value!r}"
                )
            reason = entry.get("reason")
            owner = entry.get("owner")
            if not reason or not isinstance(reason, str):
                raise LedgerError(f"leg {leg!r} {key} {pair!r}: 0 entry missing non-empty 'reason'")
            if not owner or not isinstance(owner, str):
                raise LedgerError(f"leg {leg!r} {key} {pair!r}: 0 entry missing non-empty 'owner'")
            parsed[pair] = 0
        else:
            raise LedgerError(f"leg {leg!r} {key} {pair!r}: unrecognised entry type {type(entry)}")
    return parsed


def load_ledger(path=None):
    """Parse and validate the ledger. Returns {leg: {"instrument": str, "workflow": str, "expect": {pair: int}, "capabilities": {name: int}}}.

    Every `0` cell must be a table carrying non-empty `reason` and `owner`
    strings, or this raises LedgerError. The returned `expect`/`capabilities`
    dict values are plain ints (0 or 1); reason/owner are validation-only
    metadata, not part of the CLI vector.

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
        expect = _load_expect_table(leg, table, "expect")
        capabilities = _load_expect_table(leg, table, "capabilities")
        legs[leg] = {
            "instrument": table["instrument"],
            "workflow": table["workflow"],
            "step_anchor": table.get("step_anchor"),
            "capabilities_step_anchor": table.get("capabilities_step_anchor", table.get("step_anchor")),
            "expect": expect,
            "capabilities": capabilities,
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


def render_capabilities(leg, ledger=None):
    """Return the sorted list of '--expect-cap', '<name>=<val>' argv fragments for `leg` (WI-5)."""
    legs = ledger if ledger is not None else load_ledger()
    if leg not in legs:
        raise LedgerError(f"unknown leg {leg!r}; known legs: {', '.join(sorted(legs))}")
    fragment = []
    for name in sorted(legs[leg]["capabilities"]):
        value = legs[leg]["capabilities"][name]
        fragment.append("--expect-cap")
        fragment.append(f"{name}={value}")
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


def _capability_tokens_in_file(path, step_anchor=None):
    """Return the NAME=VAL build-capability token set found in a single
    workflow file (WI-5). Same step_anchor slicing as _tokens_in_file --
    a leg with no step_anchor gets a whole-file scan.
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
    for match in _EXPECT_CAP_TOKEN_RE.finditer(text):
        tokens.add(match.group(0))
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
        # Legs whose `instrument` starts with "none" (e.g. android-arm64-v8a's
        # "none (accepted gap)") have NO runnable capability instrument in CI
        # at all -- there is nothing for their workflow to assert, by design,
        # so a whole-leg comparison against workflow tokens would always be
        # red. This is a deliberate, printed SKIP (never silent, per the
        # 2026-08-25 lesson: a silently-skipped gate looks identical to a
        # fully-checked green one), not an exemption from the ledger's
        # reason/owner discipline -- load_ledger() still requires every 0
        # cell to carry both.
        instrument = table.get("instrument", "")
        if instrument.startswith("none"):
            print(
                f"::notice::SKIP leg {leg!r}: instrument={instrument!r} has no runnable "
                f"CI capability probe, so its workflow is not checked against the ledger."
            )
            continue
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

        # WI-5 (S-E2): same per-leg, two-directional comparison for the
        # build-capability vector. Uses `capabilities_step_anchor`, NOT the
        # codec `step_anchor` -- the macos legs' capability steps' names
        # also contain the "native leg"/"cross leg" substrings (so the two
        # steps read side by side, plan step 5.4), which would otherwise
        # make the codec anchor match the EARLIER codec step's header and
        # clip the scan before ever reaching the capability step's own
        # tokens (the SF2 hazard one level down; see the ledger's
        # capabilities_step_anchor comment). Legs without a dedicated
        # capabilities_step_anchor fall back to step_anchor (or None, i.e.
        # whole-file scan) via load_ledger's default.
        cap_leg_tokens = {f"{name}={value}" for name, value in table.get("capabilities", {}).items()}
        cap_wf_tokens = _capability_tokens_in_file(wf_path, step_anchor=table.get("capabilities_step_anchor"))
        for token in sorted(cap_wf_tokens - cap_leg_tokens):
            disagreements.append(
                f"leg {leg!r} workflow {workflow_name!r} asserts capability {token!r} but the ledger leg does not claim it"
            )
        for token in sorted(cap_leg_tokens - cap_wf_tokens):
            disagreements.append(
                f"leg {leg!r} ledger claims capability {token!r} but its workflow {workflow_name!r} does not assert it"
            )
    return disagreements


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--leg", metavar="LEG_ID", help="Print the --expect argv fragment for this leg")
    ap.add_argument("--capabilities", action="store_true",
                    help="With --leg, print the --expect-cap build-capability argv fragment (WI-5) instead of the codec --expect fragment")
    ap.add_argument("--check", action="store_true",
                    help="Compare the ledger against .github/workflows/*.yml and exit 1 on disagreement")
    ap.add_argument("--ledger", metavar="PATH", default=None, help="Ledger path (default: %(default)s)")
    ap.add_argument("--workflows-dir", metavar="DIR", default=None,
                    help="Workflows directory for --check (default: .github/workflows)")
    args = ap.parse_args(argv)

    if not args.leg and not args.check:
        ap.error("one of --leg or --check is required")
    if args.capabilities and not args.leg:
        ap.error("--capabilities requires --leg")

    try:
        legs = load_ledger(args.ledger)
    except (LedgerError, FileNotFoundError, tomllib.TOMLDecodeError) as exc:
        print(f"::error::{exc}", file=sys.stderr)
        return 1

    rc = 0
    if args.leg:
        try:
            fragment = render_capabilities(args.leg, ledger=legs) if args.capabilities else render(args.leg, ledger=legs)
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
