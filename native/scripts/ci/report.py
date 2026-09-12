"""The sole writer of observable CI output lines: markers, RC lines, section
banners, and GitHub Actions annotations.

Frozen interface: docs/logs/2026-09-13/pyci-plan.md WI-2. Every line format
below is a contract that downstream markerdiff.py (WI-3) parses byte-exactly
and that the AC-2 golden fixtures were captured against — a "helpful"
reformat (added whitespace, a trailing period, `RC =` instead of `RC=`) is a
silent regression that only a multiset marker diff against a real CI run
would catch, so this module never varies its own format.

Why `flush=True` everywhere: this process interleaves its own prints with a
child's inherited stdout (via run.py's subprocess calls sharing the parent's
stdout when not captured, and via job-log ordering in general); unflushed
buffering would let this module's lines appear out of order relative to the
child's in the job log.

Why `error()` defaults to stderr but takes a `stream` override: every ported
`::error::` line in the workflows ends in `>&2`, except `linux_build.yml:878`
(`echo "::error::probe_codecs exited ${RC} …"`, no redirect) and its
macOS/Windows equivalents — WI-13 passes `stream=sys.stdout` for those call
sites to preserve that exact pre-existing asymmetry (C-G4/C-G5: port the
practice, not an idealization of it).
"""

from __future__ import annotations

import sys


def marker(name: str, value) -> None:
    """Prints exactly ``NAME=value``."""
    print(f"{name}={value}", flush=True)


def rc(name: str, code: int) -> None:
    """Prints exactly ``NAME_RC=code``."""
    print(f"{name}_RC={code}", flush=True)


def bare_rc(code: int, label: str = "") -> None:
    """Prints exactly ``RC=<code>`` when ``label`` is empty, else
    ``RC=<code> (<label>)``."""
    if label:
        print(f"RC={code} ({label})", flush=True)
    else:
        print(f"RC={code}", flush=True)


def section(title: str) -> None:
    """Prints exactly ``== title ==``."""
    print(f"== {title} ==", flush=True)


def error(msg: str, stream=None) -> None:
    """Prints exactly ``::error::msg``. Defaults to stderr; pass
    ``stream=sys.stdout`` for the documented pre-existing exceptions above."""
    target = stream if stream is not None else sys.stderr
    print(f"::error::{msg}", file=target, flush=True)


def notice(msg: str) -> None:
    """Prints exactly ``::notice::msg`` to stdout."""
    print(f"::notice::{msg}", flush=True)


def plain(text: str) -> None:
    """Verbatim passthrough for ported ``echo`` lines that carry no marker
    structure of their own."""
    print(text, flush=True)


def github_env_append(github_env_path, key: str, value: str) -> None:
    """Appends ``key=value\\n`` to the file at ``github_env_path``.

    Refuses (prints an ``::error::`` and raises ``SystemExit(2)``) when the
    path argument is empty or missing: a step that silently exports nothing
    and still exits 0 is precisely the false-green shape a prior CI round
    produced (C-G17 sibling lesson — a step that appears to run but does not
    do its one job is worse than a step that fails loudly).
    """
    if not github_env_path:
        error("github_env_append: no path given (refusing a silent no-op export)")
        raise SystemExit(2)
    from pathlib import Path

    path = Path(github_env_path)
    if not path.exists():
        error(f"github_env_append: path does not exist: {github_env_path}")
        raise SystemExit(2)
    with path.open("a", encoding="utf-8") as fh:
        fh.write(f"{key}={value}\n")
