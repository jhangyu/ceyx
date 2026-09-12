"""Unit tests for native/scripts/codec_capability_probe.py.

Run: python3 -m pytest native/scripts/tests/test_codec_capability_probe.py -q
"""
import ctypes
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
PROBE_PATH = REPO_ROOT / "native" / "scripts" / "codec_capability_probe.py"

spec = importlib.util.spec_from_file_location("codec_capability_probe", PROBE_PATH)
ccp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ccp)


def run_cli(args):
    return subprocess.run(
        [sys.executable, str(PROBE_PATH)] + args,
        capture_output=True, text=True,
    )


def test_help_exits_zero():
    result = run_cli(["--help"])
    assert result.returncode == 0


def test_formats_and_directions_vocabulary():
    assert ccp.FORMATS == {"jpeg": 1, "webp": 2, "heic": 3, "avif": 4, "jxl": 5}
    assert ccp.DIRECTIONS == ("encode", "decode")


def test_parse_expectation_valid():
    assert ccp.parse_expectation("heic:encode=1") == ("heic", "encode", 1)
    assert ccp.parse_expectation("jpeg:decode=0") == ("jpeg", "decode", 0)


def test_parse_expectation_malformed():
    try:
        ccp.parse_expectation("garbage")
        assert False, "expected ValueError"
    except ValueError as exc:
        assert "malformed expectation" in str(exc)


def test_parse_expectation_unknown_format():
    try:
        ccp.parse_expectation("bogus:encode=1")
        assert False, "expected ValueError"
    except ValueError as exc:
        assert "unknown format" in str(exc)


def test_parse_expectation_unknown_direction():
    try:
        ccp.parse_expectation("jpeg:sideways=1")
        assert False, "expected ValueError"
    except ValueError as exc:
        assert "unknown direction" in str(exc)


def test_parse_expectation_bad_value():
    try:
        ccp.parse_expectation("jpeg:encode=2")
        assert False, "expected ValueError"
    except ValueError as exc:
        assert "must be 0 or 1" in str(exc)


def test_cli_bad_spec_exits_2():
    result = run_cli(["/nonexistent/lib", "--expect", "bogus:encode=1"])
    assert result.returncode == 2
    assert "unknown format" in result.stderr


def test_cli_missing_lib_exits_1():
    result = run_cli(["/nonexistent/lib.so", "--expect", "jxl:encode=1"])
    assert result.returncode == 1
    assert "dlopen failed" in result.stderr


def _find_local_lib():
    """Locate a locally built dng_decoder_native shared library, if any."""
    for pattern in ("libdng_decoder_native.dylib", "libdng_decoder_native.so",
                    "dng_decoder_native.dll"):
        matches = sorted(REPO_ROOT.glob(f"native/build*/{pattern}"))
        if matches:
            return matches[0]
    return None


def test_load_library_missing_raises_probe_error():
    try:
        ccp.load_library("/nonexistent/lib.so")
        assert False, "expected ProbeError"
    except ccp.ProbeError as exc:
        assert "dlopen failed" in str(exc)


def test_wrong_expectation_exits_1_on_real_library(tmp_path):
    lib = _find_local_lib()
    if lib is None:
        import pytest
        pytest.skip("no locally built dng_decoder_native library found")
    loaded = ctypes.CDLL(str(lib))
    loaded.ceyx_encode_supports.argtypes = [ctypes.c_int32]
    loaded.ceyx_encode_supports.restype = ctypes.c_int32
    actual = loaded.ceyx_encode_supports(ccp.FORMATS["jxl"])
    wrong = 0 if actual == 1 else 1
    result = run_cli([str(lib), "--expect", f"jxl:encode={wrong}"])
    assert result.returncode == 1
    assert "capability mismatch" in result.stderr


def test_json_out_written_on_dlopen_failure(tmp_path):
    out_path = tmp_path / "result.json"
    result = run_cli(["/nonexistent/lib.so", "--expect", "jxl:encode=1",
                       "--json-out", str(out_path)])
    assert result.returncode == 1
    assert out_path.exists()
    data = json.loads(out_path.read_text())
    assert data["ok"] is False
    assert "error" in data


