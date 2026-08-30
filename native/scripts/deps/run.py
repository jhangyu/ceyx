"""Subprocess wrapper: argv lists only, shell=False, exit code read from
``CompletedProcess.returncode`` -- never from a pipeline.

Spec: docs/logs/2026-08-30/Spec_build_rewrite.md §2.3 (mandatory
Python-specific rules) and §2.4. This module exists to make the four
Windows path-mangling triggers documented in
docs/logs/2026-08-30/build-knowledge-handoff.md §A structurally
impossible: every external tool is invoked via ``CreateProcessW`` with an
explicit argument list, and no intermediate shell (MSYS/Git-Bash/pwsh)
ever gets a chance to re-interpret argv.

Do not add a code path that sets ``shell=True`` anywhere in this module --
``native/scripts`` is linted for that literal (A3.1).
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import PureWindowsPath
from typing import Mapping, Optional, Sequence, Union

PathLike = Union[str, "os.PathLike[str]"]


class SubprocessError(RuntimeError):
    """Raised when an external tool invoked via :func:`run` exits non-zero."""

    def __init__(self, argv: Sequence[str], returncode: int, stdout: str, stderr: str) -> None:
        self.argv = list(argv)
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
        super().__init__(f"command failed (exit {returncode}): {' '.join(self.argv)}")


def assert_native_windows_interpreter() -> None:
    """Refuse to run under an MSYS/Cygwin Python interpreter.

    Fix (round-1 review finding F1): MSYS2/Cygwin CPython reports
    ``os.name == "posix"`` (it is a POSIX-emulation layer on top of
    Windows), NOT ``"nt"`` -- so a guard that early-returns on
    ``os.name != "nt"`` is a no-op on exactly the interpreter it must
    reject. The reliable signal is ``sys.platform``, which MSYS2/Cygwin
    Python reports as ``"msys"``/``"cygwin"`` regardless of ``os.name``.
    This check therefore runs unconditionally (not gated on ``os.name``),
    so it also rejects an MSYS/Cygwin interpreter accidentally invoked
    from a non-Windows CI runner image.

    On native Windows Python (``os.name == "nt"``, ``sys.platform ==
    "win32"``) this additionally checks ``sys.executable`` for the
    MSYS/Cygwin path layout and a missing drive letter, as a second,
    independent signal (spec §2.3).
    """
    if sys.platform in ("cygwin", "msys"):
        raise RuntimeError(
            f"refusing to run under sys.platform={sys.platform!r}: this is "
            "MSYS/Cygwin Python, not native Windows Python -- use native "
            "Windows Python, invoked via `shell: pwsh`"
        )
    if os.name != "nt":
        return
    # PureWindowsPath is used explicitly (not the platform-dependent Path)
    # so this branch is exercisable and unit-testable from macOS/Linux too.
    executable_posix = PureWindowsPath(sys.executable).as_posix()
    if "/usr/bin" in executable_posix:
        raise RuntimeError(
            "refusing to run under a non-native Windows Python interpreter: "
            f"{sys.executable!r} (looks like MSYS/Cygwin Python -- use native "
            "Windows Python, invoked via `shell: pwsh`)"
        )
    if not PureWindowsPath(sys.executable).drive:
        raise RuntimeError(
            "refusing to run under a Python interpreter with no drive letter: "
            f"{sys.executable!r} (looks like MSYS/Cygwin Python -- use native "
            "Windows Python, invoked via `shell: pwsh`)"
        )


def run(
    argv: Sequence[str],
    *,
    cwd: Optional[PathLike] = None,
    env: Optional[Mapping[str, str]] = None,
    check: bool = True,
    capture_output: bool = True,
) -> "subprocess.CompletedProcess[str]":
    """Run ``argv`` with ``shell=False``. ``argv`` must be a list/tuple of
    ``str``, never a single command string.

    Rules enforced here (spec §2.3, each mechanically checkable):

    - ``argv`` must not be a ``str``/``bytes`` -- a caller building a
      command string instead of a list is exactly the mistake this module
      exists to prevent.
    - Every element must be a ``str``.
    - No element may contain ``|`` -- pipelines are forbidden (spec
      K41/N19: the SIGPIPE-under-``pipefail`` inversion this project has
      already paid for once; in this module the failure mode cannot occur
      because no pipeline is ever constructed).
    - ``shell`` is always ``False``; there is no parameter to override it.
    - The exit status returned to the caller is always
      ``CompletedProcess.returncode``, never inferred from output text or
      a pipeline's last stage.
    """
    # Enforced on every call, not just at CLI startup, so a direct importer
    # of this module (bypassing build_deps.py's startup check) cannot
    # accidentally shell out from an MSYS/Cygwin interpreter either
    # (round-1 review finding F1, related note).
    assert_native_windows_interpreter()

    if isinstance(argv, (str, bytes)):
        raise TypeError("run() requires a list of argv elements, not a string")
    argv_list = list(argv)
    if not argv_list:
        raise ValueError("run() requires a non-empty argv list")
    for element in argv_list:
        if not isinstance(element, str):
            raise TypeError(f"argv element is not a str: {element!r}")
        if "|" in element:
            raise ValueError(
                f"argv element contains '|' -- pipelines are forbidden here "
                f"(spec K41/N19, SIGPIPE-under-pipefail): {element!r}"
            )

    result = subprocess.run(  # noqa: S603 -- argv is always a list, shell is always False
        argv_list,
        cwd=os.fspath(cwd) if cwd is not None else None,
        env=dict(env) if env is not None else None,
        shell=False,
        capture_output=capture_output,
        text=True,
    )
    if check and result.returncode != 0:
        raise SubprocessError(argv_list, result.returncode, result.stdout or "", result.stderr or "")
    return result
