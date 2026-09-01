#!/usr/bin/env python3
"""Mechanical checker for .github/CI-CONVENTIONS.md.

Spec: docs/logs/2026-08-31/plan-ci-codec-integration.md, Task 2 (CI-T2), DP-6
(closed, option A): this checker is COMMITTED but deliberately NOT wired into
any CI workflow. It runs by hand, from each task's own acceptance criteria and
from CI-T10's sign-off (which must record that it stays unwired). Do not add
a workflow step that invokes this script.

Implements rules C1, C2, C3, C4, C5, C6, C8, C9 from CI-T2 (C7 -- "android_build.yml
contains an emulator job" -- was REMOVED per the 2026-08-31 compile-only
ruling and is not implemented here). C10 (A-T12 option a) guards the
plugin/android jniLibs .gitignore exclusion.

Every rule has a paired negative-control fixture proving it actually detects
a violation (native/scripts/tests/test_ci_conventions_check.py, R8): a rule
with no failing fixture is not merged.

Exit 0 iff every implemented rule passes against the given workflows
directory. One `::error::` line per violation; all violations are reported
before exit, never just the first.
"""
import argparse
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"
ARCH_MAP_PATH = REPO_ROOT / "native" / "deps" / "arch_map.toml"

# CI-CONVENTIONS.md rule 2: names that are neither the canonical
# <component>-<platform>-<arch> shape nor a zero-hyphen diagnostic name, but
# are a documented, deliberate exception. `artifacts-lock` is the run-level
# artifacts.lock carrier (build.yml:556-562); it is explicitly excluded from
# the canonical `*-*-*` download filter by a comment at build.yml:450-451
# because it has exactly one hyphen and no platform token. Recorded here
# rather than silently widening the canonical-name regex.
_KNOWN_NON_CANONICAL_ARTIFACT_NAMES = {"artifacts-lock"}

# CI-CONVENTIONS.md rule 1: allowed workflow filename roles.
_ROLE_PATTERNS = [
    re.compile(r"^build\.yml$"),
    re.compile(r"^[a-z0-9]+_build\.yml$"),
    re.compile(r"^[a-z0-9]+_dist_[a-z0-9]+\.yml$"),
]


def _platform_tokens():
    """Read the platform vocabulary from native/deps/arch_map.toml's [platforms] table."""
    if not ARCH_MAP_PATH.exists():
        return ["macos", "linux", "windows", "android"]
    import tomllib
    with open(ARCH_MAP_PATH, "rb") as f:
        data = tomllib.load(f)
    return data.get("platforms", {}).get("names", ["macos", "linux", "windows", "android"])


def _workflow_files(workflows_dir):
    return sorted(pathlib.Path(workflows_dir).glob("*.yml"))


def check_c1_roles(workflows_dir):
    """Every workflow filename maps to a rule-1 role, or carries `# RATIONALE:` in its first 5 lines."""
    violations = []
    for wf in _workflow_files(workflows_dir):
        if any(p.match(wf.name) for p in _ROLE_PATTERNS):
            continue
        head = wf.read_text().splitlines()[:5]
        if any("# RATIONALE:" in line for line in head):
            continue
        violations.append(
            f"C1: {wf.name} does not match an allowed role and has no '# RATIONALE:' "
            f"comment in its first 5 lines"
        )
    return violations


def check_c2_naming(workflows_dir):
    """No 'x86-64' token in upload-artifact name:/asset filenames; no apple-silicon/intel in those contexts."""
    violations = []
    for wf in _workflow_files(workflows_dir):
        for lineno, line in enumerate(wf.read_text().splitlines(), start=1):
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            if "x86-64" in line and "AOT_TARGET" not in line:
                violations.append(f"C2: {wf.name}:{lineno}: 'x86-64' token outside an annotated exception: {stripped}")
            if ("apple-silicon" in line or "intel" in line) and "name:" in line:
                violations.append(f"C2: {wf.name}:{lineno}: apple-silicon/intel token in a name: context: {stripped}")
    return violations


def check_c3_single_publisher(workflows_dir):
    """'gh release upload' / 'softprops/action-gh-release' appears in exactly one file, at most once."""
    pattern = re.compile(r"gh release upload|softprops/action-gh-release")
    hits_by_file = {}
    for wf in _workflow_files(workflows_dir):
        # Comment lines are historical prose ("this used to call X, now replaced
        # by publish_release.py") -- only live YAML tokens count as usage.
        code_lines = [line for line in wf.read_text().splitlines()
                      if not line.strip().startswith("#")]
        count = len(pattern.findall("\n".join(code_lines)))
        if count:
            hits_by_file[wf.name] = count
    violations = []
    if len(hits_by_file) > 1:
        violations.append(f"C3: publish tokens found in multiple files: {sorted(hits_by_file)}")
    total = sum(hits_by_file.values())
    if total > 1:
        violations.append(f"C3: publish tokens found {total} times, expected at most once total: {hits_by_file}")
    return violations


