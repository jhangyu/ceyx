"""Unit tests for native/scripts/render_expectations.py.

Run: python3 -m pytest native/scripts/tests/test_render_expectations.py -q
"""
import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "native" / "scripts" / "render_expectations.py"

spec = importlib.util.spec_from_file_location("render_expectations", MODULE_PATH)
re_mod = importlib.util.module_from_spec(spec)
sys.modules["render_expectations"] = re_mod
spec.loader.exec_module(re_mod)


def _write_ledger(tmp_path, content):
    path = tmp_path / "codec_expectations.toml"
    path.write_text(content)
    return path


def test_load_real_ledger_succeeds():
    legs = re_mod.load_ledger()
    assert "windows-x86_64" in legs
    assert legs["windows-x86_64"]["instrument"] == "capability-probe"


def test_bare_zero_rejected(tmp_path):
    path = _write_ledger(tmp_path, '''
[leg-a]
instrument = "capability-probe"

[leg-a.expect]
"jpeg:decode" = 0
''')
    try:
        re_mod.load_ledger(path)
        assert False, "expected LedgerError"
    except re_mod.LedgerError as exc:
        assert "bare 0 is rejected" in str(exc)


def test_zero_with_reason_and_owner_accepted(tmp_path):
    path = _write_ledger(tmp_path, '''
[leg-a]
instrument = "capability-probe"

[leg-a.expect]
"jpeg:decode" = { value = 0, reason = "deliberate", owner = "CI-T1" }
''')
    legs = re_mod.load_ledger(path)
    assert legs["leg-a"]["expect"]["jpeg:decode"] == 0


def test_zero_missing_reason_rejected(tmp_path):
    path = _write_ledger(tmp_path, '''
[leg-a]
instrument = "capability-probe"

[leg-a.expect]
"jpeg:decode" = { value = 0, owner = "CI-T1" }
''')
    try:
        re_mod.load_ledger(path)
        assert False, "expected LedgerError"
    except re_mod.LedgerError as exc:
        assert "reason" in str(exc)


def test_zero_missing_owner_rejected(tmp_path):
    path = _write_ledger(tmp_path, '''
[leg-a]
instrument = "capability-probe"

[leg-a.expect]
"jpeg:decode" = { value = 0, reason = "deliberate" }
''')
    try:
        re_mod.load_ledger(path)
        assert False, "expected LedgerError"
    except re_mod.LedgerError as exc:
        assert "owner" in str(exc)


def test_render_produces_sorted_expect_flags(tmp_path):
    path = _write_ledger(tmp_path, '''
[leg-a]
instrument = "capability-probe"

[leg-a.expect]
"webp:encode" = 1
"jpeg:encode" = 1
''')
    legs = re_mod.load_ledger(path)
    fragment = re_mod.render("leg-a", ledger=legs)
    assert fragment == ["--expect", "jpeg:encode=1", "--expect", "webp:encode=1"]


def test_render_unknown_leg_raises(tmp_path):
    path = _write_ledger(tmp_path, '''
[leg-a]
instrument = "capability-probe"

[leg-a.expect]
"jpeg:encode" = 1
''')
    legs = re_mod.load_ledger(path)
    try:
        re_mod.render("nonexistent-leg", ledger=legs)
        assert False, "expected LedgerError"
    except re_mod.LedgerError as exc:
        assert "unknown leg" in str(exc)


def test_check_agrees_when_workflow_matches_ledger(tmp_path):
    ledger = {"leg-a": {"instrument": "capability-probe", "expect": {"jpeg:encode": 1}}}
    workflows_dir = tmp_path / "workflows"
    workflows_dir.mkdir()
    (workflows_dir / "fake.yml").write_text("  - run: probe.py --expect jpeg:encode=1\n")
    assert re_mod.check(ledger=ledger, workflows_dir=workflows_dir) == []


def test_check_flags_ledger_claim_workflow_does_not_assert(tmp_path):
    ledger = {"leg-a": {"instrument": "capability-probe", "expect": {"jpeg:encode": 1}}}
    workflows_dir = tmp_path / "workflows"
    workflows_dir.mkdir()
    (workflows_dir / "fake.yml").write_text("  - run: echo nothing\n")
    disagreements = re_mod.check(ledger=ledger, workflows_dir=workflows_dir)
    assert any("jpeg:encode=1" in d for d in disagreements)


def test_check_flags_workflow_claim_ledger_does_not_have(tmp_path):
    ledger = {"leg-a": {"instrument": "capability-probe", "expect": {"jpeg:encode": 1}}}
    workflows_dir = tmp_path / "workflows"
    workflows_dir.mkdir()
    (workflows_dir / "fake.yml").write_text(
        "  - run: probe.py --expect jpeg:encode=1 --expect heic:decode=0\n"
    )
    disagreements = re_mod.check(ledger=ledger, workflows_dir=workflows_dir)
    assert any("heic:decode=0" in d for d in disagreements)


def test_cli_leg_help_exits_zero():
    import subprocess
    result = subprocess.run(
        [sys.executable, str(MODULE_PATH), "--help"],
        capture_output=True, text=True,
    )
    assert result.returncode == 0
