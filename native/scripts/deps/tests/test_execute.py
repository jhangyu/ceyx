"""Execution-layer tests -- Plan D3 (round 4).

Mocking boundary: ``deps.run.run`` (and, for acquisition, the
``urllib.request.urlopen`` seam ``test_fetch.py`` already uses). Nothing
here monkeypatches ``os.environ`` or ``subprocess`` globally -- that route
is known to produce a pytest INTERNALERROR in this suite, and it also
destroys the property these tests exist to prove, namely that every
external tool goes through ``run.run`` as an argv list.

Every configure/build/install command is therefore asserted as a LIST of
argv elements, so a regression that reintroduces a command string (or a
pipeline) fails here rather than on a Windows runner.
"""
from __future__ import annotations

import io
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path
from unittest import mock

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from deps import execute  # noqa: E402
from deps import fetch as fetch_mod  # noqa: E402

_ARCH_MAP = {
    "arm64": {"apple": "arm64", "aom_target_cpu": "arm64"},
    "x86_64": {"apple": "x86_64", "aom_target_cpu": "x86_64"},
}


def _loaded(component_overrides: dict | None = None) -> dict:
    component = {
        "role": "test",
        "version": "9.9.9",
        "licence_files": ["COPYING*", "LICENSE*"],
        "path_keys": ["CMAKE_INSTALL_PREFIX"],
        "source": {
            "default": {
                "kind": "tarball",
                "reason": "no vcpkg port exists for widget at any version",
                "url": "https://example.invalid/widget-{version}.tar.gz",
                "sha256": "00",
            },
            "windows": {
                "override_only": True,
                "kind": "git",
                "reason": "the release tarball omits a Windows-only source file",
                "repo": "https://example.invalid/widget.git",
                "tag": "v{version}",
            },
        },
        "cmake": {
            "base": {"CMAKE_BUILD_TYPE": "Release", "CMAKE_INSTALL_PREFIX": "{dist}"},
            "macos": {"CMAKE_OSX_ARCHITECTURES": "{arch.apple}"},
        },
        "outputs": {
            "static_lib": {"candidates": ["lib/libwidget.a", "lib/widget.lib"], "required": True},
            "tool": {"candidates": ["bin/widget"], "required": False},
        },
    }
    if component_overrides:
        component.update(component_overrides)
    return {"manifest": {"component": {"widget": component}}, "arch_map": _ARCH_MAP}


def _make_tarball(path: Path, root: str, members: dict[str, bytes]) -> str:
    """Build a real .tar.gz on disk; return its sha256."""
    with tarfile.open(path, "w:gz") as archive:
        for name, payload in members.items():
            info = tarfile.TarInfo(f"{root}/{name}")
            info.size = len(payload)
            archive.addfile(info, io.BytesIO(payload))
    return fetch_mod.sha256_of(path)


# ---------------------------------------------------------------------------
# resolve_source / version substitution
# ---------------------------------------------------------------------------
def test_platform_override_is_additive_over_default() -> None:
    merged = execute.resolve_source(_loaded(), "widget", "windows")
    assert merged["kind"] == "git"
    assert merged["repo"] == "https://example.invalid/widget.git"
    # Inherited from source.default, NOT restated in the windows block.
    assert merged["url"] == "https://example.invalid/widget-{version}.tar.gz"


def test_default_used_when_platform_has_no_override() -> None:
    merged = execute.resolve_source(_loaded(), "widget", "macos")
    assert merged["kind"] == "tarball"


def test_unknown_component_raises_naming_it() -> None:
    with pytest.raises(execute.ExecuteError) as exc:
        execute.resolve_source(_loaded(), "nonesuch", "macos")
    assert "nonesuch" in str(exc.value)


# ---------------------------------------------------------------------------
# acquire()
# ---------------------------------------------------------------------------
def test_acquire_tarball_verifies_hash_then_extracts() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / "stage"
        stage.mkdir()
        source_archive = Path(tmp) / "upstream.tar.gz"
        sha = _make_tarball(source_archive, "widget-9.9.9", {"CMakeLists.txt": b"project(widget)"})
        payload = source_archive.read_bytes()

        loaded = _loaded()
        loaded["manifest"]["component"]["widget"]["source"]["default"]["sha256"] = sha

        with mock.patch.object(fetch_mod, "download", side_effect=lambda url, dest: (
            Path(dest).parent.mkdir(parents=True, exist_ok=True),
            Path(dest).write_bytes(payload),
            Path(dest),
        )[-1]):
            src_dir = execute.acquire(loaded, "widget", "macos", stage)

        assert src_dir == stage / "widget-9.9.9"
        assert (src_dir / "CMakeLists.txt").read_bytes() == b"project(widget)"


def test_acquire_tarball_rejects_wrong_hash_and_removes_the_file() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / "stage"
        stage.mkdir()
        payload = b"corrupted bytes"
        loaded = _loaded()
        loaded["manifest"]["component"]["widget"]["source"]["default"]["sha256"] = (
            "0" * 64
        )
        with mock.patch.object(fetch_mod, "download", side_effect=lambda url, dest: (
            Path(dest).parent.mkdir(parents=True, exist_ok=True),
            Path(dest).write_bytes(payload),
            Path(dest),
        )[-1]):
            with pytest.raises(fetch_mod.FetchError):
                execute.acquire(loaded, "widget", "macos", stage)
        assert not (stage / "widget-9.9.9.tar.gz").exists()