_UPLOAD_ARTIFACT_NAME_RE = re.compile(
    r"uses:\s*actions/upload-artifact[^\n]*\n(?:[^\n]*\n){0,5}?\s*name:\s*([^\n#]+)"
)


_ARCH_TAG_RE = re.compile(r"arch_tag:\s*(\S+)")


def _upload_artifact_names(workflows_dir):
    """Return {filename: [names]}, resolving the one known matrix template
    (`${{ matrix.arch_tag }}`) against the file's own `arch_tag:` matrix
    entries so a per-leg matrix name is still checkable statically. Any other
    unresolvable `${{ }}` expression is left as-is (skipped by callers)."""
    names = {}
    for wf in _workflow_files(workflows_dir):
        text = wf.read_text()
        arch_tags = _ARCH_TAG_RE.findall(text)
        for match in _UPLOAD_ARTIFACT_NAME_RE.finditer(text):
            name = match.group(1).strip().strip("'\"")
            if "${{ matrix.arch_tag }}" in name and arch_tags:
                for tag in arch_tags:
                    names.setdefault(wf.name, []).append(name.replace("${{ matrix.arch_tag }}", tag))
            else:
                names.setdefault(wf.name, []).append(name)
    return names


def check_c4_artifact_names(workflows_dir):
    """Every upload-artifact name is canonical <component>-<platform>-<arch> (>=2 hyphens,
    platform token from arch_map.toml) or a zero-hyphen diagnostic name."""
    platforms = _platform_tokens()
    violations = []
    for wf_name, names in _upload_artifact_names(workflows_dir).items():
        for name in names:
            # Skip expression-templated names (e.g. matrix interpolation); those
            # are checked at the source-token level, not resolvable statically.
            if "${{" in name:
                continue
            hyphens = name.count("-")
            if hyphens == 0:
                continue  # zero-hyphen diagnostic name: allowed (F10)
            if hyphens >= 2 and any(p in name for p in platforms):
                continue
            if name in _KNOWN_NON_CANONICAL_ARTIFACT_NAMES:
                continue
            violations.append(
                f"C4: {wf_name}: upload-artifact name {name!r} is neither canonical "
                f"(<component>-<platform>-<arch>, >=2 hyphens with a known platform token) "
                f"nor a zero-hyphen diagnostic name"
            )
    return violations


def check_c5_real_trigger(workflows_dir):
    """Every workflow has a real trigger, not a `ci/**`-push-only trigger."""
    violations = []
    for wf in _workflow_files(workflows_dir):
        text = wf.read_text()
        match = re.search(r"^on:\s*\n((?:[ \t]+.*\n?)+)", text, re.MULTILINE)
        on_block = match.group(1) if match else ""
        has_workflow_call = "workflow_call" in on_block
        has_dispatch = "workflow_dispatch" in on_block
        has_pr = "pull_request" in on_block
        has_push = "push" in on_block
        push_paths_ci_only = bool(re.search(r"paths:\s*\n\s*-\s*['\"]?ci/\*\*", on_block))
        if has_workflow_call or has_dispatch or has_pr:
            continue
        if has_push and not push_paths_ci_only:
            continue
        if has_push and push_paths_ci_only:
            violations.append(f"C5: {wf.name}: only trigger present is a ci/**-push, with no PR/push-main/workflow_call pairing")
            continue
        if not (has_workflow_call or has_dispatch or has_pr or has_push):
            violations.append(f"C5: {wf.name}: no recognisable real trigger in its 'on:' block")
    return violations


def check_c6_required_asset_set(workflows_dir):
    """The publish job's required= list is exactly the set of legs' canonical asset artifact names."""
    build_yml = pathlib.Path(workflows_dir) / "build.yml"
    violations = []
    if not build_yml.exists():
        return violations  # nothing to check against in a partial fixture
    text = build_yml.read_text()
    match = re.search(r'required="\n(.*?)\n\s*"', text, re.DOTALL)
    if not match:
        violations.append("C6: build.yml has no 'required=\"...\"' asset-set block to check")
        return violations
    required = {line.strip() for line in match.group(1).splitlines() if line.strip()}

    canonical = set()
    for wf_name, names in _upload_artifact_names(workflows_dir).items():
        for name in names:
            if "${{" in name:
                continue
            if name.count("-") >= 2:
                canonical.add(name)

    missing_from_required = canonical - required
    missing_from_canonical = required - canonical
    for name in sorted(missing_from_required):
        violations.append(f"C6: {name!r} is produced by an upload-artifact step but missing from build.yml's required= set")
    for name in sorted(missing_from_canonical):
        violations.append(f"C6: {name!r} is in build.yml's required= set but no upload-artifact step produces it")
    return violations


