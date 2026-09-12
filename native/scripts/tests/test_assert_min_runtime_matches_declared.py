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
