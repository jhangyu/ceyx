"""Unit tests for native/scripts/gen_linkage_table.py.

Run: python3 -m pytest native/scripts/tests/test_gen_linkage_table.py -q
"""
import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "native" / "scripts" / "gen_linkage_table.py"
COMMITTED_OUTPUT = REPO_ROOT / "native" / "deps" / "linkage_table.md"

spec = importlib.util.spec_from_file_location("gen_linkage_table", MODULE_PATH)
gen = importlib.util.module_from_spec(spec)
sys.modules["gen_linkage_table"] = gen
spec.loader.exec_module(gen)


def test_check_passes_against_committed_file():
    rc = gen.main(["--check"])
    assert rc == 0


def test_check_detects_staleness_negative_control(tmp_path):
    stale = tmp_path / "linkage_table.md"
    stale.write_text("stale content\n")
    rc = gen.main(["--output", str(stale), "--check"])
    assert rc == 1


def test_static_components_exclude_halide():
    names = gen.load_static_components()
    assert "libwebp" in names
    assert "libjxl" in names
    assert "libraw" in names
    assert "aom" in names
    assert "kvazaar" in names
    assert "halide" not in names
    assert "libheif" not in names  # shared, not static
    assert "libde265" not in names  # shared, not static


def test_capabilities_cell_is_cross_reference_only_no_values():
    import read_shipped_files as rsf
    platforms = rsf.load_declaration()
    rendered = gen.render(platforms, gen.load_static_components(), pin=None)
    assert "codec_expectations.toml" in rendered
    # No capability value tokens (e.g. "jpeg:decode") duplicated into this table.
    assert ":encode" not in rendered
    assert ":decode" not in rendered


def test_column2_equals_pin_true_when_sets_match():
    entry = {"decoder": "d.so", "companions": ["a.so", "b.so"]}
    pin = {"assets": {"target": {"libraries": [
        {"artifact": "d.so"}, {"artifact": "a.so"}, {"artifact": "b.so"},
    ]}}}
    gen._PLATFORM_TO_PIN_TARGET["_test_platform"] = "target"
    try:
        status, equal = gen.column2_equals_pin("_test_platform", entry, pin)
        assert equal is True
        assert status == "True"
    finally:
        del gen._PLATFORM_TO_PIN_TARGET["_test_platform"]


def test_column2_equals_pin_false_negative_control():
    # Negative control: proves the equality check actually detects a
    # mismatched set (R8 pairing with the True case above).
    entry = {"decoder": "d.so", "companions": ["a.so", "b.so", "missing.so"]}
    pin = {"assets": {"target": {"libraries": [
        {"artifact": "d.so"}, {"artifact": "a.so"}, {"artifact": "b.so"},
    ]}}}
    gen._PLATFORM_TO_PIN_TARGET["_test_platform"] = "target"
    try:
        status, equal = gen.column2_equals_pin("_test_platform", entry, pin)
        assert equal is False
        assert status == "False"
    finally:
        del gen._PLATFORM_TO_PIN_TARGET["_test_platform"]


def test_column2_equals_pin_skip_when_pin_absent():
    status, equal = gen.column2_equals_pin("windows", {"decoder": "x", "companions": []}, None)
    assert equal is None
    assert "SKIP" in status


def test_column2_equals_pin_line_always_printed(capsys):
    # The plan is explicit: "a silent skip is a FAIL" -- the line must be
    # printed unconditionally, never omitted.
    rc = gen.main(["--check"])
    captured = capsys.readouterr()
    for platform in ("windows", "linux", "macos", "android"):
        assert f"COLUMN2_EQUALS_PIN({platform})=" in captured.out
    assert rc == 0
