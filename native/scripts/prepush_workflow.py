"""Workflow reader for `ci.py prepush` -- the gate's steps come FROM the
workflow files, not from a hand-kept copy of them.

Parses this repo's .github/workflows/*.yml as indentation-structured text
(no PyYAML: CI's pinned interpreter does not ship it and the gate must not
depend on it either). It understands exactly the subset this repo uses:

    jobs:
      <job>:
        env:            KEY: value            (job env, may reference matrix)
        strategy.matrix.include: list of rows (flat key: value maps)
        steps:
          - name: ...
            if: ...                           (matrix.K == 'V' [&& ...], always(), failure())
            uses: ...
            shell: ...
            working-directory: ...
            env:          KEY: value
            run: <one line>   |   run: |  <block>

`derive_argv()` turns a step whose run body is ONE command into an argv,
substituting `${{ matrix.* }}`, `${{ github.workspace }}`, `${{ env.* }}` and
`$VAR` / `${VAR}` from the supplied environment. A step with a multi-command
body is not derivable; the gate must then name an implementation for it, and
the classification table in prepush.py is checked against the parsed step
NAMES on every run, so a step added to a workflow cannot go unnoticed.
"""
from __future__ import annotations

import re
import shlex
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


@dataclass
class WfStep:
    name: str
    keys: dict = field(default_factory=dict)  # if / uses / shell / working-directory
    env: dict = field(default_factory=dict)
    run: Optional[str] = None


@dataclass
class WfJob:
    file: str
    job: str
    env: dict = field(default_factory=dict)
    rows: list = field(default_factory=list)  # matrix include rows ([{}] when no matrix)
    steps: list = field(default_factory=list)
    uses: Optional[str] = None  # reusable-workflow caller


