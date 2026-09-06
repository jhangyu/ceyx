#!/usr/bin/env python3
"""Guard against test EXECUTION creeping back into ceyx CI.

User decree (2026-09-07): ceyx CI is COMPILE-ONLY, mirroring Halcyon's own
`no-serial-test-gate`-style policy lint (TestNoTestExecutionInCI). Building
test targets/binaries is allowed and is exactly what caught the ddb45b2/R7
CI-configure breakage; RUNNING them is not, because functional/perf
verification reads fixtures (sample DNGs, symbol-absent dylibs) that are
either gitignored or too expensive to fetch on every push, and because a
flaky test execution step is a false-negative CI signal, not a real one.

This scans every `.github/workflows/*.yml` file for `run:` step BODIES
containing a test-execution command, and fails loudly naming the exact file
and line. It deliberately does NOT flag:
  * `dart analyze` / `flutter analyze` (static analysis, not execution).
  * `flutter pub get` / cmake `add_executable(test_*...)` / a
    `cmake --build --target test_*` line (BUILDING a test target is allowed
    -- only RUNNING one is not).

Scope: only `.github/workflows/*.yml` `run:` step bodies are scanned -- this
script's own source (which necessarily names these patterns in prose) is
never read by itself, so no self-exclusion logic is needed.

Patterns treated as test EXECUTION:
  * `flutter test ...` / `dart test ...` / `ctest` -- flagged anywhere they
    appear as a real (non-comment) token in a `run:` body; no legitimate
    use of these three words exists in this repo's CI other than invoking
    them.
  * direct execution of a built `test_*`/`probe_*` binary -- flagged ONLY
    when the binary name is the FIRST TOKEN of the shell line (the command
    being run), not when it appears as a `--target <name>` build argument,
    a bare source/artifact filename (`.c`/`.cpp`/`.txt`/`.exe`/`.o`), inside
    a shell comment, or inside an unrelated string (e.g. `PROBE_CODECS_RC`,
    a variable name that merely CONTAINS "probe" -- case-sensitive match on
    the lowercase `probe_`/`test_` prefix already excludes it).

    EXCEPTION, explicit and printed (no-silent-caps): `probe_codecs`, in
    linux_build.yml/macos_build.yml/windows_build.yml, is a pre-existing,
    project-decreed-ALLOWED capability probe -- CLAUDE.md: "Capability
    probes (load the built library, query 'supports format X') are allowed
    as build-integrity checks." It compiles a tiny C snippet and runs it to
    read back a bitmask of codec support; it is not a functional/perf test
    and predates this guard. Excluded by exact basename, not by file.

Usage: python3 native/scripts/ci/check_no_test_execution_in_ci.py
Zero args; paths derived from this file's own location.
"""
import re
import sys
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
NATIVE_DIR = _SCRIPT_DIR.parent.parent
REPO_ROOT = NATIVE_DIR.parent
WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

# Substring patterns: flagged wherever they appear in a non-comment run-body
# line. No `--flag`-style false positive exists for these three in this
# repo's CI today.
SUBSTRING_PATTERNS = [
    ("flutter test", re.compile(r"(?<![\w./-])flutter\s+test\b")),
    ("dart test", re.compile(r"(?<![\w./-])dart\s+test\b")),
    ("ctest", re.compile(r"(?<![\w./-])ctest\b")),
]

# First-token check: a test_*/probe_* binary is EXECUTION only when it is the
# command word itself, not an argument to something else. Strips a leading
# quote and any `./` / `$VAR/` / `"$VAR"/`-style path prefix before matching.
FIRST_TOKEN_STRIP_RE = re.compile(
    r'^[\'"]?(?:(?:\./)|(?:\$\{?[A-Za-z_][A-Za-z0-9_]*\}?"?/))*'
)
TEST_OR_PROBE_BINARY_RE = re.compile(r"^(test_|probe_)[\w-]*")
# Extensions that mean "this is a NON-executable filename being referenced"
# (source files, logs, object files) -- NOT `.exe`, which on Windows is how
# an execution line invokes the binary directly (windows_build.yml:698).
SOURCE_OR_LOG_SUFFIXES = (".c", ".cpp", ".cc", ".txt", ".log", ".o")

# CLAUDE.md-decreed capability-probe carve-out (see module docstring). Exact
# basename only, so a differently-named future probe is NOT silently exempt.
ALLOWED_CAPABILITY_PROBES = {"probe_codecs"}

