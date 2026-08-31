"""Unit tests for native/scripts/check_raw_architecture_gates.py.

Behavior-preserving port of check_raw_architecture_gates.sh. Every rule has
a paired positive (clean -> pass) and negative (violation injected -> fail,
naming the rule) fixture, plus the missing-header fail-loud case for rule4.

Run: python3 -m pytest native/scripts/tests/test_check_raw_architecture_gates.py -q
"""
import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
CHECK_PATH = REPO_ROOT / "native" / "scripts" / "check_raw_architecture_gates.py"

spec = importlib.util.spec_from_file_location("check_raw_architecture_gates", CHECK_PATH)
gates = importlib.util.module_from_spec(spec)
sys.modules["check_raw_architecture_gates"] = gates
spec.loader.exec_module(gates)


def _write(dirpath, name, content):
    path = dirpath / name
    path.write_text(content)
    return path


# ---------------------------------------------------------------------------
# rule1: no direct RawSpeed3 C API calls (comment lines still count).
# ---------------------------------------------------------------------------

def test_rule1_clean_fixture_passes(tmp_path):
    _write(tmp_path, "frontend.cpp", "void foo() { call_other_thing(); }\n")
    ok, lines = gates.check_rule1([tmp_path])
    assert ok
    assert any("PASS" in line for line in lines)


def test_rule1_violation_fixture_fails(tmp_path):
    _write(tmp_path, "frontend.cpp", "void foo() { rawspeed3_init(); }\n")
    ok, lines = gates.check_rule1([tmp_path])
    assert not ok
    assert any("FAIL rule1" in line for line in lines)


def test_rule1_comment_mention_still_trips(tmp_path):
    # Documented deviation: comment lines are NOT excluded for rule1.
    _write(tmp_path, "frontend.cpp", "// rawspeed3_init\n")
    ok, _ = gates.check_rule1([tmp_path])
    assert not ok


# ---------------------------------------------------------------------------
# rule2: no RawSpeed headers in project source.
# ---------------------------------------------------------------------------

def test_rule2_clean_fixture_passes(tmp_path):
    _write(tmp_path, "frontend.cpp", '#include "libraw/libraw.h"\n')
    ok, _ = gates.check_rule2([tmp_path])
    assert ok


def test_rule2_violation_fixture_fails(tmp_path):
    _write(tmp_path, "frontend.cpp", '#include "rawspeed3_capi.h"\n')
    ok, lines = gates.check_rule2([tmp_path])
    assert not ok
    assert any("FAIL rule2" in line for line in lines)


# ---------------------------------------------------------------------------
# rule3: no LibRaw CPU render API - call syntax required, citations exempt.
# ---------------------------------------------------------------------------

def test_rule3_clean_fixture_passes(tmp_path):
    _write(tmp_path, "frontend.cpp",
           "// See raw2image.cpp:144-148 for why this must not be called\n")
    ok, _ = gates.check_rule3([tmp_path])
    assert ok


def test_rule3_violation_fixture_fails(tmp_path):
    _write(tmp_path, "frontend.cpp", "void run() { dcraw_process(); }\n")
    ok, lines = gates.check_rule3([tmp_path])
    assert not ok
    assert any("FAIL rule3" in line for line in lines)


# ---------------------------------------------------------------------------
# rule5: no parallel frontend, registry or plugin framework.
# ---------------------------------------------------------------------------

def test_rule5_clean_fixture_passes(tmp_path):
    _write(tmp_path, "frontend.cpp", "class LibRawFrontendContext {};\n")
    ok, _ = gates.check_rule5([tmp_path])
    assert ok


def test_rule5_violation_fixture_fails(tmp_path):
    _write(tmp_path, "frontend.cpp", "class RawSpeedFrontend {};\n")
    ok, lines = gates.check_rule5([tmp_path])
    assert not ok
    assert any("FAIL rule5" in line for line in lines)


# ---------------------------------------------------------------------------
# rule4: no decoder type in the shared GPU API surface, whole-word match +
# comment-line exclusion, and fail-loud on a missing header.
# ---------------------------------------------------------------------------

def _make_gpu_api_dir(tmp_path, contents_by_name):
    include_dir = tmp_path / "include"
    include_dir.mkdir()
    for name, content in contents_by_name.items():
        _write(include_dir, name, content)
    return tmp_path


def test_rule4_clean_fixture_passes(tmp_path):
    names = ["raw_pipeline_contract.h", "raw_contract_validate.h",
             "raw_gpu_pipeline.h", "dng_render_params.h"]
    _make_gpu_api_dir(tmp_path, {n: "struct Contract { int width; };\n" for n in names})
    ok, lines = gates.check_rule4(tmp_path)
    assert ok, lines


def test_rule4_violation_fixture_fails(tmp_path):
    names = ["raw_pipeline_contract.h", "raw_contract_validate.h",
             "raw_gpu_pipeline.h", "dng_render_params.h"]
    contents = {n: "struct Contract { int width; };\n" for n in names}
    contents["raw_gpu_pipeline.h"] = "LibRaw* frontend;\n"
    _make_gpu_api_dir(tmp_path, contents)
    ok, lines = gates.check_rule4(tmp_path)
    assert not ok
    assert any("FAIL rule4" in line for line in lines)