def _indent(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def _scalar(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    return value


def _strip_comment(line: str) -> str:
    # Workflow values in this repo never contain " #" inside quotes.
    idx = line.find(" #")
    return line if idx < 0 else line[:idx]


def parse_workflow(path: Path) -> list:
    lines = path.read_text(encoding="utf-8").splitlines()
    jobs = []
    i = 0
    n = len(lines)
    in_jobs = False
    while i < n:
        raw = lines[i]
        if raw and not raw.startswith((" ", "#")):
            in_jobs = raw.rstrip() == "jobs:"
            i += 1
            continue
        if in_jobs and _indent(raw) == 2 and re.match(r"^  [A-Za-z0-9_-]+:\s*(#.*)?$", raw):
            job = WfJob(file=path.name, job=raw.strip().split(":")[0])
            i = _parse_job(lines, i + 1, job)
            jobs.append(job)
            continue
        i += 1
    return jobs


def _parse_map(lines: list, i: int, indent: int, out: dict) -> int:
    """Flat `KEY: value` map at exactly `indent`; returns next index."""
    while i < len(lines):
        raw = lines[i]
        if not raw.strip() or raw.lstrip().startswith("#"):
            i += 1
            continue
        if _indent(raw) < indent:
            break
        if _indent(raw) == indent:
            key, _, value = _strip_comment(raw).strip().partition(":")
            out[key.strip()] = _scalar(value)
        i += 1
    return i


def _parse_job(lines: list, i: int, job: WfJob) -> int:
    rows_mode = False
    while i < len(lines):
        raw = lines[i]
        if raw and not raw.startswith(" ") and not raw.startswith("#"):
            return i
        if raw.strip() and not raw.lstrip().startswith("#") and _indent(raw) == 2:
            return i
        stripped = raw.strip()
        ind = _indent(raw)
        if ind == 4 and stripped.startswith("uses:"):
            job.uses = _scalar(stripped.split(":", 1)[1])
        elif ind == 4 and stripped == "env:":
            i = _parse_map(lines, i + 1, 6, job.env)
            continue
        elif ind == 6 and stripped == "matrix:":
            rows_mode = True
        elif rows_mode and stripped.startswith("- ") and ind == 10:
            row: dict = {}
            key, _, value = _strip_comment(stripped[2:]).partition(":")
            row[key.strip()] = _scalar(value)
            i = _parse_map(lines, i + 1, 12, row)
            job.rows.append(row)
            continue
        elif ind == 4 and stripped == "steps:":
            rows_mode = False
            i = _parse_steps(lines, i + 1, job)
            continue
        i += 1
    return i


_BLOCK_SCALARS = {"|", ">"}


def _parse_steps(lines: list, i: int, job: WfJob) -> int:
    step: Optional[WfStep] = None
    while i < len(lines):
        raw = lines[i]
        if not raw.strip() or raw.lstrip().startswith("#"):
            i += 1
            continue
        ind = _indent(raw)
        if ind < 6:
            break
        stripped = raw.strip()
        if ind == 6 and stripped.startswith("- "):
            step = WfStep(name="")
            job.steps.append(step)
            stripped = stripped[2:]
            ind = 8
        if step is None or ind != 8:
            i += 1
            continue
        key, _, value = stripped.partition(":")
        key = key.strip()
        value = value.strip()
        if key == "name":
            step.name = _scalar(_strip_comment(value) if not value.startswith(("'", '"')) else value)
        elif key == "env":
            i = _parse_map(lines, i + 1, 10, step.env)
            continue
        elif key == "with":
            i = _parse_map(lines, i + 1, 10, {})
            continue
        elif key == "run":
            if value.rstrip("-+") in _BLOCK_SCALARS:
                body = []
                i += 1
                while i < len(lines) and (not lines[i].strip() or _indent(lines[i]) >= 10):
                    body.append(lines[i][10:] if lines[i].strip() else "")
                    i += 1
                step.run = "\n".join(body).strip("\n")
                continue
            step.run = value
        else:
            step.keys[key] = _scalar(_strip_comment(value))
        i += 1
    return i


# ---------------------------------------------------------------------------
# Evaluation helpers.
# ---------------------------------------------------------------------------
_COND_TERM = re.compile(r"^matrix\.([A-Za-z0-9_]+)\s*==\s*'([^']*)'$")


def condition(step: WfStep, row: dict) -> str:
    """'run' | 'skip-row' | 'on-failure'. Unknown condition shapes raise --
    a condition the gate cannot evaluate must not be guessed."""
    cond = step.keys.get("if", "").strip()
    if cond.startswith("${{") and cond.endswith("}}"):
        cond = cond[3:-2].strip()
    if not cond:
        return "run"
    if "failure()" in cond:
        return "on-failure"
    for term in (t.strip() for t in cond.split("&&")):
        if term == "always()":
            continue
        m = _COND_TERM.match(term)
        if not m:
            raise ValueError(f"unsupported if: {cond!r} in step {step.name!r}")
        if str(row.get(m.group(1), "")) != m.group(2):
            return "skip-row"
    return "run"


_EXPR = re.compile(r"\$\{\{\s*([^}]+?)\s*\}\}")


def expand_expr(text: str, row: dict, workspace: str, env: dict) -> str:
    def repl(m):
        expr = m.group(1)
        if expr == "github.workspace":
            return workspace
        if expr.startswith("matrix."):
            return str(row[expr[len("matrix."):]])
        if expr.startswith("env."):
            return env[expr[len("env."):]]
        raise ValueError(f"unsupported expression ${{{{ {expr} }}}}")
    return _EXPR.sub(repl, text)


_VAR = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)")


def expand_vars(text: str, env: dict) -> str:
    def repl(m):
        name = m.group(1) or m.group(2)
        if name not in env:
            raise KeyError(f"${name} is not defined for this step")
        return env[name]
    return _VAR.sub(repl, text)


@dataclass
class Command:
    argv: list
    env: dict  # `export` assignments in effect for this command
    tee: Optional[str] = None  # `2>&1 | tee FILE` target


# Lines with no effect on the command sequence's outcome under the RC
# discipline this repo's workflows use: errexit toggles, `X=$?` RC captures
# and the `echo`/`exit` lines that print/return that captured RC.
_INERT = re.compile(r"^(set\s+[-+][euxo]+(\s+\w+)?|[A-Za-z_][A-Za-z0-9_]*=\$\?|echo\b.*|exit\b.*)$")
_EXPORT = re.compile(r"^export\s+([A-Za-z_][A-Za-z0-9_]*)=(.*)$")
_TEE = re.compile(r"\s+(?:2>&1\s+)?\|\s*tee\s+(\S+)\s*$")
_KEYWORDS = {"if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case", "esac", "function"}


def _shell_syntax(line: str) -> bool:
    """True when `line` has an unquoted shell operator or starts with a
    compound-command keyword -- checked on shlex tokens, so quoted text
    (e.g. an `--error "...(...)..."` message) never trips it."""
    lexer = shlex.shlex(line, posix=True, punctuation_chars=True)
    lexer.whitespace_split = True
    try:
        tokens = list(lexer)
    except ValueError:
        return True
    if not tokens or tokens[0] in _KEYWORDS:
        return True
    return any(tok and all(ch in "();<>|&" for ch in tok) for tok in tokens) or "`" in line


def derive_commands(step: WfStep, row: dict, workspace: str, env: dict) -> Optional[list]:
    """The run body as a sequence of commands, or None if it uses any shell
    construct beyond: comments, `\\` continuations, `export K=V`, a trailing
    `2>&1 | tee FILE`, and the inert RC-discipline lines in `_INERT`.
    Semantics when executed: in order, stopping at the first non-zero RC,
    which is the step's RC -- every multi-command body here either runs
    under errexit or captures and exits with the single command's RC."""
    if step.run is None:
        return None
    logical, pending = [], ""
    for raw in step.run.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.endswith("\\"):
            pending += line[:-1] + " "
            continue
        logical.append(pending + line)
        pending = ""
    if pending:
        logical.append(pending)
    commands, exported = [], {}
    for line in logical:
        line = expand_expr(line, row, workspace, {**env, **exported})
        if _INERT.match(line):
            continue
        m = _EXPORT.match(line)
        if m:
            exported[m.group(1)] = expand_vars(" ".join(shlex.split(m.group(2))), {**env, **exported})
            continue
        tee = None
        m = _TEE.search(line)
        if m:
            tee = expand_vars(m.group(1).strip("\"'"), {**env, **exported})
            line = line[: m.start()]
        if _shell_syntax(line):
            return None
        scope = {**env, **exported}
        # shlex (POSIX) on the line first, then $VAR in each token, so a
        # Windows path value with backslashes is never re-tokenised.
        try:
            argv = [expand_vars(tok, scope) for tok in shlex.split(line)]
        except KeyError:
            return None  # e.g. a PowerShell `$LASTEXITCODE` body
        commands.append(Command(argv, dict(exported), tee))
    return commands or None


def job_env(job: WfJob, row: dict, workspace: str) -> dict:
    out: dict = {}
    for key, value in job.env.items():
        out[key] = expand_expr(value, row, workspace, out)
    return out