# YAML step keys that introduce a shell command body for the CURRENT step:
# `run: <inline>` or `run: |` / `run: >` followed by an indented block.
RUN_INLINE_RE = re.compile(r"^\s*run:\s*(.+)$")
RUN_BLOCK_RE = re.compile(r"^\s*run:\s*[|>][+-]?\s*$")


def iter_run_lines(text):
    """Yields (line_no, line_text) for every line that is part of a `run:`
    step body -- inline `run: <cmd>` lines, and every line of a `run: |`/`>`
    block until a line at or below the block's own indentation appears.
    """
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        inline = RUN_INLINE_RE.match(line)
        if inline and not RUN_BLOCK_RE.match(line):
            yield i + 1, inline.group(1)
            i += 1
            continue
        if RUN_BLOCK_RE.match(line):
            block_indent = len(line) - len(line.lstrip(" "))
            i += 1
            while i < len(lines):
                body_line = lines[i]
                if body_line.strip() == "":
                    yield i + 1, body_line
                    i += 1
                    continue
                indent = len(body_line) - len(body_line.lstrip(" "))
                if indent <= block_indent:
                    break
                yield i + 1, body_line
                i += 1
            continue
        i += 1


def main():
    if not WORKFLOWS_DIR.is_dir():
        print(f"[FAIL] workflow directory not found: {WORKFLOWS_DIR}")
        return 1

    workflow_files = sorted(WORKFLOWS_DIR.glob("*.yml"))
    if not workflow_files:
        print(f"[FAIL] no workflow files found under {WORKFLOWS_DIR}")
        return 1

    checked_lines = 0
    failures = []   # (file_rel, line_no, label, line_text)
    allowed = []    # (file_rel, line_no, binary_name, line_text)

    for wf in workflow_files:
        wf_rel = wf.relative_to(REPO_ROOT)
        text = wf.read_text(encoding="utf-8")
        for line_no, line_text in iter_run_lines(text):
            stripped = line_text.strip()
            checked_lines += 1
            if not stripped or stripped.startswith("#"):
                continue

            for label, pattern in SUBSTRING_PATTERNS:
                if pattern.search(line_text):
                    failures.append((str(wf_rel), line_no, label, stripped))

            first_token = stripped.split()[0]
            candidate = FIRST_TOKEN_STRIP_RE.sub("", first_token)
            # A trailing quote (the command was a quoted path, e.g.
            # `"$PROBE_DIR/probe_codecs"`) is part of the shell token, not the
            # binary's own name -- strip it BEFORE the extension/allowlist
            # checks below so `probe_codecs.exe"` is seen as ending in
            # `.exe`, not `.exe"`.
            candidate = candidate.rstrip('"\'')
            if not TEST_OR_PROBE_BINARY_RE.match(candidate):
                continue
            if candidate.endswith(SOURCE_OR_LOG_SUFFIXES):
                # A source/log filename REFERENCE (e.g. `probe_codecs.c`),
                # not the binary being invoked.
                continue
            basename = candidate[:-4] if candidate.endswith(".exe") else candidate
            if basename in ALLOWED_CAPABILITY_PROBES:
                allowed.append((str(wf_rel), line_no, basename, stripped))
                continue
            failures.append(
                (str(wf_rel), line_no, "direct test/probe binary execution",
                 stripped)
            )

    print(f"[check_no_test_execution_in_ci] scanned {len(workflow_files)} "
          f"workflow file(s), {checked_lines} `run:` step line(s).")

    if allowed:
        print(f"[check_no_test_execution_in_ci] ALLOWED {len(allowed)} "
              "capability-probe execution(s) (CLAUDE.md decree, exact-"
              "basename exemption -- see module docstring):")
        for wf_rel, line_no, basename, stripped in allowed:
            print(f"  {wf_rel}:{line_no}: [{basename}] {stripped}")
    else:
        print("[check_no_test_execution_in_ci] 0 capability-probe "
              "exemptions used.")

    if failures:
        print(f"[FAIL] {len(failures)} test-execution reference(s) found in "
              "CI workflow(s) -- ceyx CI is compile-only by user decree "
              "(2026-09-07). Building a test target/binary is fine; running "
              "one is not:")
        for wf_rel, line_no, label, line_text in failures:
            print(f"  {wf_rel}:{line_no}: [{label}] {line_text}")
        return 1

    print("[check_no_test_execution_in_ci] PASS -- no test-execution "
          "commands found in any CI workflow `run:` step.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