_HEX40_RE = re.compile(r"\b[0-9a-f]{40}\b")
_BASELINE_CONTEXT_RE = re.compile(r"baseline|vcpkg", re.IGNORECASE)


def check_c9_no_hardcoded_vcpkg_baseline(workflows_dir):
    """No workflow file hardcodes the vcpkg builtin-baseline SHA.

    Ruling 5 (2026-09-01): native/vcpkg/vcpkg.json's "builtin-baseline" is the
    ONLY authority for this value; every workflow must derive it at runtime
    (see the "Derive vcpkg baseline from vcpkg.json" step in
    macos_build.yml/heif_dist_windows.yml) rather than hardcode its own copy.
    A hardcoded copy is a violation whether or not it currently matches
    vcpkg.json's value -- a second authority that happens to agree today is
    exactly the drift risk this rule exists to catch before it disagrees.

    Any 40-hex token on a line that also mentions "baseline" or "vcpkg"
    (case-insensitive) is flagged, except a line that is itself the runtime
    derivation (contains "GITHUB_ENV" or "builtin-baseline" -- the latter is
    the JSON key name read out of vcpkg.json, not a SHA literal).
    """
    violations = []
    for wf in _workflow_files(workflows_dir):
        for lineno, line in enumerate(wf.read_text().splitlines(), start=1):
            if "GITHUB_ENV" in line or "builtin-baseline" in line:
                continue
            if not _BASELINE_CONTEXT_RE.search(line):
                continue
            for token in _HEX40_RE.findall(line):
                violations.append(
                    f"C9: {wf.name}:{lineno}: hardcoded 40-hex vcpkg baseline literal "
                    f"{token!r} -- derive it at runtime from native/vcpkg/vcpkg.json's "
                    f"builtin-baseline instead (ruling 5, 2026-09-01): {line.strip()}"
                )
    return violations


def check_c8_ledger_sync(workflows_dir):
    """render_expectations.py --check passes against the given workflows directory."""
    sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))
    import render_expectations as re_mod
    try:
        disagreements = re_mod.check(workflows_dir=workflows_dir)
    except Exception as exc:  # ledger load errors etc. surface as a C8 violation
        return [f"C8: {exc}"]
    return [f"C8: {d}" for d in disagreements]


_GITIGNORE_PATH = REPO_ROOT / ".gitignore"


def check_c10_android_jnilibs_so_gitignored(workflows_dir):
    """.gitignore excludes plugin/android/src/main/jniLibs/*/*.so.

    Ruling 2026-08-31, A-T12 option (a): the Android jniLibs .so is
    placed-at-build/fetch, never committed (same policy as
    plugin/windows/Libraries/*.dll). This rule guards against someone
    silently reverting the .gitignore exclusion and re-committing a
    trust-on-first-use binary. `workflows_dir` is unused -- this rule checks
    the repo-root .gitignore, not a workflow file -- but is accepted for a
    uniform RULES call signature.
    """
    if not _GITIGNORE_PATH.exists():
        return ["C10: .gitignore is missing"]
    text = _GITIGNORE_PATH.read_text()
    if "plugin/android/src/main/jniLibs/*/*.so" not in text:
        return [
            "C10: .gitignore does not exclude plugin/android/src/main/jniLibs/*/*.so "
            "(ruling 2026-08-31 option a, A-T12) -- the Android jniLibs .so must stay "
            "placed-at-build/fetch, not committed"
        ]
    return []


RULES = [
    check_c1_roles,
    check_c2_naming,
    check_c3_single_publisher,
    check_c4_artifact_names,
    check_c5_real_trigger,
    check_c6_required_asset_set,
    check_c8_ledger_sync,
    check_c9_no_hardcoded_vcpkg_baseline,
    check_c10_android_jnilibs_so_gitignored,
]


def run_all(workflows_dir):
    violations = []
    for rule in RULES:
        violations.extend(rule(workflows_dir))
    return violations


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--workflows-dir", default=str(DEFAULT_WORKFLOWS_DIR),
                    help="Directory of workflow .yml files to check (default: %(default)s)")
    args = ap.parse_args(argv)

    violations = run_all(args.workflows_dir)
    if violations:
        for v in violations:
            print(f"::error::{v}", file=sys.stderr)
        return 1
    print(f"all {len(RULES)} CI convention rules pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
