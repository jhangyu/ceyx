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

WI-5 (2026-09-13) extends this guard with two more scan surfaces, both
following the same no-silent-caps rule -- an exemption is only real if it is
named and printed, never a quiet filter:

  (a) UR-1 DETECTION + EXEMPTION -- a `python[3]? -m pytest|unittest ...`,
      bare `pytest ...`, or `native/tests/run_dist_equivalence.py` line is
      now a recognized test-SUITE invocation (previously invisible to this
      guard entirely -- P3 proved that by running its own regex objects
      against the literal workflow lines and finding none of them contained
      "pytest"). It is ALLOWED only when every path-shaped argument on the
      line starts with `native/scripts/deps/`, or the line is one of the
      three USER-RULED (2026-09-13) call sites in `ALLOWED_TEST_SUITE_CALLS`
      (the deps suite legitimately runs in CI on Linux/Windows; the D6
      layer-1 argv-equivalence check on macOS is not a pytest invocation at
      all but is exempted by the same named mechanism). Anything else --
      including a bare `pytest -q` with no path argument -- FAILS.

  (b) the Python side of the migration itself, `native/scripts/ci/**.py`,
      gains an AST scan: any `subprocess.*` call outside `run.py` is
      `[subprocess-outside-run]`, and any `run.run()`/`run_to_file()`/
      `capture()` call whose argv[0] statically resolves to a `test_`/
      `probe_` name is held to the identical `ALLOWED_CAPABILITY_PROBES`
      rule the YAML scan already uses. An argv[0] this scan cannot
      statically resolve (a variable, a dynamically-built path) is reported
      as `UNRESOLVED` rather than silently passed.

      CARRY-3 (leader ruling, 2026-09-13): `check_cmake_sources_tracked.py`
      already imports `subprocess` and predates this migration entirely --
      its last change, commit `2a5f28db` ("guard cmake source references
      against untracked files", 2026-09-07), is a `git merge-base
      --is-ancestor` verified ancestor of this campaign's `d33cc607`
      baseline. It is not migrated logic, so it is named in
      `GRANDFATHERED_SUBPROCESS_FILES` with its reason and provenance commit
      and printed every run -- never silently path-filtered, which would
      also exempt every FUTURE file dropped into the same directory.

Usage: python3 native/scripts/ci/check_no_test_execution_in_ci.py
Zero args; paths derived from this file's own location.
"""
import ast
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

# --- WI-5(a): UR-1 test-suite invocation detection + exemption ---

PYTHON_SCAN_ROOTS = ("native/scripts/ci",)

# UR-1 part (a) -- DETECTION. Until this pattern exists the guard cannot see
# a pytest/unittest invocation at all (see module docstring).
TEST_SUITE_INVOCATION_RE = re.compile(
    r"(?<![\w./-])(?:python3?|py)\s+-m\s+(?:pytest|unittest)\b"
    r"|(?<![\w./-])pytest\b"
    r"|(?<![\w./-])[\w./$-]*run_dist_equivalence\.py\b"
)

# A line naming `pytest`/`unittest` as a package being INSTALLED (`pip
# install ... pytest`) is not an invocation -- excluded the same way
# SOURCE_OR_LOG_SUFFIXES excludes a filename reference from the binary
# first-token rule above. Without this, `windows_build.yml:89` /
# `linux_build.yml:201` ("Install pytest for deps suite") would be flagged
# as a bare `pytest` invocation by the second alternative above, which is a
# false positive this guard must not introduce.
PIP_INSTALL_RE = re.compile(r"(?<![\w./-])pip\s+install\b")

# UR-1 part (b) -- EXEMPTION, named + printed, scoped by PATH ARGUMENT (not
# command name) so a suite outside native/scripts/deps/ can never inherit
# it. USER RULING 2026-09-13 -- detected, then exempted by name; a test
# suite outside these paths FAILS this guard.
ALLOWED_TEST_SUITE_PATH_PREFIXES = ("native/scripts/deps/",)
ALLOWED_TEST_SUITE_CALLS = (
    ("linux_build.yml", "Deps manifest/render/execute/heif test suite + no-shell lint",
     "python3 -m pytest native/scripts/deps/ -q"),
    ("windows_build.yml", "Run deps unit suite (native Windows Python)",
     "python -m pytest native/scripts/deps/ -q"),
    ("macos_build.yml", "D6 layer 1 — argv equivalence (renderer vs golden vs legacy shell)",
     "native/tests/run_dist_equivalence.py"),
)

# --- WI-5(b): CARRY-3 grandfathered subprocess-outside-run exemption ---
# See module docstring. Named + printed every run, never a silent filter.
GRANDFATHERED_SUBPROCESS_FILES = (
    (
        "native/scripts/ci/check_cmake_sources_tracked.py",
        "pre-existing guard predating this migration (commit 2a5f28db, "
        "2026-09-07, verified ancestor of baseline d33cc607) -- not "
        "migrated logic, CARRY-3",
    ),
)

RUN_PRIMITIVE_ATTRS = ("run", "run_to_file", "capture")


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


def classify_pytest_line(line):
    """Classifies a single `run:` body line for the UR-1 test-suite rule.

    Returns "not-a-pytest-line" (not a test-suite invocation at all),
    "allowed" (invocation, but exempted by path prefix or by the named
    literal call-site list), or "violation" (invocation outside the
    exemption -- this guard must FAIL it).
    """
    stripped = line.strip()
    if not stripped or stripped.startswith("#"):
        return "not-a-pytest-line"
    if not TEST_SUITE_INVOCATION_RE.search(stripped):
        return "not-a-pytest-line"
    if PIP_INSTALL_RE.search(stripped):
        return "not-a-pytest-line"

    path_tokens = [tok.strip("\"'\\") for tok in stripped.split() if "/" in tok]
    if path_tokens and all(
        any(tok.startswith(prefix) for prefix in ALLOWED_TEST_SUITE_PATH_PREFIXES)
        for tok in path_tokens
    ):
        return "allowed"

    for _workflow, _step_name, literal in ALLOWED_TEST_SUITE_CALLS:
        if literal in stripped:
            return "allowed"

    return "violation"


def _resolve_argv0_basename(node):
    """Statically resolves an argv[0] AST node to its basename string, or
    returns None if it cannot be resolved without executing the program.

    Handles: a plain string literal, an f-string whose final segment is a
    literal (the dynamic prefix is irrelevant to the basename), a
    `Path(...) / "name"` join, and a `str(...)` wrapper around any of the
    above.
    """
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return Path(node.value).name
    if isinstance(node, ast.JoinedStr) and node.values:
        last = node.values[-1]
        if isinstance(last, ast.Constant) and isinstance(last.value, str):
            tail = last.value
            return Path(tail).name if "/" in tail or "\\" in tail else tail
        return None
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div):
        if isinstance(node.right, ast.Constant) and isinstance(node.right.value, str):
            return node.right.value
        return None
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "str" and node.args:
        return _resolve_argv0_basename(node.args[0])
    return None


