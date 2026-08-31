"""CLI-surface tests for native/scripts/build_deps.py (R4).

Two things are pinned here:

1. **The frozen interface contract.** `build <component> --platform <p>
   --arch <a> --dist <dir>` exists, `--arch` speaks the arch_map vocabulary
   (`x86_64`, never `x64`), and `--dist` actually displaces render.py's
   built-in DEFAULT_DIST constants. impl-carrier-win-opus's Windows modules
   are built against exactly this surface, so a change here breaks them.

2. **The `--dist` cross-platform path defect** found while wiring the
   Windows dispatch: an unconditional `Path(...).resolve()` prepended the
   host's Unix cwd to a drive-lettered Windows path, which PureWindowsPath
   then rendered as `\\Users\\...\\C:\\ceyx\\dist`. Same drive-letter-eating
   family as the 2026-08-30 shell finding, this time self-inflicted. These
   tests fail if it comes back.
"""
from __future__ import annotations

import sys
from pathlib import Path, PureWindowsPath

import pytest

_SCRIPTS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_SCRIPTS))

import build_deps  # noqa: E402


# ---------------------------------------------------------------------------
# resolve_dist -- the defect this file was created for
# ---------------------------------------------------------------------------
def test_windows_dist_keeps_its_drive_letter_when_rendered_from_a_unix_host() -> None:
    """spec §8.1: a developer on macOS must be able to inspect the exact
    Windows command line. Resolving against the host cwd destroyed it."""
    resolved = build_deps.resolve_dist("C:/ceyx/dist", "windows")
    # Both assertions verified to FAIL against the previous
    # `Path(raw).resolve()` implementation, which produced
    # `\\Users\\jhangyu\\project\\ceyx-wt-round3\\C:\\ceyx\\dist`.
    # (A colon-count check was tried here and DROPPED: the corrupt value also
    # contains exactly one colon, so it discriminates nothing and would have
    # been false comfort.)
    assert str(PureWindowsPath(resolved)) == r"C:\ceyx\dist"
    assert "ceyx-wt" not in str(resolved)  # no host cwd prepended


def test_windows_dist_with_backslashes_survives_too() -> None:
    resolved = build_deps.resolve_dist(r"D:\a\ceyx\dist", "windows")
    assert str(PureWindowsPath(resolved)) == r"D:\a\ceyx\dist"


def _foreign_platform() -> str:
    """A target platform that is NOT the host's.

    Hardcoding "windows" here was a latent Windows-only failure: on a Windows
    host that target is the SAME platform, so resolve_dist() correctly
    resolves instead of raising and the test would have failed on the Windows
    CI leg alone. Same green-on-macOS/red-on-Windows shape as the `/src`
    literal in test_execute.py, found by sweeping for siblings after CI caught
    that one rather than by CI catching this one too.
    """
    return "linux" if sys.platform == "win32" else "windows"


def test_relative_dist_for_a_foreign_target_is_rejected_not_guessed() -> None:
    """There is no meaningful cwd on the other platform, so a plausible
    looking wrong path is the worst possible outcome."""
    with pytest.raises(ValueError) as exc:
        build_deps.resolve_dist("relative/dir", _foreign_platform())
    assert "relative" in str(exc.value)


def test_windows_host_branch_is_exercised_from_any_host() -> None:
    """Cover the Windows-HOST half of resolve_dist's branch from a Unix box.

    Without this the branch only ever executes on the Windows CI leg — which
    is exactly how both defects in this file's history got there. The host is
    INJECTED rather than simulated by patching `os.name`: that was tried
    first and mutates a shared global, changing pathlib's behaviour
    process-wide (`Path.resolve()` starts raising UnsupportedOperation). Same
    reason the suite never patches os.environ/subprocess globally.
    """
    # Windows host + posix target = foreign: an absolute path passes through...
    foreign = build_deps.resolve_dist("/opt/ceyx-dist", "linux", host_is_windows=True)
    assert str(foreign).replace("\\", "/") == "/opt/ceyx-dist"

    # ...and a relative one is rejected, not resolved against a Windows cwd.
    with pytest.raises(ValueError):
        build_deps.resolve_dist("relative/dir", "linux", host_is_windows=True)

    # The Windows-host + Windows-target (same-platform) case is NOT asserted
    # here: it takes the resolve() path, whose output on this Unix box would
    # be a Unix absolute path and would prove nothing about Windows. The
    # equivalent same-platform behaviour is covered by
    # test_same_platform_dist_is_still_resolved_absolutely.


