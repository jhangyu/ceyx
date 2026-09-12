"""Unit tests for native/scripts/read_shipped_files.py.

Run: python3 -m pytest native/scripts/tests/test_read_shipped_files.py -q
"""
import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "native" / "scripts" / "read_shipped_files.py"

spec = importlib.util.spec_from_file_location("read_shipped_files", MODULE_PATH)
rsf = importlib.util.module_from_spec(spec)
sys.modules["read_shipped_files"] = rsf
spec.loader.exec_module(rsf)


def _write_declaration(tmp_path, content):
    path = tmp_path / "shipped_files.toml"
    path.write_text(content)
    return path


def test_load_real_declaration_succeeds():
    platforms = rsf.load_declaration()
    for platform in ("windows", "linux", "macos", "android"):
        assert platform in platforms
    assert platforms["windows"]["decoder"] == "dng_decoder_native.dll"
    assert "libomp140.x86_64.dll" in platforms["windows"]["companions"]
    # lcms2 is REMOVED as of WI-5 (OQ-N4 option Z, user ruling 2026-09-12):
    # lcms2 is dead code on every platform, ENABLE_LCMS is forced OFF, and
    # this declaration is authored from the post-removal state.
    assert "liblcms2.2.dylib" not in platforms["macos"]["companions"]
    assert platforms["android"]["placed"] is False
    assert platforms["android"]["not_placed_reason"]


def test_unknown_platform_exit_names_known_keys(capsys):
    rc = rsf.main(["--platform", "bogus", "--decoder"])
    assert rc == 2
    captured = capsys.readouterr()
    assert "unknown platform 'bogus'" in captured.err
    for platform in ("windows", "linux", "macos", "android"):
        assert platform in captured.err


def test_placed_false_without_reason_rejected(tmp_path):
    path = _write_declaration(tmp_path, '''
[android]
decoder = "libdng_decoder_native.so"
companions = ["libheif.so", "libde265.so"]
staged_from = "native/build-android"
placed = false
source = "test"
''')
    try:
        rsf.load_declaration(path)
        assert False, "expected DeclarationError"
    except rsf.DeclarationError as exc:
        assert "not_placed_reason" in str(exc)


def test_companions_output_space_separated(capsys):
    rc = rsf.main(["--platform", "linux", "--companions"])
    assert rc == 0
    captured = capsys.readouterr()
    assert captured.out.strip() == "libheif.so.1 libde265.so.0"


def test_all_output_prepends_decoder(capsys):
    rc = rsf.main(["--platform", "android", "--all"])
    assert rc == 0
    captured = capsys.readouterr()
    assert captured.out.strip() == "libdng_decoder_native.so libheif.so libde265.so"


def test_json_output_is_valid(capsys):
    import json
    rc = rsf.main(["--platform", "macos", "--json"])
    assert rc == 0
    captured = capsys.readouterr()
    parsed = json.loads(captured.out)
    assert parsed["decoder"] == "libdng_decoder_native.dylib"
    assert parsed["placed"] is True
