"""Tests for assert_min_runtime_matches_declared.py (WI-14 step 14.4, S-F3)."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import assert_min_runtime_matches_declared as amr  # noqa: E402


def _declared(tmp_path, platform, value):
    p = tmp_path / "min_runtime_expected.toml"
    p.write_text(f'[{platform}]\nvalue = "{value}"\nmeasured_on = "2026-09-12"\nreason = "test"\n')
    return p


def _emitted(tmp_path, lines):
    p = tmp_path / "min_runtime.txt"
    p.write_text("\n".join(lines) + "\n")
    return p


def _invoke(monkeypatch, argv, capsys):
    monkeypatch.setattr(sys, "argv", ["assert_min_runtime_matches_declared.py"] + argv)
    rc = amr.main()
    out = capsys.readouterr()
    return rc, out.out, out.err


def test_match_passes(tmp_path, monkeypatch, capsys):
    declared = _declared(tmp_path, "macos", "15.0")
    emitted = _emitted(tmp_path, ["MIN_RUNTIME_macos=15.0", "  decoder=14.0"])
    rc, out, err = _invoke(monkeypatch, [
        "--emitted", str(emitted), "--declared", str(declared), "--platform", "macos",
    ], capsys)
    assert rc == 0
    assert "MIN_RUNTIME_DRIFT_RESULT=PASS" in out


def test_windows_extra_provenance_line_ignored(tmp_path, monkeypatch, capsys):
    declared = _declared(tmp_path, "windows", "6.0")
    emitted = _emitted(tmp_path, ["MIN_RUNTIME_windows=6.0", "PROVENANCE=linker-default"])
    rc, out, err = _invoke(monkeypatch, [
        "--emitted", str(emitted), "--declared", str(declared), "--platform", "windows",
    ], capsys)
    assert rc == 0


def test_mismatch_fails_naming_both(tmp_path, monkeypatch, capsys):
    """S-F3 red: edit the declared value, the leg fails naming both values."""
    declared = _declared(tmp_path, "windows", "10.0")
    emitted = _emitted(tmp_path, ["MIN_RUNTIME_windows=6.0", "PROVENANCE=linker-default"])
    rc, out, err = _invoke(monkeypatch, [
        "--emitted", str(emitted), "--declared", str(declared), "--platform", "windows",
    ], capsys)
    assert rc == 1
    assert "MIN_RUNTIME_DRIFT_RESULT=FAIL" in out
    assert "measured MIN_RUNTIME_windows=6.0" in err
    assert "declared" in err and "10.0" in err


def test_missing_emitted_file_fails(tmp_path, monkeypatch, capsys):
    declared = _declared(tmp_path, "linux", "2.35")
    rc, out, err = _invoke(monkeypatch, [
        "--emitted", str(tmp_path / "nope.txt"), "--declared", str(declared), "--platform", "linux",
    ], capsys)
    assert rc == 1
    assert "MIN_RUNTIME_DRIFT_RESULT=FAIL" in err


def test_missing_platform_in_declared_fails(tmp_path, monkeypatch, capsys):
    declared = _declared(tmp_path, "linux", "2.35")
    emitted = _emitted(tmp_path, ["MIN_RUNTIME_android=21", "SOURCE=gradle-declaration"])
    rc, out, err = _invoke(monkeypatch, [
        "--emitted", str(emitted), "--declared", str(declared), "--platform", "android",
    ], capsys)
    assert rc == 1
    assert "MIN_RUNTIME_DRIFT_RESULT=FAIL" in err


# --- per-arch declarations (added 2026-09-12, CI run 34697591379) -----------
# The macOS floor is arch-dependent: the arm64 leg bundles libomp.dylib at
# 15.0, the x86_64 cross leg the vendored libomp-x86_64 at 14.0. A single
# scalar made the x86_64 leg compare itself against the arm64 floor and fail.

PER_ARCH_MACOS = (
    '[macos.arm64]\nvalue = "15.0"\nmeasured_on = "2026-09-12"\nreason = "t"\n'
    '\n[macos.x86_64]\nvalue = "14.0"\nmeasured_on = "2026-09-12"\nreason = "t"\n'
)


def _declared_text(tmp_path, text):
    p = tmp_path / "min_runtime_expected.toml"
    p.write_text(text)
    return p


def test_per_arch_x86_64_passes_against_its_own_floor(tmp_path, monkeypatch, capsys):
    """The exact case that was red in CI: measured 14.0 on the x86_64 leg."""
    declared = _declared_text(tmp_path, PER_ARCH_MACOS)
    emitted = _emitted(tmp_path, ["MIN_RUNTIME_macos=14.0", "  decoder=14.0"])
    rc, out, err = _invoke(monkeypatch, [
        "--emitted", str(emitted), "--declared", str(declared),
        "--platform", "macos", "--arch", "x86_64",
    ], capsys)
    assert rc == 0
    assert "MIN_RUNTIME_DRIFT_RESULT=PASS" in out
    assert "[macos.x86_64]" in out


def test_per_arch_arm64_still_holds_the_higher_floor(tmp_path, monkeypatch, capsys):
    """The arm64 value was NOT lowered to 14.0: 14.0 on arm64 is still drift."""
    declared = _declared_text(tmp_path, PER_ARCH_MACOS)
    emitted = _emitted(tmp_path, ["MIN_RUNTIME_macos=14.0"])
    rc, out, err = _invoke(monkeypatch, [
        "--emitted", str(emitted), "--declared", str(declared),
        "--platform", "macos", "--arch", "arm64",
    ], capsys)
    assert rc == 1
    assert "MIN_RUNTIME_DRIFT_RESULT=FAIL" in out
    assert "[macos.arm64].value=15.0" in err


def test_per_arch_platform_without_arch_flag_fails_loudly(tmp_path, monkeypatch, capsys):
    """Dropping --arch must not silently fall back to some default floor."""
    declared = _declared_text(tmp_path, PER_ARCH_MACOS)
    emitted = _emitted(tmp_path, ["MIN_RUNTIME_macos=14.0"])
    rc, out, err = _invoke(monkeypatch, [
        "--emitted", str(emitted), "--declared", str(declared), "--platform", "macos",
    ], capsys)
    assert rc == 1
    assert "PER-ARCH" in err
    assert "MIN_RUNTIME_DRIFT_RESULT=FAIL" in err


def test_per_arch_unknown_arch_fails(tmp_path, monkeypatch, capsys):
    declared = _declared_text(tmp_path, PER_ARCH_MACOS)
    emitted = _emitted(tmp_path, ["MIN_RUNTIME_macos=14.0"])
    rc, out, err = _invoke(monkeypatch, [
        "--emitted", str(emitted), "--declared", str(declared),
        "--platform", "macos", "--arch", "riscv64",
    ], capsys)
    assert rc == 1
    assert "riscv64" in err


def test_mixed_scalar_and_arch_tables_is_rejected_as_ambiguous(tmp_path, monkeypatch, capsys):
    """Guard against a half-migrated declaration: a leftover scalar next to
    arch subtables would let a caller that forgot --arch read the old value."""
    declared = _declared_text(
        tmp_path, '[macos]\nvalue = "15.0"\n\n[macos.x86_64]\nvalue = "14.0"\n')
    emitted = _emitted(tmp_path, ["MIN_RUNTIME_macos=15.0"])
    rc, out, err = _invoke(monkeypatch, [
        "--emitted", str(emitted), "--declared", str(declared), "--platform", "macos",
    ], capsys)
    assert rc == 1
    assert "BOTH" in err


def test_arch_flag_is_harmless_on_an_arch_independent_platform(tmp_path, monkeypatch, capsys):
    """windows/linux/android keep their single scalar; passing --arch must not
    invent a [windows.x86_64] key in the message."""
    declared = _declared(tmp_path, "windows", "6.0")
    emitted = _emitted(tmp_path, ["MIN_RUNTIME_windows=6.0", "PROVENANCE=linker-default"])
    rc, out, err = _invoke(monkeypatch, [
        "--emitted", str(emitted), "--declared", str(declared),
        "--platform", "windows", "--arch", "x86_64",
    ], capsys)
    assert rc == 0
    assert "[windows]" in out
    assert "[windows.x86_64]" not in out


def test_real_declaration_file_is_resolvable_for_every_shipped_leg():
    """Integration check against the REAL committed declaration + the arch
    tags the workflows actually pass (macos_build.yml matrix.arch_tag)."""
    import tomllib
    real = pathlib.Path(__file__).resolve().parents[2] / "deps" / "min_runtime_expected.toml"
    with real.open("rb") as fh:
        data = tomllib.load(fh)
    assert amr.resolve_declared(data, "macos", "arm64") == "15.0"
    assert amr.resolve_declared(data, "macos", "x86_64") == "14.0"
    for platform in ("windows", "linux", "android"):
        assert amr.resolve_declared(data, platform) is not None
