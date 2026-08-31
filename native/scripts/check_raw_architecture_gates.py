#!/usr/bin/env python3
"""Mechanises the spec section 13.3 code-review grep gate.

FROZEN TRANSCRIPTION: this is a behavior-preserving port of
native/scripts/check_raw_architecture_gates.sh (deleted in the same commit
that introduced this file). Last .sh commit before deletion:
07596ef604badbc2037342d078d60242c511f2e4

Do not "improve" the rules here without also updating the transcription
tests in native/scripts/tests/test_check_raw_architecture_gates.py - every
rule below has a documented false-positive incident (see comments) that
shaped its exact pattern.

Scope: production project source only. The vendor subtree is excluded on
purpose - LibRaw's own RawSpeed glue is exactly where rawspeed3_* SHOULD
appear; the rule is that PROJECT source must not.
"""
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
NATIVE_DIR = SCRIPT_DIR.parent
SCOPE = [NATIVE_DIR / "src", NATIVE_DIR / "include", NATIVE_DIR / "tests"]
INCLUDE_SUFFIXES = {".cpp", ".h", ".hpp"}
COMMENT_LINE_RE = re.compile(r"^\s*(//|\*)")


def _iter_scope_files(scope=None):
    """Yield (path, lineno, text) for every non-third_party source line."""
    for root in (scope if scope is not None else SCOPE):
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.suffix not in INCLUDE_SUFFIXES:
                continue
            if "third_party" in path.parts:
                continue
            try:
                text = path.read_text(errors="replace")
            except OSError:
                continue
            for lineno, line in enumerate(text.splitlines(), start=1):
                yield path, lineno, line


def scan(pattern, scope=None):
    """Equivalent of the .sh's scan(): regex search over scope files,
    excluding third_party. Does NOT strip comment lines (see module
    docstring / rule1 note below) - callers that need comment exclusion
    do it themselves (rule4)."""
    regex = re.compile(pattern)
    hits = []
    for path, lineno, line in _iter_scope_files(scope):
        if regex.search(line):
            hits.append(f"{path}:{lineno}:{line}")
    return hits


def forbid(rule, pattern, description, scope=None):
    hits = scan(pattern, scope)
    count = len(hits)
    lines = []
    if count != 0:
        lines.append(f"[ArchGate] FAIL {rule} ({description}): {count} occurrence(s)")
        for hit in hits:
            lines.append(f"    {hit}")
        return False, lines
    lines.append(f"[ArchGate] {rule} -> PASS ({description}: 0 occurrences)")
    return True, lines


def require_count(rule, pattern, want, description, scope=None):
    count = len(scan(pattern, scope))
    if count < want:
        return False, [f"[ArchGate] FAIL {rule} ({description}): found {count}, need >= {want}"]
    return True, [f"[ArchGate] {rule} -> PASS ({description}: {count})"]


# --- forbidden architecture (spec section 13.3) ---------------------------

def check_rule1(scope=None):
    # NOTE: this intentionally does NOT strip comment lines. Step 2's
    # red-proof mutation ("// rawspeed3_init") is itself a comment and must
    # still trip rule1 - the spec's "zero matches" requirement for the
    # forbidden-call rules is literal, including mentions in comments.
    # Per-rule false positives from legitimate documentation (round-8,
    # finding F4 recurrence) are fixed by tightening each rule's own
    # pattern below, not by weakening this shared scan.
    return forbid(
        "rule1",
        r"rawspeed3_init|rawspeed3_decodefile|rawspeed3_release|rawspeed3_close",
        "no direct RawSpeed3 C API calls",
        scope,
    )


def check_rule2(scope=None):
    return forbid(
        "rule2",
        r'#include *[<"]rawspeed|rawspeed3_capi\.h',
        "no RawSpeed headers in project source",
        scope,
    )


def check_rule3(scope=None):
    # Deviation from the plan's literal pattern (round-8, finding F4
    # recurrence): bare-name matching trips on documentation that CITES the
    # forbidden API as a file:line reference (e.g. "raw2image.cpp:144-148")
    # to explain why the frontend must not call it - not an actual call.
    # Requiring call syntax (name immediately followed by '(') still catches
    # a real invocation while leaving such citations alone.
    return forbid(
        "rule3",
        r"\b(raw2image|dcraw_process|dcraw_ppm_tiff_writer|dcraw_make_mem_image)\s*\(",
        "no LibRaw CPU render API",
        scope,
    )


