"""Unit tests for native/scripts/coverage_matrix.py.

Run: python3 -m pytest native/scripts/tests/test_coverage_matrix.py -q
"""
import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPTS_DIR = REPO_ROOT / "native" / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

spec = importlib.util.spec_from_file_location("coverage_matrix", SCRIPTS_DIR / "coverage_matrix.py")
cm_mod = importlib.util.module_from_spec(spec)
sys.modules["coverage_matrix"] = cm_mod
spec.loader.exec_module(cm_mod)

from render_expectations import load_ledger  # noqa: E402


def test_build_matrix_from_real_ledger_includes_android_honest_gap():
    legs = load_ledger()
    columns, rows = cm_mod.build_matrix(legs)
    rows_by_leg = dict(rows)
    assert "android-arm64-v8a" in rows_by_leg
    android_cells = rows_by_leg["android-arm64-v8a"]
    assert all(cell == "0 (none (accepted gap))" for cell in android_cells.values())


def test_build_matrix_other_legs_not_degraded():
    legs = load_ledger()
    columns, rows = cm_mod.build_matrix(legs)
    rows_by_leg = dict(rows)
    linux = rows_by_leg["linux-x86_64"]
    assert linux["heic:decode"] == "1"
    assert linux["jpeg:decode"].startswith("0 (")  # deliberate cross-platform 0, unchanged


def test_render_markdown_has_header_and_all_legs():
    legs = load_ledger()
    columns, rows = cm_mod.build_matrix(legs)
    text = cm_mod.render_markdown(columns, rows)
    assert text.startswith("| leg |")
    for leg in legs:
        assert leg in text


def test_main_runs_rc0_to_stdout(capsys):
    rc = cm_mod.main([])
    assert rc == 0
    out = capsys.readouterr().out
    assert "android-arm64-v8a" in out


def test_main_writes_to_out_file(tmp_path):
    out_path = tmp_path / "matrix.md"
    rc = cm_mod.main(["--out", str(out_path)])
    assert rc == 0
    assert out_path.exists()
    assert "android-arm64-v8a" in out_path.read_text()
