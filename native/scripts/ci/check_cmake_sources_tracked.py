#!/usr/bin/env python3
"""Guard against "local green, commit red": a cmake source-file reference that
exists on the author's disk (so their local configure/build succeeds) but was
never `git add`ed (so CI's checkout doesn't have it, and configure dies with
"Cannot find source file").

Incident this closes (2026-09-06/07, ceyx main): commit ddb45b2's
`git add native/cmake/tests.cmake` picked up the CURRENT WORKING-TREE state of
that shared file, which included a concurrent session's uncommitted
`add_executable(probe_strided_output tests/probe_strided_output.cpp)` block.
The `.cpp` existed on the author's disk (hence a clean local configure) but
was untracked, so every CI configure step failed. Disk existence is
DELIBERATELY NOT what this script checks -- that is the exact signal that
fooled the author. It checks git-tracked status instead.

Scope: every `add_executable`, `add_library`, and `target_sources` call in
native/cmake/*.cmake and native/CMakeLists.txt. Source arguments are resolved
relative to native/ (CMAKE_CURRENT_SOURCE_DIR for an `include()`d .cmake file
is the including CMakeLists.txt's directory, i.e. native/ itself -- not the
.cmake file's own directory; verified against this repo's existing entries,
e.g. tests.cmake's `tests/foo.cpp` resolving to native/tests/foo.cpp).

Skips (a source token cannot be resolved statically), printed so a skip is
never silent (this project's no-silent-caps rule):
  * any token containing "$" (a ${VARIABLE}, $<generator-expression>, or
    $ENV{...} reference) -- e.g. ${HALIDE_OUTPUT_DIR}/halide_runtime... .
  * known non-file keywords for the three call kinds (STATIC/SHARED/MODULE/
    OBJECT/ALIAS/IMPORTED/GLOBAL/EXCLUDE_FROM_ALL/PRIVATE/PUBLIC/INTERFACE).

Exit code: 0 if every resolvable source is `git ls-files`-tracked; 1 and a
named list of (cmake file:line, referenced path, resolved path) otherwise.

Usage: python3 native/scripts/ci/check_cmake_sources_tracked.py
Zero args; the repo root and native/ are derived from this file's own path,
so it runs the same locally and in CI regardless of cwd.
"""
import re
import shlex
import subprocess
import sys
from pathlib import Path

# native/scripts/ci/<this file> -> native/scripts -> native -> repo root.
_SCRIPT_DIR = Path(__file__).resolve().parent
NATIVE_DIR = _SCRIPT_DIR.parent.parent
REPO_ROOT = NATIVE_DIR.parent

CALL_KINDS = ("add_executable", "add_library", "target_sources")

# Tokens that are never a source path for these three call kinds.
NON_FILE_KEYWORDS = {
    "STATIC", "SHARED", "MODULE", "OBJECT", "ALIAS", "IMPORTED", "GLOBAL",
    "EXCLUDE_FROM_ALL", "PRIVATE", "PUBLIC", "INTERFACE",
}

CALL_START_RE = re.compile(r"\b(" + "|".join(CALL_KINDS) + r")\s*\(")


def strip_comments(text):
    """Removes a CMake `#`-to-end-of-line comment from each line.

    Deliberately naive (no quote-awareness): none of this repo's
    add_executable/add_library/target_sources calls embed a literal '#'
    inside a quoted argument, and a false strip here would only make the
    parse MORE conservative (fewer tokens seen), never silently accept an
    untracked file -- the failure mode this script exists to prevent stays
    closed either way.
    """
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def find_calls(text):
    """Yields (kind, line_no, inner_text) for each balanced call in `text`.

    `text` must already be comment-stripped. line_no is 1-based, pointing at
    the line the call NAME starts on.
    """
    for m in CALL_START_RE.finditer(text):
        kind = m.group(1)
        depth = 1
        i = m.end()
        start = i
        while i < len(text) and depth > 0:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        inner = text[start:i - 1]
        line_no = text.count("\n", 0, m.start()) + 1
        yield kind, line_no, inner


