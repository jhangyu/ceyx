"""Regression test for the 2026-09-01 android jxl_dist_android.yml red:
``execute.acquire()``'s git branch cloned the pinned tag but never
initialised a manifest-declared ``submodules`` list, so any git-kind
component with submodules (libjxl: brotli/highway/skcms) produced a source
tree missing them on every caller EXCEPT the desktop-only
``fetch_libjxl.py`` carrier, which had its own dedicated clone_source().

Surfaced as CI run 33454838057: cmake configure failed with "Highway
library (hwy) not found" because ``third_party/highway`` under the cloned
tree was an empty submodule placeholder.

New file (not an edit to the shared test_execute.py) to avoid touching
another owner's file mid-round.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from deps import execute  # noqa: E402


def _loaded_with_submodules() -> dict:
    component = {
        "role": "test",
        "version": "9.9.9",
        "licence_files": ["COPYING*"],
        "path_keys": ["CMAKE_INSTALL_PREFIX"],
        "source": {
            "default": {
                "kind": "git",
                "reason": "no tarball with submodules is ever published upstream",
                "repo": "https://example.invalid/widget.git",
                "tag": "v{version}",
                "submodules": ["third_party/hwy", "third_party/brotli"],
            },
        },
        "cmake": {"base": {"CMAKE_INSTALL_PREFIX": "{dist}"}},
        "outputs": {},
    }
    return {"manifest": {"component": {"widget": component}}, "arch_map": {}}


def _loaded_without_submodules() -> dict:
    loaded = _loaded_with_submodules()
    del loaded["manifest"]["component"]["widget"]["source"]["default"]["submodules"]
    return loaded


def test_acquire_git_initialises_declared_submodules() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / "stage"
        calls: list[list[str]] = []

        def _fake_run(argv, **kwargs):
            calls.append(list(argv))
            if argv[:2] == ["git", "clone"]:
                Path(argv[-1]).mkdir(parents=True, exist_ok=True)
            return subprocess.CompletedProcess(list(argv), 0, "", "")

        with mock.patch.object(execute, "run", _fake_run), \
             mock.patch("deps.fetch.run", _fake_run):
            execute.acquire(_loaded_with_submodules(), "widget", "android", stage)

        assert len(calls) == 2, f"expected clone + submodule-init, got: {calls}"
        clone_argv, submodule_argv = calls
        assert clone_argv[:2] == ["git", "clone"]
        assert submodule_argv[:4] == ["git", "-C", str(stage / "widget-9.9.9"), "submodule"]
        assert "update" in submodule_argv and "--init" in submodule_argv
        assert "third_party/hwy" in submodule_argv
        assert "third_party/brotli" in submodule_argv


def test_acquire_git_skips_submodule_step_when_none_declared() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / "stage"
        calls: list[list[str]] = []

        def _fake_run(argv, **kwargs):
            calls.append(list(argv))
            if argv[:2] == ["git", "clone"]:
                Path(argv[-1]).mkdir(parents=True, exist_ok=True)
            return subprocess.CompletedProcess(list(argv), 0, "", "")

        with mock.patch.object(execute, "run", _fake_run), \
             mock.patch("deps.fetch.run", _fake_run):
            execute.acquire(_loaded_without_submodules(), "widget", "android", stage)

        assert len(calls) == 1, f"expected only clone, no submodule step: {calls}"