def check_rule5(scope=None):
    return forbid(
        "rule5",
        r"RawSpeedFrontend|RawSpeedAdapter|decoder_registry|DecoderFactory|decoder_plugin",
        "no parallel frontend, registry or plugin framework",
        scope,
    )


def check_rule4(native_dir=None):
    # rule4 is scoped to the shared GPU API surface only.
    native_dir = native_dir or NATIVE_DIR
    gpu_api = [
        native_dir / "include" / "raw_pipeline_contract.h",
        native_dir / "include" / "raw_contract_validate.h",
        native_dir / "include" / "raw_gpu_pipeline.h",
        native_dir / "include" / "dng_render_params.h",
    ]
    rule4_hits = 0
    lines = []
    pattern = re.compile(r"\bLibRaw\b|\blibraw_[a-z_]+_t\b|\brawspeed\b")
    for header in gpu_api:
        if not header.is_file():
            # P3 (round-8 parking lot): a missing GPU-API header is a
            # coverage regression, not a skip - silently continuing would
            # let rule4's surface shrink without anyone noticing. Fail
            # loudly with the path.
            lines.append(f"[ArchGate] FAIL rule4 (missing GPU API header): {header}")
            rule4_hits += 1
            continue
        # Deviation from the plan's literal pattern (round-8, finding F4
        # recurrence): 'LibRaw[^F]|libraw_data_t|rawspeed' substring-matched
        # the contract's own backend-identity vocabulary (enum names like
        # kRawFrontendLibRaw, fields like rawspeed_flags, string literals
        # like "rawspeed3") which this header is REQUIRED to carry for
        # rule8 observability - not an actual LibRaw/RawSpeed decoder type
        # leaking in. Tightened to whole-word matches plus comment-line
        # exclusion so only real type/API references trip the gate.
        # N2 (round-8 nit): the lowercase LibRaw typedef family
        # (libraw_data_t, libraw_output_params_t, libraw_processed_image_t,
        # ...) is matched generically instead of enumerating each struct
        # name.
        # N1 (round-8 nit): the earlier third pipeline stage
        # ('grep -v RawForcedBackend') is removed - the comment-line
        # exclusion above already removes all three real hits in the
        # current tree, so the extra stage was dead and would have
        # silently hidden a genuine future leak that happened to share a
        # line with RawForcedBackend.
        try:
            text = header.read_text(errors="replace")
        except OSError:
            continue
        header_hit = False
        for line in text.splitlines():
            if COMMENT_LINE_RE.match(line):
                continue
            if pattern.search(line):
                header_hit = True
        if header_hit:
            rule4_hits += 1

    if rule4_hits != 0:
        lines.append("[ArchGate] FAIL rule4 (no decoder type in the shared GPU API surface)")
        return False, lines
    lines.append("[ArchGate] rule4 -> PASS (no decoder type in the shared GPU API surface)")
    return True, lines


# --- required implementation (spec section 13.3, second block) ------------

def check_rule6(scope=None):
    return require_count("rule6", r"class LibRawFrontendContext", 1,
                          "one LibRaw-owned generic frontend", scope)


def check_rule7(scope=None):
    return require_count("rule7", r"class LibRawGpuInputAdapter", 1,
                          "one LibRaw rawdata/metadata -> RawGpuInput adapter", scope)


def check_rule8(scope=None):
    return require_count("rule8", r"LIBRAW_WARN_RAWSPEED3_PROCESSED", 1,
                          "RawSpeed3/native fallback observability", scope)


def check_rule9(scope=None):
    return require_count("rule9", r"bool runRenderStage4HalideAot", 2,
                          "one shared Stage4 core (plain + device forms)", scope)


def run_all_gates(native_dir=None, scope=None):
    """Run every rule and return (failures_count, output_lines)."""
    failures = 0
    output = []
    for check in (check_rule1, check_rule2, check_rule3, check_rule5):
        ok, lines = check(scope)
        output.extend(lines)
        if not ok:
            failures += 1

    ok, lines = check_rule4(native_dir)
    output.extend(lines)
    if not ok:
        failures += 1

    for check in (check_rule6, check_rule7, check_rule8, check_rule9):
        ok, lines = check(scope)
        output.extend(lines)
        if not ok:
            failures += 1

    return failures, output


def main():
    failures, output = run_all_gates()
    for line in output:
        print(line)
    if failures != 0:
        print(f"[ArchGate] FAIL ({failures} rules)")
        return 1
    print("[ArchGate] ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