def test_acquire_registry_kind_refuses_rather_than_returning_a_ghost_path() -> None:
    loaded = _loaded()
    loaded["manifest"]["component"]["widget"]["source"]["default"] = {
        "kind": "registry",
        "version": "9.9.9",
        "reason": "supplied by vcpkg",
    }
    with pytest.raises(execute.ExecuteError) as exc:
        execute.acquire(loaded, "widget", "macos", Path("/nonexistent"))
    message = str(exc.value)
    assert "registry" in message and "not built by this script" in message


def test_acquire_git_clones_the_pinned_tag_as_an_argv_list() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / "stage"
        calls: list[list[str]] = []

        def _fake_run(argv, **kwargs):
            calls.append(list(argv))
            Path(argv[-1]).mkdir(parents=True, exist_ok=True)
            return subprocess.CompletedProcess(list(argv), 0, "", "")

        with mock.patch.object(fetch_mod, "run", _fake_run):
            src_dir = execute.acquire(_loaded(), "widget", "windows", stage)

        assert src_dir == stage / "widget-9.9.9"
        assert len(calls) == 1
        argv = calls[0]
        assert isinstance(argv, list) and argv[0] == "git"
        # The tag's {version} placeholder must be substituted, not passed through.
        assert "v9.9.9" in argv
        assert "{version}" not in " ".join(argv)


# ---------------------------------------------------------------------------
# configure / build / install
# ---------------------------------------------------------------------------
def test_three_cmake_phases_are_three_argv_lists_no_pipelines() -> None:
    calls: list[list[str]] = []

    def _fake_run(argv, **kwargs):
        calls.append(list(argv))
        return subprocess.CompletedProcess(list(argv), 0, "", "")

    with mock.patch.object(execute, "run", _fake_run):
        execute.configure_build_install(
            Path("/src"), Path("/build"), ["-DCMAKE_BUILD_TYPE=Release"], jobs=4
        )

    assert len(calls) == 3
    configure, build, install = calls
    assert configure[:5] == ["cmake", "-S", "/src", "-B", "/build"]
    assert "-DCMAKE_BUILD_TYPE=Release" in configure
    assert build == ["cmake", "--build", "/build", "--parallel", "4"]
    assert install == ["cmake", "--install", "/build"]
    for argv in calls:
        assert isinstance(argv, list)
        assert not any("|" in element for element in argv)


def test_build_component_renders_manifest_argv_and_appends_extra_args() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        dist = tmp_path / "dist"
        stage = tmp_path / "stage"
        (dist / "lib").mkdir(parents=True)
        (dist / "lib" / "libwidget.a").write_bytes(b"ar")
        calls: list[list[str]] = []

        def _fake_run(argv, **kwargs):
            calls.append(list(argv))
            return subprocess.CompletedProcess(list(argv), 0, "", "")

        with mock.patch.object(execute, "acquire", return_value=stage / "widget-9.9.9"), \
                mock.patch.object(execute, "run", _fake_run):
            found = execute.build_component(
                _loaded(), "widget", "macos", "arm64", dist, stage,
                extra_args=["-DCMAKE_PREFIX_PATH=/vcpkg/arm64-osx"],
            )

        configure = calls[0]
        assert f"-DCMAKE_INSTALL_PREFIX={dist}" in configure
        assert "-DCMAKE_OSX_ARCHITECTURES=arm64" in configure
        # extra_args land AFTER the rendered argv and never replace it.
        assert configure[-1] == "-DCMAKE_PREFIX_PATH=/vcpkg/arm64-osx"
        assert found == [dist / "lib" / "libwidget.a"]


# ---------------------------------------------------------------------------
# verify_outputs -- "cmake --install exited 0" != "the artefact exists"
# ---------------------------------------------------------------------------
def test_verify_outputs_accepts_any_candidate_spelling() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        dist = Path(tmp)
        (dist / "lib").mkdir()
        (dist / "lib" / "widget.lib").write_bytes(b"ar")
        assert execute.verify_outputs(_loaded(), "widget", dist) == [dist / "lib" / "widget.lib"]


def test_verify_outputs_fails_red_when_a_required_output_is_absent() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        with pytest.raises(execute.ExecuteError) as exc:
            execute.verify_outputs(_loaded(), "widget", Path(tmp))
        message = str(exc.value)
        assert "static_lib" in message
        # Names every candidate it looked for, so the message is actionable.
        assert "libwidget.a" in message and "widget.lib" in message


def test_verify_outputs_tolerates_an_absent_optional_output() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        dist = Path(tmp)
        (dist / "lib").mkdir()
        (dist / "lib" / "libwidget.a").write_bytes(b"ar")
        # `tool` is required = false and absent: must not raise.
        assert execute.verify_outputs(_loaded(), "widget", dist) == [dist / "lib" / "libwidget.a"]


def test_build_component_propagates_a_non_zero_cmake_exit() -> None:
    """The exit status comes from CompletedProcess.returncode via run.run(),
    which raises SubprocessError -- never inferred from output text."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)

        def _failing_run(argv, **kwargs):
            from deps.run import SubprocessError

            raise SubprocessError(list(argv), 2, "", "configure error")

        with mock.patch.object(execute, "acquire", return_value=tmp_path / "src"), \
                mock.patch.object(execute, "run", _failing_run):
            from deps.run import SubprocessError

            with pytest.raises(SubprocessError):
                execute.build_component(
                    _loaded(), "widget", "macos", "arm64", tmp_path / "dist", tmp_path / "stage"
                )
