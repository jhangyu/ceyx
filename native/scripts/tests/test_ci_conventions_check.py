"""Unit tests for native/scripts/ci_conventions_check.py.

Per R8, every implemented rule (C1, C2, C3, C4, C5, C6, C8) has a paired
negative-control fixture proving the rule actually detects a violation, in
addition to a positive control proving it stays silent on a compliant
fixture. Fixtures are written to tmp_path per test rather than committed
files, so each test is self-contained and the "malformed on purpose" content
never risks being mistaken for a real workflow.

Run: python3 -m pytest native/scripts/tests/test_ci_conventions_check.py -q
"""
import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
CHECK_PATH = REPO_ROOT / "native" / "scripts" / "ci_conventions_check.py"

spec = importlib.util.spec_from_file_location("ci_conventions_check", CHECK_PATH)
cc = importlib.util.module_from_spec(spec)
sys.modules["ci_conventions_check"] = cc
spec.loader.exec_module(cc)


def _write(dirpath, name, content):
    path = dirpath / name
    path.write_text(content)
    return path


# ---------------------------------------------------------------------------
# C1: workflow filename must map to a role, or carry # RATIONALE: in the
# first 5 lines.
# ---------------------------------------------------------------------------

def test_c1_compliant_fixture_passes(tmp_path):
    _write(tmp_path, "linux_build.yml", "name: Linux Build\non:\n  workflow_call:\n")
    assert cc.check_c1_roles(tmp_path) == []


def test_c1_violation_fixture_fails(tmp_path):
    _write(tmp_path, "random_thing.yml", "name: Something\non:\n  push:\n")
    violations = cc.check_c1_roles(tmp_path)
    assert any("random_thing.yml" in v for v in violations)


def test_c1_rationale_exempts_nonrole_file(tmp_path):
    _write(tmp_path, "random_thing.yml",
           "name: Something\n# RATIONALE: needed for X\non:\n  push:\n")
    assert cc.check_c1_roles(tmp_path) == []


# ---------------------------------------------------------------------------
# C2: no 'x86-64' or apple-silicon/intel tokens in name: contexts.
# ---------------------------------------------------------------------------

def test_c2_compliant_fixture_passes(tmp_path):
    _write(tmp_path, "linux_build.yml",
           "steps:\n  - uses: actions/upload-artifact@v4\n    with:\n      name: dng_decoder_native-linux-x86_64\n")
    assert cc.check_c2_naming(tmp_path) == []


def test_c2_violation_fixture_fails(tmp_path):
    _write(tmp_path, "linux_build.yml",
           "steps:\n  - uses: actions/upload-artifact@v4\n    with:\n      name: dng_decoder_native-linux-x86-64\n")
    violations = cc.check_c2_naming(tmp_path)
    assert any("x86-64" in v for v in violations)


def test_c2_annotated_exception_exempt(tmp_path):
    _write(tmp_path, "macos_build.yml",
           "env:\n  X86_AOT_TARGET: x86-64-osx-metal\n")
    assert cc.check_c2_naming(tmp_path) == []


# ---------------------------------------------------------------------------
# C3: 'gh release upload' / 'softprops/action-gh-release' in exactly one
# file, at most once.
# ---------------------------------------------------------------------------

def test_c3_compliant_fixture_passes(tmp_path):
    _write(tmp_path, "build.yml", "  - run: python3 native/scripts/publish_release.py\n")
    assert cc.check_c3_single_publisher(tmp_path) == []


def test_c3_violation_fixture_fails(tmp_path):
    _write(tmp_path, "build.yml", "  - run: gh release upload $TAG file.tar.gz\n")
    _write(tmp_path, "linux_build.yml", "  - run: gh release upload $TAG file2.tar.gz\n")
    violations = cc.check_c3_single_publisher(tmp_path)
    assert any("multiple files" in v for v in violations)


def test_c3_comment_mention_is_not_usage(tmp_path):
    _write(tmp_path, "build.yml", "  # historical note: gh release upload used to run here\n")
    assert cc.check_c3_single_publisher(tmp_path) == []


# ---------------------------------------------------------------------------
# C4: upload-artifact names are canonical (>=2 hyphens, platform token) or
# zero-hyphen diagnostic names.
# ---------------------------------------------------------------------------

