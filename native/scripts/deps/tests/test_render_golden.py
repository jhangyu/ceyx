"""render.py tests -- Plan D2 acceptance A2.1-A2.3.

A2.4 (`build_deps.py --dry-run ... --platform windows`) is D3's CLI, owned by
M3, and is out of this deliverable's file ownership; this suite tests
`render()` directly, which is what A2.4's CLI will call.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from deps import manifest, render  # noqa: E402

_GOLDEN_DIR = Path(__file__).resolve().parent / "golden"
_COMPONENTS = ("kvazaar", "libde265", "aom", "libheif")
# Round 2: libwebp and libjxl have a `cmake` block (render()-able) but no live
# Windows acquisition script exists for either -- see their manifest.toml
# comments. Their golden combos are therefore macOS/Linux only; adding a
# "windows" combo would assert a build that does not exist today (red line:
# do not invent a build step). libraw and halide have NO `cmake` block at all
# (libraw is add_subdirectory'd; halide is download-only) and are excluded
# from render() entirely -- there is nothing for the renderer to produce.
_RENDERABLE_COMPONENTS = ("kvazaar", "libde265", "aom", "libheif", "libwebp", "libjxl")
_ALL_PLATFORM_ARCH = (("macos", "arm64"), ("macos", "x86_64"), ("linux", "x86_64"), ("windows", "x86_64"))
_MACOS_LINUX_ONLY = (("macos", "arm64"), ("macos", "x86_64"), ("linux", "x86_64"))
_PLATFORM_ARCH_BY_COMPONENT = {
    "libwebp": _MACOS_LINUX_ONLY,
    "libjxl": _MACOS_LINUX_ONLY,
}
_COMBOS = [
    (comp, platform, arch)
    for comp in _RENDERABLE_COMPONENTS
    for platform, arch in _PLATFORM_ARCH_BY_COMPONENT.get(comp, _ALL_PLATFORM_ARCH)
]


@pytest.fixture(scope="module")
def loaded():
    return manifest.load()


# --- A2.1: render() is a pure function --------------------------------------


def test_a2_1_render_source_never_references_subprocess_or_cwd():
    """Static check, deliberately not a runtime monkeypatch of `subprocess`/
    `Path.cwd`: those are process-wide, and pytest itself uses both
    internally (capture, path resolution), so poisoning them globally breaks
    the TEST RUNNER rather than isolating render.py (the same failure mode
    that made the os.environ runtime-poisoning approach below unsafe --
    observed for real as a pytest INTERNALERROR, not a render.py defect).
    A source-text check gives the same guarantee without touching shared
    interpreter state."""
    assert "subprocess" not in render.__dict__
    assert not any(hasattr(v, "__module__") and v.__module__ == "subprocess" for v in vars(render).values())
    import inspect

    for _, obj in inspect.getmembers(render, inspect.isfunction):
        if obj.__module__ != render.__name__:
            continue
        code_text = inspect.getsource(obj)
        # Strip the function's own docstring before scanning its BODY for a
        # subprocess call or a Path.cwd() call -- the module/function
        # docstrings legitimately discuss subprocess in prose.
        body_only = re.sub(r'""".*?"""', "", code_text, flags=re.S)
        assert "subprocess" not in body_only, f"{obj.__qualname__} references subprocess"
        assert ".cwd(" not in body_only, f"{obj.__qualname__} calls .cwd()"


def test_a2_1_render_returns_correct_shape_for_every_combo(loaded):
    for comp, platform, arch in _COMBOS:
        argv = render.render(loaded, comp, platform, arch)
        assert isinstance(argv, list)
        assert all(isinstance(x, str) for x in argv)


def test_a2_1_render_never_reads_os_environ():
    """render.py's own source never references os.environ or os.getenv --
    static check, deliberately not a runtime os.environ monkeypatch: globally
    poisoning os.environ during the test also intercepts pytest's OWN
    environment reads (e.g. its TerminalWriter probing COLUMNS/PY_COLORS
    while formatting a failure), which produces a false, misleading crash
    unrelated to render.py's behaviour (observed for real: INTERNALERROR
    triggered by this exact pattern). A static source-text check gives the
    same guarantee -- render.py imports neither `os` nor `os.environ` at
    all -- without touching global interpreter state."""
    import inspect

    for _, obj in inspect.getmembers(render, inspect.isfunction):
        if obj.__module__ != render.__name__:
            continue
        body_only = re.sub(r'""".*?"""', "", inspect.getsource(obj), flags=re.S)
        assert "os.environ" not in body_only
        assert "os.getenv" not in body_only
    module_source = inspect.getsource(render)
    import_lines = [ln for ln in module_source.splitlines() if ln.strip().startswith(("import ", "from "))]
    assert not any(ln.strip() in ("import os",) or ln.strip().startswith("from os import") for ln in import_lines)


# --- A2.2: rendered argv equals the golden file -----------------------------


@pytest.mark.parametrize("comp,platform,arch", _COMBOS)
def test_a2_2_rendered_argv_matches_golden(loaded, comp, platform, arch):
    golden_path = _GOLDEN_DIR / f"{comp}.{platform}.{arch}.argv"
    assert golden_path.exists(), f"missing golden file {golden_path}"
    expected = golden_path.read_text().splitlines()
    actual = render.render(loaded, comp, platform, arch)
    assert actual == expected


# --- A2.3: path lints on the rendered Windows argv --------------------------

_DRIVE_LETTER_RE = re.compile(r"^[A-Za-z]:[\\/]")
_CLANG_PASSTHROUGH_RE = re.compile(r"^/clang:")


def _declared_path_keys(loaded, comp):
    return set(loaded["manifest"]["component"][comp].get("path_keys", []))


@pytest.mark.parametrize("comp", _COMPONENTS)
def test_a2_3_windows_path_elements_have_drive_letters(loaded, comp):
    argv = render.render(loaded, comp, "windows", "x86_64")
    path_keys = _declared_path_keys(loaded, comp)
    for entry in argv:
        if not entry.startswith("-D"):
            continue
        key, _, value = entry[2:].partition("=")
        if key in path_keys:
            assert _DRIVE_LETTER_RE.match(value), f"{comp}: {key}={value!r} has no drive letter"


@pytest.mark.parametrize("comp", _COMPONENTS)
def test_a2_3_no_stray_leading_slash_other_than_clang_passthrough(loaded, comp):
    argv = render.render(loaded, comp, "windows", "x86_64")
    for entry in argv:
        assert isinstance(entry, str)
        # An argv element itself may legitimately start with "-D"/"-G"; the
        # lint targets the VALUE portion of a -D flag for a bare leading "/"
        # (MSYS's path-rewrite trigger, handoff §A), except clang-cl's own
        # `/clang:-mXXX` passthrough spelling.
        if entry.startswith("-D") and "=" in entry:
            _, _, value = entry.partition("=")
            for token in value.split(" "):
                if token.startswith("/") and not _CLANG_PASSTHROUGH_RE.match(token):
                    pytest.fail(f"{entry!r}: bare leading-slash token {token!r} (layer 1/handoff §A)")


@pytest.mark.parametrize("comp", _COMPONENTS)
def test_a2_3_no_element_contains_undeclared_semicolon(loaded, comp):
    """Every argv element is a plain str (never a nested list); a ';' is only
    legitimate inside a value the manifest declared as a path-list key
    (CMAKE_IGNORE_PREFIX_PATH)."""
    argv = render.render(loaded, comp, "windows", "x86_64")
    path_list_keys = set(loaded["manifest"]["component"][comp].get("path_list_keys", []))
    for entry in argv:
        assert isinstance(entry, str)
        key = entry[2:].partition("=")[0] if entry.startswith("-D") else None
        if ";" in entry:
            assert key in path_list_keys, f"undeclared ';' in {entry!r}"


def test_a2_3_red_demonstration_bare_leading_slash_is_caught():
    """Demonstrated red (A2.3): a deliberately malformed manifest entry with a
    bare leading-slash flag baked into CMAKE_C_FLAGS must be caught by the
    lint above, proving the lint actually fires rather than vacuously
    passing."""
    loaded_bad = {
        "manifest": {
            "component": {
                "widget": {
                    "path_keys": [],
                    "path_list_keys": [],
                    "cmake": {
                        "base": {},
                        "windows": {"CMAKE_C_FLAGS": ["/DWIN32", "/clang:-msse4.1"]},
                    },
                }
            }
        },
        "arch_map": {"x86_64": {"apple": "x86_64", "aom_target_cpu": "x86_64"}},
    }
    argv = render.render(loaded_bad, "widget", "windows", "x86_64")
    entry = argv[0]
    _, _, value = entry.partition("=")
    bad_tokens = [
        t for t in value.split(" ") if t.startswith("/") and not _CLANG_PASSTHROUGH_RE.match(t)
    ]
    assert bad_tokens == ["/DWIN32"], "the lint's own fixture must contain exactly one bad token"


# --- Error paths -------------------------------------------------------------


def test_render_unknown_component_raises(loaded):
    with pytest.raises(render.RenderError):
        render.render(loaded, "no-such-component", "macos", "arm64")


def test_render_unknown_platform_raises(loaded):
    with pytest.raises(render.RenderError):
        render.render(loaded, "kvazaar", "atari", "arm64")


def test_render_unknown_arch_raises(loaded):
    with pytest.raises(render.RenderError):
        render.render(loaded, "kvazaar", "macos", "sparc")
