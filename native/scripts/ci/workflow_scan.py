"""Parses `run:` step bodies out of a GitHub Actions workflow YAML file, by
line, without a full YAML parser -- because a full YAML parser throws away
line numbers and the whole point of this module is naming an offending
`<file>:<line>`.

Every step in this repo's workflows starts with `- name: ...` (verified by
grep across all five workflow files before writing this scanner -- there is
no unnamed step to handle), so the scanner keys off that convention: a step
boundary is a line matching `-\\s+name:`, and every key belonging to that
step (`run:`, `uses:`, `shell:`, `if:`, ...) sits at `name:`'s own indent.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

_STEP_NAME_RE = re.compile(r"^-\s+name:\s*(.*)$")
_RUN_KEY_RE = re.compile(r"^run:\s*(\||>[-+]?)?\s*(.*)$")


@dataclass(frozen=True)
class RunStep:
    workflow: str  # file name, e.g. "linux_build.yml"
    step_name: str  # the `- name:` this body belongs to ("" if unnamed)
    start_line: int  # 1-based line of the `run:` key
    body_lines: list = field(default_factory=list)  # (line_no, text) pairs


def _unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
        return value[1:-1]
    return value


def iter_run_steps(text: str, workflow_name: str):
    """Yields one RunStep per `run:` key found, in document order."""
    lines = text.splitlines()
    n = len(lines)
    step_name = ""
    step_key_indent = None  # indent of the keys (`run:`, etc.) inside the
    # current step -- one `- ` (2 chars) deeper than the `-` itself.
    i = 0
    while i < n:
        raw = lines[i]
        stripped = raw.lstrip(" ")
        indent = len(raw) - len(stripped)

        m_name = _STEP_NAME_RE.match(stripped)
        if m_name:
            step_name = _unquote(m_name.group(1))
            step_key_indent = indent + 2
            i += 1
            continue

        m_run = _RUN_KEY_RE.match(stripped)
        if m_run and step_key_indent is not None and indent == step_key_indent:
            block_indicator = m_run.group(1)
            inline_rest = m_run.group(2)
            start_line = i + 1
            body_lines = []
            if block_indicator:
                j = i + 1
                while j < n:
                    body_line = lines[j]
                    if body_line.strip() == "":
                        body_lines.append((j + 1, ""))
                        j += 1
                        continue
                    body_indent = len(body_line) - len(body_line.lstrip(" "))
                    # A line at or below the `run:` key's OWN indent is the
                    # next key in the same step (or the next step entirely),
                    # never part of this body -- this is the "body whose
                    # first line is at the same indent as run: must not be
                    # swallowed" requirement.
                    if body_indent <= indent:
                        break
                    body_lines.append((j + 1, body_line))
                    j += 1
                i = j
            else:
                if inline_rest:
                    body_lines.append((start_line, inline_rest))
                i += 1
            # Trim trailing blank lines that only exist because the block
            # scalar was followed by blank separator lines before the next key.
            while body_lines and body_lines[-1][1] == "":
                body_lines.pop()
            yield RunStep(
                workflow=workflow_name,
                step_name=step_name,
                start_line=start_line,
                body_lines=body_lines,
            )
            continue

        i += 1


def code_lines(step: RunStep) -> list:
    """Body lines with comments and blank lines removed, stripped."""
    out = []
    for _line_no, text in step.body_lines:
        t = text.strip()
        if not t or t.startswith("#"):
            continue
        out.append(t)
    return out