def test_same_platform_dist_is_still_resolved_absolutely(tmp_path: Path) -> None:
    """The convenience behaviour must survive the fix: a relative --dist for
    the HOST platform still resolves against the cwd."""
    host_platform = build_deps.detect_platform()
    resolved = build_deps.resolve_dist(str(tmp_path), host_platform)
    assert resolved.is_absolute()
    assert resolved == tmp_path.resolve()


# ---------------------------------------------------------------------------
# The frozen contract surface
# ---------------------------------------------------------------------------
def test_arch_uses_the_arch_map_vocabulary_and_rejects_the_x64_shorthand() -> None:
    parser = build_deps.build_subcommand_parser()
    args = parser.parse_args(["heif-stack", "--dist", "/d", "--arch", "x86_64"])
    assert args.arch == "x86_64"
    with pytest.raises(SystemExit):
        parser.parse_args(["heif-stack", "--dist", "/d", "--arch", "x64"])


def test_dist_is_required() -> None:
    with pytest.raises(SystemExit):
        build_deps.build_subcommand_parser().parse_args(["heif-stack"])


def test_dist_displaces_render_default_dist_for_every_platform(capsys) -> None:
    """render.DEFAULT_DIST exists only to keep render() total; a real
    invocation must never leak it into the argv."""
    for platform, dist in (
        ("macos", "/tmp/ceyx-x"),
        ("linux", "/tmp/ceyx-x"),
        ("windows", "C:/ceyx-x"),
    ):
        rc = build_deps.main(
            ["build", "heif-stack", "--platform", platform, "--arch", "x86_64",
             "--dist", dist, "--dry-run"]
        )
        assert rc == 0
        out = capsys.readouterr().out
        assert "ceyx-x" in out
        from deps import render as render_mod

        assert render_mod.DEFAULT_DIST[platform] not in out


# ---------------------------------------------------------------------------
# Platform dispatch of the heif-stack pseudo-component
# ---------------------------------------------------------------------------
def test_windows_heif_stack_dry_run_renders_the_decode_only_pair(capsys) -> None:
    """Windows is decode-only (libde265 + libheif). kvazaar/aom belong to the
    Unix stack and must NOT appear -- their presence would mean the dispatch
    fell through to the Unix implementation."""
    rc = build_deps.main(
        ["build", "heif-stack", "--platform", "windows", "--arch", "x86_64",
         "--dist", "C:/ceyx-dist", "--dry-run"]
    )
    assert rc == 0
    out = capsys.readouterr().out
    assert "# libde265" in out and "# libheif" in out
    assert "# kvazaar" not in out and "# aom" not in out


def test_unix_heif_stack_dry_run_renders_the_encode_enabled_pair(capsys) -> None:
    rc = build_deps.main(
        ["build", "heif-stack", "--platform", "macos", "--arch", "arm64",
         "--dist", "/tmp/ceyx-dist", "--dry-run"]
    )
    assert rc == 0
    out = capsys.readouterr().out
    assert "# kvazaar" in out and "# libheif" in out


def test_stage_flag_is_rejected_on_windows_rather_than_silently_ignored(capsys) -> None:
    """The Windows dist is built in one pass. Accepting --stage and ignoring
    it would let a caller believe they built one component when they built
    everything (or nothing)."""
    rc = build_deps.main(
        ["build", "heif-stack", "--platform", "windows", "--arch", "x86_64",
         "--dist", "C:/ceyx-dist", "--stage", "libheif"]
    )
    assert rc == 1
    assert "no stage split" in capsys.readouterr().err