def test_json_out_written_on_capability_mismatch(tmp_path):
    lib = _find_local_lib()
    if lib is None:
        import pytest
        pytest.skip("no locally built dng_decoder_native library found")
    loaded = ctypes.CDLL(str(lib))
    loaded.ceyx_encode_supports.argtypes = [ctypes.c_int32]
    loaded.ceyx_encode_supports.restype = ctypes.c_int32
    actual = loaded.ceyx_encode_supports(ccp.FORMATS["jxl"])
    wrong = 0 if actual == 1 else 1
    out_path = tmp_path / "result.json"
    result = run_cli([str(lib), "--expect", f"jxl:encode={wrong}",
                       "--json-out", str(out_path)])
    assert result.returncode == 1
    assert out_path.exists()
    data = json.loads(out_path.read_text())
    assert data["ok"] is False
    assert data["results"][0]["expected"] == wrong


# --- WI-5: --expect-cap build-capability surface ---------------------------

def test_capability_names_vocabulary():
    assert ccp.CAPABILITY_NAMES == ("ICC", "OPENMP", "HEIF", "WEBP", "JXL", "RAW")


def test_parse_capability_expectation_valid():
    assert ccp.parse_capability_expectation("ICC=0") == ("ICC", 0)
    assert ccp.parse_capability_expectation("OPENMP=1") == ("OPENMP", 1)


def test_parse_capability_expectation_malformed():
    try:
        ccp.parse_capability_expectation("garbage")
        assert False, "expected ValueError"
    except ValueError as exc:
        assert "malformed capability expectation" in str(exc)


def test_parse_capability_expectation_unknown_name():
    try:
        ccp.parse_capability_expectation("BOGUS=1")
        assert False, "expected ValueError"
    except ValueError as exc:
        assert "unknown capability" in str(exc)


def test_parse_capability_expectation_bad_value():
    try:
        ccp.parse_capability_expectation("ICC=2")
        assert False, "expected ValueError"
    except ValueError as exc:
        assert "must be 0 or 1" in str(exc)


def test_cli_requires_expect_or_expect_cap():
    result = run_cli(["/nonexistent/lib"])
    assert result.returncode == 2
    assert "--expect" in result.stderr or "--expect" in result.stdout


def test_cli_bad_capability_spec_exits_2():
    result = run_cli(["/nonexistent/lib", "--expect-cap", "BOGUS=1"])
    assert result.returncode == 2
    assert "unknown capability" in result.stderr


def test_cli_missing_lib_exits_1_for_capability_only():
    result = run_cli(["/nonexistent/lib.so", "--expect-cap", "ICC=0"])
    assert result.returncode == 1
    assert "dlopen failed" in result.stderr


def test_capability_mismatch_and_unverified_on_real_library(tmp_path):
    lib = _find_local_lib()
    if lib is None:
        import pytest
        pytest.skip("no locally built dng_decoder_native library found")
    loaded = ctypes.CDLL(str(lib))
    try:
        cap_fn = loaded.ceyx_build_capabilities
    except AttributeError:
        import pytest
        pytest.skip("local library predates WI-5's ceyx_build_capabilities export")
    cap_fn.argtypes = [ctypes.c_char_p]
    cap_fn.restype = ctypes.c_int32

    # Missing/unexpectedly-present: flip a real answer and expect MISMATCH.
    actual = cap_fn(b"ICC")
    wrong = 0 if actual == 1 else 1
    out_path = tmp_path / "result.json"
    result = run_cli([str(lib), "--expect-cap", f"ICC={wrong}", "--json-out", str(out_path)])
    assert result.returncode == 1
    assert "capability mismatch" in result.stderr
    data = json.loads(out_path.read_text())
    assert data["capability_results"][0]["ok"] is False

    # Unknown-capability-name: the library itself reports -1 for a name it
    # does not recognise (WI-5 plan step 5.3: an unrecognised-by-the-library
    # answer is UNVERIFIED, not "confirmed absent").
    unknown = cap_fn(b"NOTACAPABILITY")
    assert unknown == -1


def test_capability_ok_on_real_library():
    lib = _find_local_lib()
    if lib is None:
        import pytest
        pytest.skip("no locally built dng_decoder_native library found")
    loaded = ctypes.CDLL(str(lib))
    try:
        cap_fn = loaded.ceyx_build_capabilities
    except AttributeError:
        import pytest
        pytest.skip("local library predates WI-5's ceyx_build_capabilities export")
    cap_fn.argtypes = [ctypes.c_char_p]
    cap_fn.restype = ctypes.c_int32
    actual = cap_fn(b"ICC")
    result = run_cli([str(lib), "--expect-cap", f"ICC={actual}"])
    assert result.returncode == 0
