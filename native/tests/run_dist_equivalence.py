#!/usr/bin/env python3
"""D6 equivalence gate: committed (shell-built) HEIF dist vs carrier-built dist.

Spec: docs/logs/2026-08-30/Spec_build_rewrite.md section 6 (three layers);
Plan_build_rewrite.md D6 (A6.1-A6.4); round-5 contract AC1
(docs/logs/2026-08-31/round-5-contract.md).

=========================================================================
PRE-REGISTERED CRITERIA -- read before running, do not tune afterwards
=========================================================================
Everything below is the comparison rule and its expected value. It is
written here BEFORE the first run so that a green result cannot be
manufactured by adjusting a threshold once the numbers exist (ledger
2026-08-23). Tightening a check later is allowed; loosening one to reach
green is a contract violation.

FROZEN CONSTRAINT (spec section 6.2, plan A6.4): equivalence is judged BY
CAPABILITY. Byte-identity comparison of build outputs is explicitly
FORBIDDEN and this script performs none -- compiler nondeterminism,
embedded timestamps and embedded absolute paths make it a false
instrument. (Byte comparison IS used in layer 1, where the compared
objects are rendered argv strings, not build outputs: there is no
nondeterminism to normalise away, so exact equality is the correct and
strongest criterion there.)

LAYER 1 -- argv equivalence (pure, runs on any host, covers Windows)
  L1A  For every (component, platform, arch) combo below, deps.render.render()
       must equal the committed golden argv file EXACTLY (list equality,
       element for element, order included).
       Expected: 22 combos, 22 PASS.
  L1B  Transcription check against the LEGACY SHELL SCRIPT that is still in
       the tree (native/scripts/fetch_heif_deps.sh). L1A alone is
       self-consistency -- golden files and renderer are both ours -- so it
       cannot detect a mis-transcription made when the goldens were first
       written. L1B compares the rendered argv against the flags literally
       present in the shell script that produced the committed dist.
       Criterion, per component:
         (a) KEY SET EQUALITY: the set of -D keys rendered for macos/arm64
             equals the set of -D keys in the shell script's configure
             block. A key present on one side only is a FAIL.
         (b) VALUE EQUALITY for every key whose shell-script value contains
             no shell expansion ("${...}") and which the manifest does NOT
             declare as a path key. These are the load-bearing ON/OFF
             capability switches; an inverted one is exactly the migration
             defect this layer exists to catch.
         (c) PRESENCE ONLY for keys whose shell value is a shell expansion
             or which the manifest declares in path_keys/path_list_keys.
             NORMALISATION NAMED AND JUSTIFIED: their values are install-
             prefix dependent (${DIST}, ${STAGE}, ${KVAZAAR_LIB}) and the
             renderer substitutes its own symbolic prefix, so the values
             cannot be equal by construction and equality would be a
             meaningless assertion. Nothing else is normalised.
       Expected: kvazaar PASS, libheif PASS.
  L1 SCOPE LIMITS (spec section 6.1) -- declared, not silently absent:
       - libde265 and aom on macOS/Linux: acquired from the vcpkg registry;
         we author no argv for them, so there is nothing to diff against
         the shell script. SKIP, printed explicitly.
       - Windows: build_heif_dist_windows.sh was reduced to a delegating
         shim in round 4, so it no longer contains argv to transcribe.
         SKIP, printed explicitly. (Windows argv is still covered by L1A,
         which is the point of a pure renderer.)

LAYER 2 -- capability equivalence (needs both dists on this host)
  Runs the SAME assertion set against the baseline dist and the carrier
  dist and requires an IDENTICAL VERDICT VECTOR. Not "both green": both
  must return the same verdict for every assertion, including any
  expected-red one (spec section 6.2 / A6.2).
  Assertions (all read the artefact, all reused from deps.heif so this
  gate cannot drift from the builder's own checks):
      A-SYM-<name>   each of deps.heif._REQUIRED_HEIF_SYMBOLS present
      A-NOSYM-<name> each of deps.heif._FORBIDDEN_HEIF_SYMBOLS absent
      A-DEPS-DE265   libheif records a libde265 runtime dependency
      A-ARCH-<lib>   each shipped library carries the requested arch
  Expected: every assertion returns the same verdict on both dists, and
  the baseline vector is the reference. A verdict pair that differs is a
  FAIL naming the assertion.
  A dist that is missing (not built) is a SKIP, never a PASS, and the
  overall exit code becomes INCOMPLETE (2), never 0.

LAYER 3 -- consumer equivalence (needs a decoder linked to the carrier dist)
  Runs the project's existing consumer gate and requires PER-CASE
  EXECUTION LINES, not merely the word "harness" (ledger 2026-08-25: a
  silently skipped gate produces a PASS count identical to a full run).
  Criterion: the command exits 0 AND its output contains at least one
  line matching each required marker regex declared in _L3_REQUIRED_MARKERS.
  A missing binary is a SKIP with a printed one-line declaration, never a
  PASS.

EXIT CODES (self-captured by the caller; this script also writes its own
exit code as the last line of the report):
  0  every requested layer PASSED
  1  at least one layer FAILED
  2  INCOMPLETE -- at least one layer was skipped (missing inputs). A
     skipped layer can never produce exit 0, so a partial run is
     mechanically distinguishable from a full one.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Optional

_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "native" / "scripts"))

from deps import heif as heif_mod  # noqa: E402
from deps import manifest as manifest_mod  # noqa: E402
from deps import render as render_mod  # noqa: E402

_GOLDEN_DIR = _REPO_ROOT / "native" / "scripts" / "deps" / "tests" / "golden"
_LEGACY_UNIX_SCRIPT = _REPO_ROOT / "native" / "scripts" / "fetch_heif_deps.sh"
_DEFAULT_BASELINE = _REPO_ROOT / "native" / "third_party" / "heif-dist"

# --- Layer 1 pre-registered combo table (mirrors the manifest's renderable
# set; a combo missing a golden file is a FAIL, not a skip).
_ALL_PLATFORM_ARCH = (
    ("macos", "arm64"),
    ("macos", "x86_64"),
    ("linux", "x86_64"),
    ("windows", "x86_64"),
)
_MACOS_LINUX_ONLY = (("macos", "arm64"), ("macos", "x86_64"), ("linux", "x86_64"))
_RENDERABLE = ("kvazaar", "libde265", "aom", "libheif", "libwebp", "libjxl")
_PLATFORM_ARCH_BY_COMPONENT = {"libwebp": _MACOS_LINUX_ONLY, "libjxl": _MACOS_LINUX_ONLY}
_L1A_EXPECTED_COMBOS = 22

# --- Layer 1B: where each component's configure block lives in the legacy
# shell script. The marker is the literal `cmake -S ...` line that opens the
# block; the block ends at the first line that starts a new cmake command.
_L1B_COMPONENTS = {
    "kvazaar": "build-kvazaar",
    "libheif": "build-heif",
}
_L1B_SKIPPED = (
    ("libde265", "macos/linux", "vcpkg registry component -- we author no argv (spec 6.1 scope limit)"),
    ("aom", "macos/linux", "vcpkg registry component -- we author no argv (spec 6.1 scope limit)"),
    ("heif-stack", "windows", "build_heif_dist_windows.sh is a round-4 delegating shim -- no argv to transcribe"),
)

# --- Layer 3 required per-case execution markers.
_L3_REQUIRED_MARKERS = (
    ("decode-matrix-case", re.compile(r"^\s*\[(?:Contract|PSNR GATE)\]")),
    ("ffi-harness-case", re.compile(r"^\[FFI (?:run \d+|RGB MATCH)\]")),
    ("device-handoff-case", re.compile(r"device handoff|\[Handoff", re.IGNORECASE)),
)


class Verdict:
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"


class Report:
    """Accumulates per-check verdicts and renders the artefact."""

    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str, str]] = []

    def record(self, layer: str, check: str, verdict: str, detail: str = "") -> None:
        self.rows.append((layer, check, verdict, detail))
        print(f"[{layer}] {verdict:<4} {check}" + (f" -- {detail}" if detail else ""))

    def counts(self, layer: str) -> dict[str, int]:
        out = {Verdict.PASS: 0, Verdict.FAIL: 0, Verdict.SKIP: 0}
        for row_layer, _, verdict, _ in self.rows:
            if row_layer == layer:
                out[verdict] += 1
        return out

    def layer_verdict(self, layer: str) -> str:
        c = self.counts(layer)
        if c[Verdict.FAIL]:
            return Verdict.FAIL
        if c[Verdict.PASS] == 0 or c[Verdict.SKIP]:
            return Verdict.SKIP if c[Verdict.PASS] == 0 else "PARTIAL"
        return Verdict.PASS

    def render(self, header_lines: list[str], exit_code: int) -> str:
        lines = list(header_lines)
        lines.append("")
        lines.append("| Layer | Check | Verdict | Detail |")
        lines.append("|---|---|---|---|")
        for layer, check, verdict, detail in self.rows:
            lines.append(f"| {layer} | {check} | {verdict} | {detail.replace('|', '/')} |")
        lines.append("")
        for layer in ("L1A", "L1B", "L2", "L3"):
            c = self.counts(layer)
            if sum(c.values()) == 0:
                continue
            lines.append(
                f"- {layer}: PASS={c[Verdict.PASS]} FAIL={c[Verdict.FAIL]} SKIP={c[Verdict.SKIP]}"
            )
        lines.append("")
        lines.append(f"GATE_EXIT_CODE={exit_code}")
        return "\n".join(lines) + "\n"


# =========================================================================
# Layer 1
# =========================================================================
def layer1a(report: Report, loaded: dict[str, Any]) -> None:
    combos = [
        (comp, platform, arch)
        for comp in _RENDERABLE
        for platform, arch in _PLATFORM_ARCH_BY_COMPONENT.get(comp, _ALL_PLATFORM_ARCH)
    ]
    if len(combos) != _L1A_EXPECTED_COMBOS:
        report.record(
            "L1A",
            "combo-count",
            Verdict.FAIL,
            f"expected {_L1A_EXPECTED_COMBOS} pre-registered combos, computed {len(combos)}",
        )
        return
    report.record("L1A", "combo-count", Verdict.PASS, f"{len(combos)} combos")
    for comp, platform, arch in combos:
        name = f"{comp}.{platform}.{arch}"
        golden = _GOLDEN_DIR / f"{name}.argv"
        if not golden.is_file():
            report.record("L1A", name, Verdict.FAIL, f"missing golden file {golden}")
            continue
        expected = golden.read_text(encoding="utf-8").splitlines()
        try:
            actual = render_mod.render(loaded, comp, platform, arch)
        except render_mod.RenderError as exc:
            report.record("L1A", name, Verdict.FAIL, f"render error: {exc}")
            continue
        if actual == expected:
            report.record("L1A", name, Verdict.PASS, f"{len(actual)} argv elements identical")
        else:
            only_golden = [x for x in expected if x not in actual]
            only_render = [x for x in actual if x not in expected]
            report.record(
                "L1A",
                name,
                Verdict.FAIL,
                f"differs; only-in-golden={only_golden[:5]} only-in-rendered={only_render[:5]}",
            )


_D_FLAG_RE = re.compile(r'^-D([A-Za-z_][A-Za-z0-9_]*)=(.*)$')
_SHELL_EXPANSION_RE = re.compile(r'\$\{[^}]+\}|\$[A-Za-z_]')


def _shell_configure_flags(
    script_text: str, build_dir_marker: str
) -> Optional[tuple[dict[str, str], list[str]]]:
    """Extract the `-DKEY=VALUE` flags of the configure block whose `cmake -S`
    line mentions `build_dir_marker`, PLUS the names of every bash array the
    block expands (`"${NAME[@]}"`).

    Returning the expanded array names rather than hard-coding them per
    component is deliberate: the legacy script keeps most of its flags in
    shared arrays, and a hard-coded list silently degrades to "compare the
    handful of inline flags" if the script is reorganised -- which reads as
    a green key-set check over a nearly empty set.
    """
    lines = script_text.splitlines()
    start = None
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("cmake -S") and build_dir_marker in stripped:
            start = i
            break
    if start is None:
        return None
    flags: dict[str, str] = {}
    arrays: list[str] = []
    for line in lines[start:]:
        stripped = line.strip()
        if stripped.startswith("cmake --build") or stripped.startswith("cmake --install"):
            break
        if stripped.startswith("#"):
            continue
        arrays.extend(_ARRAY_EXPANSION_RE.findall(stripped))
        for token in re.findall(r'-D[A-Za-z_][A-Za-z0-9_]*=(?:"[^"]*"|\S*)', stripped):
            m = _D_FLAG_RE.match(token.rstrip("\\").strip())
            if m:
                flags[m.group(1)] = m.group(2).strip().strip('"')
    return flags, arrays


_ARRAY_EXPANSION_RE = re.compile(r'"\$\{([A-Za-z_][A-Za-z0-9_]*)\[@\]\}"')


def _flag_array_flags(script_text: str, array_name: str) -> dict[str, str]:
    """Extract `-DKEY=VALUE` entries from EVERY bash assignment or append to
    `array_name` (`NAME=(`, `NAME+=(`), in source order.

    Both forms matter and both are load-bearing here: the legacy script
    declares the shared flags once and then APPENDS the macOS-only
    `CMAKE_OSX_*` pair inside a `HOST_OS = Darwin` branch. Reading only the
    `NAME=(` form would compare a macOS render against a
    platform-independent subset of the shell baseline and manufacture a
    false red on exactly the arch flags this gate exists to check.
    """
    flags: dict[str, str] = {}
    for m in re.finditer(rf'^\s*{re.escape(array_name)}\+?=\(', script_text, re.M):
        for line in script_text[m.end():].splitlines():
            if line.strip().startswith(")"):
                break
            for token in re.findall(r'-D[A-Za-z_][A-Za-z0-9_]*=(?:"[^"]*"|\S*)', line):
                fm = _D_FLAG_RE.match(token.strip())
                if fm:
                    flags[fm.group(1)] = fm.group(2).strip().strip('"')
            if line.rstrip().endswith(")"):
                break  # single-line append form: NAME+=("-DX=Y")
    return flags


def layer1b(report: Report, loaded: dict[str, Any], legacy_script: Optional[Path] = None) -> None:
    _LEGACY = Path(legacy_script) if legacy_script else _LEGACY_UNIX_SCRIPT
    for comp, scope, why in _L1B_SKIPPED:
        report.record("L1B", f"{comp}/{scope}", Verdict.SKIP, why)

    if not _LEGACY.is_file():
        report.record(
            "L1B",
            "legacy-script",
            Verdict.SKIP,
            f"{_LEGACY} absent (deleted by D9?) -- transcription check "
            f"is only available while the legacy script is still in the tree",
        )
        return
    text = _LEGACY.read_text(encoding="utf-8")

    for comp, marker in _L1B_COMPONENTS.items():
        extracted = _shell_configure_flags(text, marker)
        if extracted is None:
            report.record("L1B", comp, Verdict.FAIL, f"no `cmake -S` block mentioning {marker!r}")
            continue
        shell_flags, expanded_arrays = extracted
        # Fold in every bash array the configure line expands (including its
        # macOS-only `+=` appends), so the key sets are comparable at all.
        for array_name in expanded_arrays:
            shell_flags.update(_flag_array_flags(text, array_name))
        if not expanded_arrays:
            report.record(
                "L1B",
                f"{comp}/baseline-shape",
                Verdict.FAIL,
                "the legacy configure block expands no flag array -- the extractor "
                "is probably reading the wrong block; refusing to compare against it",
            )
            continue
        rendered = render_mod.render(loaded, comp, "macos", "arm64")
        rendered_flags: dict[str, str] = {}
        for entry in rendered:
            m = _D_FLAG_RE.match(entry)
            if m:
                rendered_flags[m.group(1)] = m.group(2)

        comp_decl = loaded["manifest"]["component"][comp]
        path_keys = set(comp_decl.get("path_keys", [])) | set(comp_decl.get("path_list_keys", []))

        missing = sorted(set(shell_flags) - set(rendered_flags))
        extra = sorted(set(rendered_flags) - set(shell_flags))
        if missing or extra:
            report.record(
                "L1B",
                f"{comp}/key-set",
                Verdict.FAIL,
                f"in-shell-not-rendered={missing} rendered-not-in-shell={extra}",
            )
        else:
            report.record(
                "L1B", f"{comp}/key-set", Verdict.PASS, f"{len(shell_flags)} keys set-equal"
            )

        compared = 0
        mismatches: list[str] = []
        presence_only: list[str] = []
        for key, shell_value in sorted(shell_flags.items()):
            if key not in rendered_flags:
                continue
            if _SHELL_EXPANSION_RE.search(shell_value) or key in path_keys:
                presence_only.append(key)
                continue
            compared += 1
            if rendered_flags[key] != shell_value:
                mismatches.append(f"{key}: shell={shell_value!r} rendered={rendered_flags[key]!r}")
        if mismatches:
            report.record("L1B", f"{comp}/values", Verdict.FAIL, "; ".join(mismatches))
        elif compared == 0:
            report.record(
                "L1B",
                f"{comp}/values",
                Verdict.FAIL,
                "zero literal-valued flags compared -- the value check would be vacuous",
            )
        else:
            report.record(
                "L1B",
                f"{comp}/values",
                Verdict.PASS,
                f"{compared} literal values identical; {len(presence_only)} "
                f"prefix-dependent keys presence-only: {presence_only}",
            )


# =========================================================================
# Layer 2
# =========================================================================
def _capability_vector(platform: str, arch: str, dist: Path) -> dict[str, str]:
    """Verdict per assertion id for one dist. Never raises: a missing artefact
    yields ABSENT verdicts so the two dists stay comparable."""
    t = heif_mod.traits(platform)
    heif_lib = dist / t["heif_lib"]
    de265_lib = dist / t["de265_lib"]
    vector: dict[str, str] = {}

    if not heif_lib.is_file():
        for symbol, _ in heif_mod._REQUIRED_HEIF_SYMBOLS:
            vector[f"A-SYM-{symbol}"] = "ABSENT-ARTEFACT"
        for symbol, _ in heif_mod._FORBIDDEN_HEIF_SYMBOLS:
            vector[f"A-NOSYM-{symbol}"] = "ABSENT-ARTEFACT"
        vector["A-DEPS-DE265"] = "ABSENT-ARTEFACT"
        vector["A-ARCH-libheif"] = "ABSENT-ARTEFACT"
        vector["A-ARCH-libde265"] = "ABSENT-ARTEFACT"
        return vector

    # Capture-then-match, never `nm | grep -q`: under `set -o pipefail` a
    # matching grep closes the pipe, nm takes SIGPIPE and the pipeline
    # reports 141, inverting the verdict (ledger 2026-08-28).
    symbols = heif_mod.read_symbols(platform, heif_lib)
    dependencies = heif_mod.read_dependencies(platform, heif_lib)
    for symbol, _ in heif_mod._REQUIRED_HEIF_SYMBOLS:
        vector[f"A-SYM-{symbol}"] = "PRESENT" if symbol in symbols else "MISSING"
    for symbol, _ in heif_mod._FORBIDDEN_HEIF_SYMBOLS:
        vector[f"A-NOSYM-{symbol}"] = "ABSENT" if symbol not in symbols else "PRESENT"
    vector["A-DEPS-DE265"] = "PRESENT" if "libde265" in dependencies else "MISSING"

    for label, lib in (("libheif", heif_lib), ("libde265", de265_lib)):
        if not lib.is_file():
            vector[f"A-ARCH-{label}"] = "ABSENT-ARTEFACT"
            continue
        try:
            heif_mod.assert_arch(platform, arch, [lib])
            vector[f"A-ARCH-{label}"] = f"ARCH-OK-{arch}"
        except heif_mod.HeifError as exc:
            vector[f"A-ARCH-{label}"] = f"ARCH-BAD ({exc})"
    return vector


def layer2(report: Report, platform: str, arch: str, baseline: Path, carrier: Optional[Path]) -> None:
    if carrier is None:
        report.record(
            "L2",
            "carrier-dist",
            Verdict.SKIP,
            "no --carrier-dist supplied; capability equivalence needs BOTH dists "
            "on this host. This is a SKIP, never a PASS (exit code 2).",
        )
        return
    if not Path(baseline).is_dir():
        report.record("L2", "baseline-dist", Verdict.SKIP, f"{baseline} is not a directory")
        return
    if not Path(carrier).is_dir():
        report.record("L2", "carrier-dist", Verdict.SKIP, f"{carrier} is not a directory")
        return

    base_vec = _capability_vector(platform, arch, Path(baseline))
    carr_vec = _capability_vector(platform, arch, Path(carrier))
    for assertion in sorted(set(base_vec) | set(carr_vec)):
        b = base_vec.get(assertion, "<absent>")
        c = carr_vec.get(assertion, "<absent>")
        if b == "ABSENT-ARTEFACT" or c == "ABSENT-ARTEFACT":
            report.record("L2", assertion, Verdict.SKIP, f"baseline={b} carrier={c}")
        elif b == c:
            report.record("L2", assertion, Verdict.PASS, f"identical verdict: {b}")
        else:
            report.record("L2", assertion, Verdict.FAIL, f"baseline={b} carrier={c}")


# =========================================================================
# Layer 3
# =========================================================================
def layer3(report: Report, command: Optional[list[str]]) -> None:
    if not command:
        report.record(
            "L3",
            "consumer-gate",
            Verdict.SKIP,
            "no --consumer-command supplied; consumer equivalence needs a decoder "
            "linked against the carrier dist. SKIP, never PASS (exit code 2).",
        )
        return
    proc = subprocess.run(command, cwd=_REPO_ROOT, capture_output=True, text=True)
    rc = proc.returncode
    output = proc.stdout + proc.stderr
    log_path = _REPO_ROOT / "native" / "scripts" / "tmp" / "r5-d6-consumer.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(output + f"\nCONSUMER_RC={rc}\n", encoding="utf-8")
    report.record(
        "L3",
        "consumer-exit-code",
        Verdict.PASS if rc == 0 else Verdict.FAIL,
        f"rc={rc}, log={log_path}",
    )
    lines = output.splitlines()
    for name, pattern in _L3_REQUIRED_MARKERS:
        hits = [ln for ln in lines if pattern.search(ln)]
        if hits:
            report.record("L3", name, Verdict.PASS, f"{len(hits)} per-case execution lines")
        else:
            report.record(
                "L3",
                name,
                Verdict.FAIL,
                "no per-case execution line matched -- a silently skipped case "
                "produces a PASS count identical to a full run (ledger 2026-08-25)",
            )


# =========================================================================
def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--platform", default="macos", choices=("macos", "linux"))
    parser.add_argument("--arch", default=heif_mod.host_arch())
    parser.add_argument("--baseline-dist", default=str(_DEFAULT_BASELINE))
    parser.add_argument("--carrier-dist", default=None)
    parser.add_argument(
        "--consumer-command",
        default=None,
        help="command (shell-free argv, space separated) for layer 3, "
        "e.g. 'python3 native/tests/run_decode_matrix.py'",
    )
    parser.add_argument(
        "--legacy-script",
        default=None,
        help="override the L1B shell baseline (used to demonstrate the check red "
        "against a deliberately mutated copy; never used for the real verdict)",
    )
    parser.add_argument("--report", default=None, help="write the artefact here")
    args = parser.parse_args(argv)

    loaded = manifest_mod.load()
    report = Report()

    layer1a(report, loaded)
    layer1b(report, loaded, Path(args.legacy_script) if args.legacy_script else None)
    layer2(
        report,
        args.platform,
        args.arch,
        Path(args.baseline_dist),
        Path(args.carrier_dist) if args.carrier_dist else None,
    )
    layer3(report, args.consumer_command.split() if args.consumer_command else None)

    failed = any(v == Verdict.FAIL for _, _, v, _ in report.rows)
    # A skipped LAYER (not merely a declared in-layer scope skip) forces
    # INCOMPLETE. L1B's scope skips are pre-registered and do not count.
    skipped_layers = [
        layer
        for layer in ("L1A", "L2", "L3")
        if report.counts(layer)[Verdict.SKIP] > 0 or report.counts(layer)[Verdict.PASS] == 0
    ]
    exit_code = 1 if failed else (2 if skipped_layers else 0)

    header = [
        "# D6 equivalence gate report",
        "",
        f"- host platform/arch: {args.platform}/{args.arch}",
        f"- baseline dist: {args.baseline_dist}",
        f"- carrier dist: {args.carrier_dist or '<not supplied>'}",
        f"- consumer command: {args.consumer_command or '<not supplied>'}",
        f"- skipped layers: {skipped_layers or 'none'}",
        "- criteria: pre-registered in this script's module docstring "
        "(native/tests/run_dist_equivalence.py); no byte-identity comparison of "
        "build outputs is performed (spec 6.2 / A6.4).",
    ]
    text = report.render(header, exit_code)
    if args.report:
        Path(args.report).parent.mkdir(parents=True, exist_ok=True)
        Path(args.report).write_text(text, encoding="utf-8")
        print(f"[gate] report written to {args.report}")
    print(f"[gate] exit code {exit_code} "
          f"(0=all layers pass, 1=a check failed, 2=incomplete/skipped)")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