def extract_source_tokens(kind, inner):
    """Splits one call's inner text into candidate source-path tokens."""
    try:
        tokens = shlex.split(inner, comments=False, posix=True)
    except ValueError:
        # Unbalanced quotes -- cannot tokenize safely. Treat as nothing
        # resolvable rather than guessing; a real syntax error here would
        # already fail cmake's own configure, which this script runs before.
        return []
    if not tokens:
        return []
    # First token is always the target name for all three call kinds.
    candidates = tokens[1:]
    if kind == "add_library" and "ALIAS" in candidates:
        # add_library(<name> ALIAS <existing-target>) takes zero source
        # files -- the token after ALIAS is a TARGET name, not a path (e.g.
        # `add_library(ZLIB::ZLIB ALIAS zlibstatic)`). Treating it as a
        # source would flag every alias target as an untracked file.
        return []
    if kind == "target_sources":
        # target_sources(name <PRIVATE|PUBLIC|INTERFACE> src...)+ repeated.
        # The scope keywords are separators, not files; NON_FILE_KEYWORDS
        # already covers them below.
        pass
    return [t for t in candidates if t not in NON_FILE_KEYWORDS]


def main():
    cmake_files = sorted(NATIVE_DIR.glob("cmake/*.cmake"))
    top_level = NATIVE_DIR / "CMakeLists.txt"
    if top_level.is_file():
        cmake_files.append(top_level)

    if not cmake_files:
        print(f"[FAIL] no cmake files found under {NATIVE_DIR} -- "
              "check NATIVE_DIR derivation, this script's own path may have "
              "moved.")
        return 1

    checked = 0
    skipped = []   # (cmake_rel, line_no, token)
    failures = []  # (cmake_rel, line_no, token, resolved_rel, git_error)

    for cmake_file in cmake_files:
        cmake_rel = cmake_file.relative_to(REPO_ROOT)
        text = strip_comments(cmake_file.read_text(encoding="utf-8"))
        for kind, line_no, inner in find_calls(text):
            for token in extract_source_tokens(kind, inner):
                if "$" in token:
                    skipped.append((str(cmake_rel), line_no, token))
                    continue
                checked += 1
                token_path = Path(token.strip('"'))
                resolved = (
                    token_path if token_path.is_absolute()
                    else NATIVE_DIR / token_path
                )
                try:
                    resolved_rel = resolved.resolve().relative_to(
                        REPO_ROOT.resolve()
                    )
                except ValueError:
                    resolved_rel = resolved
                proc = subprocess.run(
                    ["git", "ls-files", "--error-unmatch", "--",
                     str(resolved_rel)],
                    cwd=REPO_ROOT,
                    capture_output=True,
                    text=True,
                )
                if proc.returncode != 0:
                    git_error = (proc.stderr or proc.stdout).strip().splitlines()
                    failures.append((
                        str(cmake_rel), line_no, token, str(resolved_rel),
                        git_error[0] if git_error else "git ls-files failed",
                    ))

    print(f"[check_cmake_sources_tracked] scanned {len(cmake_files)} cmake "
          f"file(s), {checked} resolvable source reference(s).")

    if skipped:
        print(f"[check_cmake_sources_tracked] SKIPPED {len(skipped)} "
              "variable/generator-expression reference(s) (cannot resolve "
              "statically, disk/git status not checked):")
        for cmake_rel, line_no, token in skipped:
            print(f"  {cmake_rel}:{line_no}: {token}")
    else:
        print("[check_cmake_sources_tracked] 0 references skipped.")

    if failures:
        print(f"[FAIL] {len(failures)} cmake source reference(s) are NOT "
              "tracked by git (this is the local-green-commit-red gap -- "
              "the file may exist on THIS disk but will not exist on a "
              "clean CI checkout):")
        for cmake_rel, line_no, token, resolved_rel, git_error in failures:
            print(f"  {cmake_rel}:{line_no}: \"{token}\" -> {resolved_rel} "
                  f"({git_error})")
        return 1

    print("[check_cmake_sources_tracked] PASS -- every resolvable cmake "
          "source reference is git-tracked.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
