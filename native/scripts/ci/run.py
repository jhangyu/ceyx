"""Process execution primitives. The only place under ``native/scripts/ci/``
that imports ``subprocess`` (WI-2 acceptance criterion; enforced by the AST
lint in ``native/scripts/deps/test_no_shell_lint.py`` once its scan roots are
extended to include this package).

Three rules, each earned by a prior CI incident and repeated here because a
future reader who doesn't know why will "simplify" this file back to the
broken version:

1. Never ``shell=True``, never a bare-string argv. Always a list, always
   ``shell=False``. This is what lets a mechanical AST lint prove the whole
   package clean instead of trusting every author's discipline by hand.
2. Never ``tool | grep``. Under ``pipefail`` (or even without it, the RC of
   a pipeline is the last command's), ``nm | grep -q SYMBOL`` returns 141
   when the symbol IS found, because `grep -q` exits after its first match
   and the writer (`nm`) gets SIGPIPE — a reverse gate that reports success
   as failure. Every command here writes its combined output to a string or
   file, and matching happens in Python afterward.
3. RC is captured immediately adjacent to the call that produced it — never
   read off the tail of a pipeline, never taken from a harness notification.
   ``RunResult.returncode`` comes straight from ``CompletedProcess``.
"""

from __future__ import annotations

import os
import subprocess
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class RunResult:
    argv: list = field(default_factory=list)
    returncode: int = 0
    stdout: str = ""
    stderr: str = ""


def run(argv, cwd=None, env=None) -> RunResult:
    """``subprocess.run(argv, shell=False)``, capturing combined output as
    text. Never raises: a missing executable is reported as returncode 127
    with the ``OSError`` text on stderr, so every caller always gets a
    ``RunResult`` back instead of having to catch an exception.
    """
    argv = [os.fspath(a) for a in argv]
    workdir = os.fspath(Path(cwd).resolve()) if cwd is not None else None
    try:
        completed = subprocess.run(
            argv,
            shell=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            cwd=workdir,
            env=env,
        )
    except OSError as exc:
        return RunResult(argv=argv, returncode=127, stdout="", stderr=f"{exc}\n")
    return RunResult(
        argv=argv,
        returncode=completed.returncode,
        stdout=completed.stdout or "",
        stderr=completed.stderr or "",
    )


def run_to_file(argv, out_path, cwd=None, env=None) -> RunResult:
    """Runs ``argv`` and writes its combined stdout+stderr to ``out_path``.

    This is the de-pipelining primitive that replaces a shell ``tool > file``
    redirect: the file is written by this process from the already-captured
    ``RunResult``, never by shell redirection, so the RC and the file content
    always agree with each other.
    """
    result = run(argv, cwd=cwd, env=env)
    combined = result.stdout
    if result.stderr:
        combined = combined + result.stderr
    Path(out_path).write_text(combined, encoding="utf-8")
    return result


def capture(argv, cwd=None, env=None):
    """Returns ``(rc, combined_text)`` — the shape most gate modules want
    when they only need to grep the output in Python, not keep stdout and
    stderr separate."""
    result = run(argv, cwd=cwd, env=env)
    combined = result.stdout
    if result.stderr:
        combined = combined + result.stderr
    return result.returncode, combined
