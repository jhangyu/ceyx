"""Unit tests for native/scripts/gen_shipped_files_cmake.py.

Run: python3 -m pytest native/scripts/tests/test_gen_shipped_files_cmake.py -q
"""
import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "native" / "scripts" / "gen_shipped_files_cmake.py"
COMMITTED_OUTPUT = REPO_ROOT / "native" / "cmake" / "shipped_files.cmake"

spec = importlib.util.spec_from_file_location("gen_shipped_files_cmake", MODULE_PATH)
gen = importlib.util.module_from_spec(spec)
sys.modules["gen_shipped_files_cmake"] = gen
spec.loader.exec_module(gen)


def test_check_passes_against_committed_file():
    rc = gen.main(["--check"])
    assert rc == 0


def test_check_detects_staleness_negative_control(tmp_path):
    # Positive control (above) proves --check stays silent when up to date;
    # this negative control proves it actually detects drift (R8 pairing).
    stale_output = tmp_path / "shipped_files.cmake"
    stale_output.write_text("set(CEYX_SHIPPED_WINDOWS_DECODER \"wrong.dll\")\n")
    rc = gen.main(["--output", str(stale_output), "--check"])
    assert rc == 1


def test_regenerated_content_matches_declaration():
    import read_shipped_files as rsf
    platforms = rsf.load_declaration()
    rendered = gen.render(platforms)
    assert 'set(CEYX_SHIPPED_WINDOWS_DECODER "dng_decoder_native.dll")' in rendered
    assert 'set(CEYX_SHIPPED_WINDOWS_COMPANIONS "heif.dll;libde265.dll;libomp140.x86_64.dll")' in rendered
    assert 'set(CEYX_SHIPPED_MACOS_COMPANIONS "libjpeg.8.dylib;libheif.1.dylib;libde265.0.dylib;libomp.dylib")' in rendered
    assert "lcms2" not in rendered


def test_generated_file_is_semicolon_separated_cmake_list():
    content = COMMITTED_OUTPUT.read_text()
    assert "GENERATED FILE, do not hand-edit" in content
    for line in content.splitlines():
        if "COMPANIONS" in line and "=" not in line:
            # e.g. set(CEYX_SHIPPED_LINUX_COMPANIONS "libheif.so.1;libde265.so.0")
            assert ";" in line or line.count('"') == 2