def test_force_flag_is_rejected_on_unix_rather_than_silently_ignored(capsys) -> None:
    rc = build_deps.main(
        ["build", "heif-stack", "--platform", "macos", "--arch", "arm64",
         "--dist", "/tmp/ceyx-dist", "--force"]
    )
    assert rc == 1
    assert "Windows-only" in capsys.readouterr().err


def test_unknown_component_is_rejected_and_lists_what_is_known(capsys) -> None:
    rc = build_deps.main(
        ["build", "nonesuch", "--platform", "macos", "--arch", "arm64", "--dist", "/tmp/d"]
    )
    assert rc == 1
    err = capsys.readouterr().err
    assert "nonesuch" in err and "heif-stack" in err


def test_legacy_component_form_still_works(capsys) -> None:
    """The pre-R4 surface must not have moved: other callers may use it."""
    rc = build_deps.main(["--component", "kvazaar", "--platform", "linux",
                          "--arch", "x86_64", "--dry-run"])
    assert rc == 0
    assert "-DBUILD_SHARED_LIBS=OFF" in capsys.readouterr().out


# ---------------------------------------------------------------------------
# fetch <name> arg strictness (round 3, review nit 5): --arch/--force are
# documented per-module-only in fetch_subcommand_parser()'s help text; a
# caller passing either for a module that ignores it silently gets a build
# that doesn't do what the flag implied. Same "rejected rather than silently
# ignored" doctrine as --stage/--force above for `build heif-stack`.
# ---------------------------------------------------------------------------
def test_fetch_halide_rejects_arch(capsys) -> None:
    rc = build_deps.main(["fetch", "halide", "--arch", "x86_64"])
    assert rc == 1
    err = capsys.readouterr().err
    assert "--arch is not accepted for fetch 'halide'" in err


def test_fetch_libraw_rejects_arch(capsys) -> None:
    rc = build_deps.main(["fetch", "libraw", "--arch", "arm64"])
    assert rc == 1
    err = capsys.readouterr().err
    assert "--arch is not accepted for fetch 'libraw'" in err


def test_fetch_libraw_rejects_force(capsys) -> None:
    rc = build_deps.main(["fetch", "libraw", "--force"])
    assert rc == 1
    err = capsys.readouterr().err
    assert "--force is not accepted for fetch 'libraw'" in err


def test_fetch_libjxl_still_accepts_arch_and_force(monkeypatch, tmp_path: Path) -> None:
    """Sanity check that the new rejections are scoped to halide/libraw only
    -- libjxl's documented use of both flags must keep working."""
    from deps import fetch_libjxl  # noqa: PLC0415

    calls = {}

    def _fake_build(dest, *, arch=None, force=False):
        calls["dest"] = dest
        calls["arch"] = arch
        calls["force"] = force
        return dest

    monkeypatch.setattr(fetch_libjxl, "build", _fake_build)
    rc = build_deps.main(
        ["fetch", "libjxl", "--dest", str(tmp_path / "libjxl"), "--arch", "x86_64", "--force"]
    )
    assert rc == 0
    assert calls == {"dest": tmp_path.resolve() / "libjxl", "arch": "x86_64", "force": True}


def test_fetch_rejects_nonexistent_dest_parent(tmp_path: Path, capsys) -> None:
    """A --dest whose parent directory does not exist must fail fast with a
    FetchError-style message, not a raw traceback surfaced from deep inside
    the acquisition module (e.g. a bare mkdir/shutil.move failure)."""
    bad_dest = tmp_path / "does-not-exist-yet" / "halide"
    rc = build_deps.main(["fetch", "halide", "--dest", str(bad_dest)])
    assert rc == 1
    err = capsys.readouterr().err
    assert "non-existent parent directory" in err
    assert str(bad_dest.parent) in err