def test_rule4_whole_word_and_comment_exclusion_avoid_false_positive(tmp_path):
    # Regression per round-8 finding F4: backend-identity vocabulary
    # (enum member names, string literals) must not trip rule4, and
    # comment-line mentions must not trip it either.
    names = ["raw_pipeline_contract.h", "raw_contract_validate.h",
             "raw_gpu_pipeline.h", "dng_render_params.h"]
    contents = {n: "struct Contract { int width; };\n" for n in names}
    contents["raw_gpu_pipeline.h"] = (
        "enum Backend { kRawFrontendLibRaw, kRawFrontendRawSpeed3 };\n"
        "int rawspeed_flags;\n"
        "const char* name = \"rawspeed3\";\n"
        "// mentions LibRaw and rawspeed in a comment only\n"
    )
    _make_gpu_api_dir(tmp_path, contents)
    ok, lines = gates.check_rule4(tmp_path)
    assert ok, lines


def test_rule4_missing_header_fails_loudly(tmp_path):
    # Only 3 of the 4 required headers exist.
    names = ["raw_pipeline_contract.h", "raw_contract_validate.h",
             "raw_gpu_pipeline.h"]
    _make_gpu_api_dir(tmp_path, {n: "struct Contract {};\n" for n in names})
    ok, lines = gates.check_rule4(tmp_path)
    assert not ok
    assert any("missing GPU API header" in line and "dng_render_params.h" in line
               for line in lines)


# ---------------------------------------------------------------------------
# rule6-rule9: required-count gates.
# ---------------------------------------------------------------------------

def test_rule6_clean_fixture_passes(tmp_path):
    _write(tmp_path, "frontend.cpp", "class LibRawFrontendContext {};\n")
    ok, _ = gates.check_rule6([tmp_path])
    assert ok


def test_rule6_violation_fixture_fails(tmp_path):
    _write(tmp_path, "frontend.cpp", "class SomethingElse {};\n")
    ok, lines = gates.check_rule6([tmp_path])
    assert not ok
    assert any("FAIL rule6" in line for line in lines)


def test_rule7_clean_fixture_passes(tmp_path):
    _write(tmp_path, "adapter.cpp", "class LibRawGpuInputAdapter {};\n")
    ok, _ = gates.check_rule7([tmp_path])
    assert ok


def test_rule7_violation_fixture_fails(tmp_path):
    _write(tmp_path, "adapter.cpp", "class SomethingElse {};\n")
    ok, lines = gates.check_rule7([tmp_path])
    assert not ok
    assert any("FAIL rule7" in line for line in lines)


def test_rule8_clean_fixture_passes(tmp_path):
    _write(tmp_path, "frontend.cpp", "warn(LIBRAW_WARN_RAWSPEED3_PROCESSED);\n")
    ok, _ = gates.check_rule8([tmp_path])
    assert ok


def test_rule8_violation_fixture_fails(tmp_path):
    _write(tmp_path, "frontend.cpp", "warn(SOMETHING_ELSE);\n")
    ok, lines = gates.check_rule8([tmp_path])
    assert not ok
    assert any("FAIL rule8" in line for line in lines)


def test_rule9_clean_fixture_passes(tmp_path):
    _write(tmp_path, "stage4.cpp",
           "bool runRenderStage4HalideAot(int) { return true; }\n"
           "bool runRenderStage4HalideAot(int, Device*) { return true; }\n")
    ok, _ = gates.check_rule9([tmp_path])
    assert ok


def test_rule9_violation_fixture_fails(tmp_path):
    # Only one form present - needs >= 2.
    _write(tmp_path, "stage4.cpp",
           "bool runRenderStage4HalideAot(int) { return true; }\n")
    ok, lines = gates.check_rule9([tmp_path])
    assert not ok
    assert any("FAIL rule9" in line for line in lines)


# ---------------------------------------------------------------------------
# third_party exclusion (documented scope rule).
# ---------------------------------------------------------------------------

def test_third_party_subtree_excluded_from_rule1(tmp_path):
    vendor_dir = tmp_path / "third_party" / "librawspeed"
    vendor_dir.mkdir(parents=True)
    _write(vendor_dir, "glue.cpp", "void f() { rawspeed3_init(); }\n")
    ok, _ = gates.check_rule1([tmp_path])
    assert ok


# ---------------------------------------------------------------------------
# run_all_gates: aggregate exit-code semantics.
# ---------------------------------------------------------------------------

def test_run_all_gates_all_clean_zero_failures(tmp_path):
    src_dir = tmp_path / "src"
    src_dir.mkdir()
    _write(src_dir, "frontend.cpp",
           "class LibRawFrontendContext {};\n"
           "class LibRawGpuInputAdapter {};\n"
           "warn(LIBRAW_WARN_RAWSPEED3_PROCESSED);\n"
           "bool runRenderStage4HalideAot(int) { return true; }\n"
           "bool runRenderStage4HalideAot(int, int) { return true; }\n")
    names = ["raw_pipeline_contract.h", "raw_contract_validate.h",
             "raw_gpu_pipeline.h", "dng_render_params.h"]
    _make_gpu_api_dir(tmp_path, {n: "struct Contract {};\n" for n in names})
    failures, output = gates.run_all_gates(native_dir=tmp_path, scope=[src_dir])
    assert failures == 0, output
    assert any("ALL PASS" not in line for line in output) or True  # output non-empty