def test_c4_compliant_fixture_passes(tmp_path):
    _write(tmp_path, "linux_build.yml",
           "steps:\n  - uses: actions/upload-artifact@v4\n    with:\n      name: dng_decoder_native-linux-x86_64\n")
    assert cc.check_c4_artifact_names(tmp_path) == []


def test_c4_zero_hyphen_diagnostic_name_allowed(tmp_path):
    _write(tmp_path, "linux_build.yml",
           "steps:\n  - uses: actions/upload-artifact@v4\n    with:\n      name: proberesults\n")
    assert cc.check_c4_artifact_names(tmp_path) == []


def test_c4_violation_fixture_fails(tmp_path):
    _write(tmp_path, "linux_build.yml",
           "steps:\n  - uses: actions/upload-artifact@v4\n    with:\n      name: probe-results\n")
    violations = cc.check_c4_artifact_names(tmp_path)
    assert any("probe-results" in v for v in violations)


def test_c4_matrix_arch_tag_resolved(tmp_path):
    _write(tmp_path, "macos_build.yml",
           "matrix:\n  include:\n    - arch_tag: arm64\n    - arch_tag: x86_64\n"
           "steps:\n  - uses: actions/upload-artifact@v4\n    with:\n"
           "      name: dng_decoder_native-macos-${{ matrix.arch_tag }}\n")
    assert cc.check_c4_artifact_names(tmp_path) == []


# ---------------------------------------------------------------------------
# C5: every workflow has a real trigger (not ci/**-push-only).
# ---------------------------------------------------------------------------

def test_c5_compliant_fixture_passes(tmp_path):
    _write(tmp_path, "linux_build.yml", "on:\n  workflow_call:\n")
    assert cc.check_c5_real_trigger(tmp_path) == []


def test_c5_violation_fixture_fails(tmp_path):
    _write(tmp_path, "scratch.yml", "on:\n  push:\n    paths:\n      - 'ci/**'\n")
    violations = cc.check_c5_real_trigger(tmp_path)
    assert any("scratch.yml" in v for v in violations)


def test_c5_push_paired_with_ci_paths_is_fine(tmp_path):
    _write(tmp_path, "build.yml", "on:\n  push:\n    branches: [main]\n  pull_request:\n")
    assert cc.check_c5_real_trigger(tmp_path) == []


# ---------------------------------------------------------------------------
# C6: build.yml's required= set equals the union of canonical asset names.
# ---------------------------------------------------------------------------

def test_c6_compliant_fixture_passes(tmp_path):
    _write(tmp_path, "linux_build.yml",
           "steps:\n  - uses: actions/upload-artifact@v4\n    with:\n      name: dng_decoder_native-linux-x86_64\n")
    _write(tmp_path, "build.yml",
           '  required="\n  dng_decoder_native-linux-x86_64\n  "\n')
    assert cc.check_c6_required_asset_set(tmp_path) == []


def test_c6_violation_fixture_fails(tmp_path):
    _write(tmp_path, "linux_build.yml",
           "steps:\n  - uses: actions/upload-artifact@v4\n    with:\n      name: dng_decoder_native-linux-x86_64\n")
    _write(tmp_path, "build.yml",
           '  required="\n  some-other-asset-name\n  "\n')
    violations = cc.check_c6_required_asset_set(tmp_path)
    assert len(violations) == 2  # one direction each way


# ---------------------------------------------------------------------------
# C8: render_expectations.py --check passes.
# ---------------------------------------------------------------------------

def test_c8_compliant_fixture_passes(tmp_path, monkeypatch):
    sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))
    import render_expectations as re_mod
    monkeypatch.setattr(re_mod, "load_ledger", lambda path=None: {
        "fake-leg": {"instrument": "capability-probe", "workflow": "fake.yml",
                     "expect": {"jpeg:encode": 1}}
    })
    _write(tmp_path, "fake.yml", "  - run: probe.py --expect jpeg:encode=1\n")
    assert cc.check_c8_ledger_sync(tmp_path) == []


def test_c8_violation_fixture_fails(tmp_path, monkeypatch):
    sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))
    import render_expectations as re_mod
    monkeypatch.setattr(re_mod, "load_ledger", lambda path=None: {
        "fake-leg": {"instrument": "capability-probe", "workflow": "fake.yml",
                     "expect": {"jpeg:encode": 1}}
    })
    _write(tmp_path, "fake.yml", "  - run: probe.py --expect jpeg:decode=0\n")
    violations = cc.check_c8_ledger_sync(tmp_path)
    assert violations  # jpeg:encode=1 claimed by ledger, not asserted anywhere


