"""Unit tests for native/scripts/ci/check_no_test_execution_in_ci.py — WI-5.

Loaded via importlib.util.spec_from_file_location (not `import ci.report`
etc.) so this test binds one, unambiguous module identity regardless of
whether `native/scripts` or the repo root is on sys.path — the dual-identity
trap the push-1 gate caught (`ci.*` vs `native.scripts.ci.*`).

Pins go FIRST (existing YAML behaviour must not move), then the UR-1
detection/exemption cases, then the WI-5(b) Python-side AST scan cases.
Run: python3 -m pytest native/scripts/tests/test_check_no_test_execution_in_ci.py -q
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
CHECK_PATH = REPO_ROOT / "native" / "scripts" / "ci" / "check_no_test_execution_in_ci.py"

spec = importlib.util.spec_from_file_location("check_no_test_execution_in_ci", CHECK_PATH)
guard = importlib.util.module_from_spec(spec)
sys.modules["check_no_test_execution_in_ci"] = guard
spec.loader.exec_module(guard)


# --- Pins: existing YAML-scan behaviour must survive this WI unchanged ---


def test_yaml_scan_still_flags_flutter_test():
    failures = []
    for label, pattern in guard.SUBSTRING_PATTERNS:
        if label == "flutter test" and pattern.search("run: flutter test integration/"):
            failures.append(label)
    assert failures == ["flutter test"]


def test_yaml_scan_still_allows_probe_codecs_line():
    # verbatim shape from linux_build.yml:872
    line = '"$PROBE_DIR/probe_codecs" > "$PROBE_DIR/probe_codecs.txt" 2>&1'
    first_token = line.strip().split()[0]
    candidate = guard.FIRST_TOKEN_STRIP_RE.sub("", first_token).rstrip("\"'")
    assert guard.TEST_OR_PROBE_BINARY_RE.match(candidate)
    basename = candidate[:-4] if candidate.endswith(".exe") else candidate
    assert basename in guard.ALLOWED_CAPABILITY_PROBES


def test_guard_exits_0_at_repo_tip():
    rc = guard.main()
    assert rc == 0


# --- UR-1(a) DETECTION: this is the WI's success condition ---


def test_pytest_outside_deps_fails_loudly():
    line = "python3 -m pytest native/tests/decode/ -q"
    assert guard.classify_pytest_line(line) == "violation"


def test_unittest_module_invocation_is_detected():
    line = "python3 -m unittest discover native/tests"
    assert guard.classify_pytest_line(line) == "violation"


def test_exemption_is_path_scoped_not_command_scoped():
    # no path argument at all -> must FAIL, not pass vacuously
    assert guard.classify_pytest_line("python -m pytest -q") == "violation"
    # prefix string match must not be a substring match
    assert guard.classify_pytest_line("pytest native/scripts/depsfoo/ -q") == "violation"


# --- UR-1(b) EXEMPTION: named, printed, path-scoped ---


def test_deps_pytest_passes_and_is_printed():
    for _wf, _step, literal in guard.ALLOWED_TEST_SUITE_CALLS:
        if "native/scripts/deps" in literal or "run_dist_equivalence" in literal:
            assert guard.classify_pytest_line(literal) == "allowed"


def test_run_dist_equivalence_is_detected_then_exempted():
    line = "python3 native/tests/run_dist_equivalence.py \\"
    assert guard.TEST_SUITE_INVOCATION_RE.search(line)
    assert guard.classify_pytest_line(line) == "allowed"


def test_pip_install_naming_pytest_is_not_an_invocation():
    # windows_build.yml:89 / linux_build.yml:201 shape — installing the
    # package is not running the suite.
    line = "python -m pip install --disable-pip-version-check pytest"
    assert guard.classify_pytest_line(line) == "not-a-pytest-line"


def test_printed_exemption_block_states_the_ruling_not_a_pending_review(capsys):
    guard.main()
    out = capsys.readouterr().out
    assert "USER RULING 2026-09-13" in out
    assert "surfaced for user review" not in out
    for workflow, step_name, literal in guard.ALLOWED_TEST_SUITE_CALLS:
        assert workflow in out
        assert literal in out


# --- WI-5(b): Python-side AST scan ---


def test_planted_subprocess_outside_run_fails(tmp_path):
    ci_root = tmp_path / "native" / "scripts" / "ci"
    ci_root.mkdir(parents=True)
    (ci_root / "sneaky.py").write_text(
        "import subprocess\n"
        "def f():\n"
        "    return subprocess.run(['echo', 'hi'])\n"
    )
    # scan_python_sources resolves roots against REPO_ROOT, so redirect it
    # at this fixture directory rather than the real repo.
    orig_root = guard.REPO_ROOT
    guard.REPO_ROOT = tmp_path
    try:
        failures, allowed, unresolved = guard.scan_python_sources(("native/scripts/ci",))
    finally:
        guard.REPO_ROOT = orig_root
    assert any(f[2] == "subprocess-outside-run" for f in failures)


def test_planted_probe_execution_in_python_fails(tmp_path):
    ci_root = tmp_path / "native" / "scripts" / "ci"
    ci_root.mkdir(parents=True)
    (ci_root / "run.py").write_text("def run(argv):\n    pass\n")
    (ci_root / "caller.py").write_text(
        "from . import run\n"
        "def f(d):\n"
        "    return run.run([f'{d}/probe_wrong', '-x'])\n"
    )
    orig_root = guard.REPO_ROOT
    guard.REPO_ROOT = tmp_path
    try:
        failures, allowed, unresolved = guard.scan_python_sources(("native/scripts/ci",))
    finally:
        guard.REPO_ROOT = orig_root
    assert any(f[2] == "direct test/probe binary execution" for f in failures)


def test_probe_codecs_in_python_is_allowed_and_printed(tmp_path):
    ci_root = tmp_path / "native" / "scripts" / "ci"
    ci_root.mkdir(parents=True)
    (ci_root / "run.py").write_text("def run(argv):\n    pass\n")
    (ci_root / "caller.py").write_text(
        "from . import run\n"
        "def f():\n"
        "    return run.run(['probe_codecs', '-q'])\n"
    )
    orig_root = guard.REPO_ROOT
    guard.REPO_ROOT = tmp_path
    try:
        failures, allowed, unresolved = guard.scan_python_sources(("native/scripts/ci",))
    finally:
        guard.REPO_ROOT = orig_root
    assert not failures
    assert any(a[2] == "probe_codecs" for a in allowed)


def test_unresolvable_argv0_is_reported_not_silent(tmp_path):
    ci_root = tmp_path / "native" / "scripts" / "ci"
    ci_root.mkdir(parents=True)
    (ci_root / "run.py").write_text("def run(argv):\n    pass\n")
    (ci_root / "caller.py").write_text(
        "from . import run\n"
        "def f(dynamic_argv):\n"
        "    return run.run(dynamic_argv)\n"
    )
    orig_root = guard.REPO_ROOT
    guard.REPO_ROOT = tmp_path
    try:
        failures, allowed, unresolved = guard.scan_python_sources(("native/scripts/ci",))
    finally:
        guard.REPO_ROOT = orig_root
    assert not failures
    assert len(unresolved) == 1


def test_grandfathered_check_cmake_sources_tracked_is_allowed_not_failed():
    failures, allowed, unresolved = guard.scan_python_sources(guard.PYTHON_SCAN_ROOTS)
    assert not any(
        f[0] == "native/scripts/ci/check_cmake_sources_tracked.py" for f in failures
    )
    assert any(
        a[0] == "native/scripts/ci/check_cmake_sources_tracked.py" for a in allowed
    )


def test_guard_survives_missing_ci_package(tmp_path):
    # "must still exit 0 on a checkout where native/scripts/ci/ is empty or
    # absent (it runs before the package exists on old branches)."
    failures, allowed, unresolved = guard.scan_python_sources(("native/scripts/does-not-exist",))
    assert failures == []
    assert allowed == []
    assert unresolved == []


if __name__ == "__main__":
    import pytest

    raise SystemExit(pytest.main([__file__, "-v"]))