def _source_line(source_lines, lineno):
    if 1 <= lineno <= len(source_lines):
        return source_lines[lineno - 1].strip()
    return ""


def scan_python_sources(roots):
    """AST scan of every `*.py` file under `roots` (repo-relative). Returns
    `(failures, allowed, unresolved)`, each a list of
    `(file_rel, line_no, label_or_basename, snippet)` tuples.

    Two rules (WI-5(b)):
      * `subprocess.*` anywhere outside `run.py` is `[subprocess-outside-run]`,
        except the files named in `GRANDFATHERED_SUBPROCESS_FILES` (printed,
        not silent).
      * `run.run()`/`run.run_to_file()`/`run.capture()` calls have their
        argv[0] resolved and held to the same test_/probe_ +
        ALLOWED_CAPABILITY_PROBES rule the YAML scan uses; an unresolvable
        argv[0] is reported, not silently passed.
    """
    grandfathered = dict(GRANDFATHERED_SUBPROCESS_FILES)
    failures = []
    allowed = []
    unresolved = []

    for root_rel in roots:
        root = REPO_ROOT / root_rel
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*.py")):
            if "__pycache__" in path.parts:
                continue
            rel = str(path.relative_to(REPO_ROOT))
            source = path.read_text(encoding="utf-8")
            source_lines = source.splitlines()
            try:
                tree = ast.parse(source, filename=str(path))
            except SyntaxError:
                continue
            for node in ast.walk(tree):
                if not isinstance(node, ast.Call):
                    continue
                func = node.func

                if (
                    isinstance(func, ast.Attribute)
                    and isinstance(func.value, ast.Name)
                    and func.value.id == "subprocess"
                    and path.name != "run.py"
                ):
                    snippet = _source_line(source_lines, node.lineno)
                    if rel in grandfathered:
                        allowed.append((rel, node.lineno, "subprocess-outside-run (grandfathered)", snippet))
                    else:
                        failures.append((rel, node.lineno, "subprocess-outside-run", snippet))

                if (
                    isinstance(func, ast.Attribute)
                    and func.attr in RUN_PRIMITIVE_ATTRS
                    and isinstance(func.value, ast.Name)
                    and func.value.id != "subprocess"
                ):
                    snippet = _source_line(source_lines, node.lineno)
                    if not node.args or not isinstance(node.args[0], (ast.List, ast.Tuple)) or not node.args[0].elts:
                        unresolved.append((rel, node.lineno, snippet))
                        continue
                    argv0_node = node.args[0].elts[0]
                    basename = _resolve_argv0_basename(argv0_node)
                    if basename is None:
                        unresolved.append((rel, node.lineno, snippet))
                        continue
                    if not TEST_OR_PROBE_BINARY_RE.match(basename):
                        continue
                    if basename in ALLOWED_CAPABILITY_PROBES:
                        allowed.append((rel, node.lineno, basename, snippet))
                    else:
                        failures.append((rel, node.lineno, "direct test/probe binary execution", snippet))

    return failures, allowed, unresolved


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
    test_suite_allowed = []  # (file_rel, line_no, line_text)

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

            verdict = classify_pytest_line(line_text)
            if verdict == "allowed":
                test_suite_allowed.append((str(wf_rel), line_no, stripped))
            elif verdict == "violation":
                failures.append((str(wf_rel), line_no, "test-suite execution", stripped))

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

    # UR-1 (b): the named exemption list is printed EVERY run, pass or fail,
    # so a settled policy never reads as a pending one.
    print(f"[check_no_test_execution_in_ci] ALLOWED_TEST_SUITE_CALLS "
          f"({len(ALLOWED_TEST_SUITE_CALLS)} declared, USER RULING "
          "2026-09-13 -- detected, then exempted by name; a test suite "
          "outside these paths FAILS this guard):")
    for workflow, step_name, literal in ALLOWED_TEST_SUITE_CALLS:
        print(f"  {workflow} :: {step_name} -- {literal}")

    if test_suite_allowed:
        print(f"[check_no_test_execution_in_ci] {len(test_suite_allowed)} "
              "test-suite invocation(s) ALLOWED at scan time:")
        for wf_rel, line_no, stripped in test_suite_allowed:
            print(f"  {wf_rel}:{line_no}: {stripped}")
    else:
        print("[check_no_test_execution_in_ci] 0 test-suite invocations "
              "matched at scan time.")

    # WI-5(b): the Python side of the migration itself.
    py_failures, py_allowed, py_unresolved = scan_python_sources(PYTHON_SCAN_ROOTS)
    failures.extend(py_failures)

    print(f"[check_no_test_execution_in_ci] GRANDFATHERED_SUBPROCESS_FILES "
          f"({len(GRANDFATHERED_SUBPROCESS_FILES)} declared, CARRY-3, "
          "pre-existing files predating this migration -- not migrated "
          "logic):")
    for file_rel, reason in GRANDFATHERED_SUBPROCESS_FILES:
        print(f"  {file_rel} -- {reason}")

    if py_allowed:
        print(f"[check_no_test_execution_in_ci] {len(py_allowed)} Python-side "
              "call(s) ALLOWED (grandfathered subprocess use or an "
              "ALLOWED_CAPABILITY_PROBES basename):")
        for file_rel, line_no, label, snippet in py_allowed:
            print(f"  {file_rel}:{line_no}: [{label}] {snippet}")

    if py_unresolved:
        print(f"[check_no_test_execution_in_ci] {len(py_unresolved)} "
              "UNRESOLVED argv[0] reference(s) (reported, not silently "
              "passed -- a static resolver cannot prove these safe):")
        for file_rel, line_no, snippet in py_unresolved:
            print(f"  UNRESOLVED {file_rel}:{line_no}: {snippet}")

    if failures:
        print(f"[FAIL] {len(failures)} test-execution reference(s) found in "
              "CI workflow(s)/Python CI package -- ceyx CI is compile-only "
              "by user decree (2026-09-07). Building a test target/binary "
              "is fine; running one is not:")
        for file_rel, line_no, label, line_text in failures:
            print(f"  {file_rel}:{line_no}: [{label}] {line_text}")
        return 1

    print("[check_no_test_execution_in_ci] PASS -- no test-execution "
          "commands found in any CI workflow `run:` step or the Python CI "
          "package.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