def test_c8_catches_cross_leg_drift_union_missed(tmp_path, monkeypatch):
    """C8 must inherit the per-leg fix: a wrong-vector leg hiding behind
    another leg's legitimate token must fail C8 too, not just render_expectations
    directly (round-1 should-fix #3 reproduced at the ci_conventions_check layer)."""
    sys.path.insert(0, str(REPO_ROOT / "native" / "scripts"))
    import render_expectations as re_mod
    monkeypatch.setattr(re_mod, "load_ledger", lambda path=None: {
        "leg-macos": {"instrument": "configure-log", "workflow": "macos.yml",
                      "expect": {"avif:encode": 0}},
        "leg-linux": {"instrument": "capability-probe", "workflow": "linux.yml",
                      "expect": {"avif:encode": 1}},
        "leg-windows": {"instrument": "capability-probe", "workflow": "windows.yml",
                        "expect": {"avif:encode": 1}},
    })
    _write(tmp_path, "macos.yml", "  - run: probe.py --expect avif:encode=0\n")
    _write(tmp_path, "linux.yml", "  - run: probe.py --expect avif:encode=1\n")
    _write(tmp_path, "windows.yml", "  - run: probe.py --expect avif:encode=0\n")
    violations = cc.check_c8_ledger_sync(tmp_path)
    assert any("leg-windows" in v for v in violations), violations


# ---------------------------------------------------------------------------
# C9 (ruling 5, 2026-09-01): no workflow hardcodes the vcpkg builtin-baseline
# SHA; every workflow must derive it at runtime from native/vcpkg/vcpkg.json.
# ---------------------------------------------------------------------------

def test_c9_compliant_fixture_passes(tmp_path):
    _write(
        tmp_path,
        "macos_build.yml",
        "    env:\n"
        "      VCPKG_TRIPLET: arm64-osx-heif\n"
        "    steps:\n"
        "      - name: Derive vcpkg baseline from vcpkg.json\n"
        "        run: |\n"
        "          BASELINE=\"$(python3 -c \\\"import json; "
        "print(json.load(open('native/vcpkg/vcpkg.json'))['builtin-baseline'])\\\")\"\n"
        "          echo \"VCPKG_BASELINE=$BASELINE\" >> \"$GITHUB_ENV\"\n",
    )
    assert cc.check_c9_no_hardcoded_vcpkg_baseline(tmp_path) == []


def test_c9_violation_fixture_fails(tmp_path):
    """Injected drift: a workflow hardcoding the baseline literal, exactly
    the shape ruling 5 forbids -- must be caught red before it is fixed."""
    _write(
        tmp_path,
        "linux_build.yml",
        "    env:\n"
        "      VCPKG_BASELINE: abb6dda5cc32914d2e64d7d72b974dc301d1fc8a\n",
    )
    violations = cc.check_c9_no_hardcoded_vcpkg_baseline(tmp_path)
    assert any("linux_build.yml" in v and "abb6dda5cc32914d2e64d7d72b974dc301d1fc8a" in v
               for v in violations), violations


def test_c9_flags_hardcoded_literal_even_when_it_matches_vcpkg_json(tmp_path):
    """A hardcoded copy is a violation even if it currently agrees with
    vcpkg.json -- single authority, not eventual consistency."""
    _write(
        tmp_path,
        "heif_dist_windows.yml",
        "    env:\n"
        "      # same PINNED baseline as macos_build.yml\n"
        "      VCPKG_BASELINE: abb6dda5cc32914d2e64d7d72b974dc301d1fc8a\n",
    )
    violations = cc.check_c9_no_hardcoded_vcpkg_baseline(tmp_path)
    assert violations


def test_c9_ignores_unrelated_40_hex_tokens(tmp_path):
    """A 40-hex SHA with no baseline/vcpkg context (e.g. a ZLIB_SHA literal)
    must not be flagged -- C9 is scoped to the vcpkg baseline specifically."""
    _write(
        tmp_path,
        "windows_build.yml",
        "          ZLIB_SHA=\"9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23\"\n",
    )
    assert cc.check_c9_no_hardcoded_vcpkg_baseline(tmp_path) == []
