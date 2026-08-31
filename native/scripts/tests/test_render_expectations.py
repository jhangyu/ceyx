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
workflow = "fake.yml"

[leg-a.expect]
"jpeg:decode" = 0
''')
    try:
        re_mod.load_ledger(path)
        assert False, "expected LedgerError"
    except re_mod.LedgerError as exc:
        assert "bare 0 is rejected" in str(exc)


def test_missing_workflow_key_rejected(tmp_path):
    path = _write_ledger(tmp_path, '''
[leg-a]
instrument = "capability-probe"

[leg-a.expect]
"jpeg:encode" = 1
''')
    try:
        re_mod.load_ledger(path)
        assert False, "expected LedgerError"
    except re_mod.LedgerError as exc:
        assert "workflow" in str(exc)


def test_zero_with_reason_and_owner_accepted(tmp_path):
    path = _write_ledger(tmp_path, '''
[leg-a]
instrument = "capability-probe"
workflow = "fake.yml"

[leg-a.expect]
"jpeg:decode" = { value = 0, reason = "deliberate", owner = "CI-T1" }
''')
    legs = re_mod.load_ledger(path)
    assert legs["leg-a"]["expect"]["jpeg:decode"] == 0


def test_zero_missing_reason_rejected(tmp_path):
    path = _write_ledger(tmp_path, '''
[leg-a]
instrument = "capability-probe"
workflow = "fake.yml"

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
workflow = "fake.yml"

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
workflow = "fake.yml"

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
workflow = "fake.yml"

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
    ledger = {"leg-a": {"instrument": "capability-probe", "workflow": "fake.yml",
                         "expect": {"jpeg:encode": 1}}}
    workflows_dir = tmp_path / "workflows"
    workflows_dir.mkdir()
    (workflows_dir / "fake.yml").write_text("  - run: probe.py --expect jpeg:encode=1\n")
    assert re_mod.check(ledger=ledger, workflows_dir=workflows_dir) == []


def test_check_flags_ledger_claim_workflow_does_not_assert(tmp_path):
    ledger = {"leg-a": {"instrument": "capability-probe", "workflow": "fake.yml",
                         "expect": {"jpeg:encode": 1}}}
    workflows_dir = tmp_path / "workflows"
    workflows_dir.mkdir()
    (workflows_dir / "fake.yml").write_text("  - run: echo nothing\n")
    disagreements = re_mod.check(ledger=ledger, workflows_dir=workflows_dir)
    assert any("jpeg:encode=1" in d for d in disagreements)


def test_check_flags_workflow_claim_ledger_does_not_have(tmp_path):
    ledger = {"leg-a": {"instrument": "capability-probe", "workflow": "fake.yml",
                         "expect": {"jpeg:encode": 1}}}
    workflows_dir = tmp_path / "workflows"
    workflows_dir.mkdir()
    (workflows_dir / "fake.yml").write_text(
        "  - run: probe.py --expect jpeg:encode=1 --expect heic:decode=0\n"
    )
    disagreements = re_mod.check(ledger=ledger, workflows_dir=workflows_dir)
    assert any("heic:decode=0" in d for d in disagreements)


def test_check_flags_missing_workflow_file(tmp_path):
    ledger = {"leg-a": {"instrument": "capability-probe", "workflow": "nonexistent.yml",
                         "expect": {"jpeg:encode": 1}}}
    workflows_dir = tmp_path / "workflows"
    workflows_dir.mkdir()
    disagreements = re_mod.check(ledger=ledger, workflows_dir=workflows_dir)
    assert any("nonexistent.yml" in d and "does not exist" in d for d in disagreements)


def test_check_isolates_each_leg_to_its_own_workflow_file(tmp_path):
    """Positive-control companion to the drift regression below: two legs,
    each correctly wired to its OWN file, must agree even though the two
    files individually assert different values for the same token."""
    ledger = {
        "leg-a": {"instrument": "capability-probe", "workflow": "a.yml",
                  "expect": {"avif:encode": 0}},
        "leg-b": {"instrument": "capability-probe", "workflow": "b.yml",
                  "expect": {"avif:encode": 1}},
    }
    workflows_dir = tmp_path / "workflows"
    workflows_dir.mkdir()
    (workflows_dir / "a.yml").write_text("  - run: probe.py --expect avif:encode=0\n")
    (workflows_dir / "b.yml").write_text("  - run: probe.py --expect avif:encode=1\n")
    assert re_mod.check(ledger=ledger, workflows_dir=workflows_dir) == []


def test_check_catches_cross_leg_drift_union_missed(tmp_path):
    """Reproduction of round-1 should-fix #3: a leg wired with the WRONG
    vector (windows asserting avif:encode=0 when its ledger says 1) used to
    pass a union-of-all-legs check because a DIFFERENT leg (macos-x86_64)
    legitimately asserts avif:encode=0 -- and the missing correct 1 hid
    behind a THIRD leg (linux-x86_64) which legitimately asserts
    avif:encode=1. Per-leg check() must catch this even though every token
    value appears somewhere in the union of ledger entries AND somewhere in
    the union of workflow files.
    """
    ledger = {
        # macos-x86_64 stand-in: legitimately expects (and asserts) 0.
        "leg-macos": {"instrument": "configure-log", "workflow": "macos.yml",
                      "expect": {"avif:encode": 0}},
        # linux-x86_64 stand-in: legitimately expects (and asserts) 1.
        "leg-linux": {"instrument": "capability-probe", "workflow": "linux.yml",
                      "expect": {"avif:encode": 1}},
        # windows-x86_64 stand-in: ledger says 1 (the real end-state target),
        # but its OWN workflow file is wired wrong and asserts 0.
        "leg-windows": {"instrument": "capability-probe", "workflow": "windows.yml",
                        "expect": {"avif:encode": 1}},
    }
    workflows_dir = tmp_path / "workflows"
    workflows_dir.mkdir()
    (workflows_dir / "macos.yml").write_text("  - run: probe.py --expect avif:encode=0\n")
    (workflows_dir / "linux.yml").write_text("  - run: probe.py --expect avif:encode=1\n")
    (workflows_dir / "windows.yml").write_text("  - run: probe.py --expect avif:encode=0\n")

    # Sanity: the union of ledger tokens ({0, 1}) equals the union of
    # workflow tokens ({0, 1}) -- a leg-agnostic union check would see zero
    # disagreements here, which is exactly the bug.
    ledger_union = {f"{p}={v}" for t in ledger.values() for p, v in t["expect"].items()}
    workflow_union = set()
    for wf in workflows_dir.glob("*.yml"):
        workflow_union |= re_mod._tokens_in_file(wf)
    assert ledger_union == workflow_union == {"avif:encode=0", "avif:encode=1"}

    disagreements = re_mod.check(ledger=ledger, workflows_dir=workflows_dir)
    assert any("leg-windows" in d and "avif:encode=0" in d for d in disagreements), disagreements
    assert any("leg-windows" in d and "avif:encode=1" in d for d in disagreements), disagreements


def test_check_step_anchor_discriminates_two_legs_in_one_file(tmp_path):
    """SF2 regression (round-3 review): two legs sharing ONE workflow file
    (macos_build.yml's arm64/x86_64 matrix legs), each asserting a different
    value for the same token in a different step. A whole-file scan would
    see both legs' tokens as one set and falsely flag both as wrong; the
    `step_anchor` discriminator must isolate each leg to its own step."""
    ledger = {
        "leg-native": {"instrument": "capability-probe", "workflow": "shared.yml",
                       "step_anchor": "native leg", "expect": {"avif:encode": 1}},
        "leg-cross": {"instrument": "configure-log", "workflow": "shared.yml",
                      "step_anchor": "cross leg", "expect": {"avif:encode": 0}},
    }
    workflows_dir = tmp_path / "workflows"
    workflows_dir.mkdir()
    (workflows_dir / "shared.yml").write_text(
        "jobs:\n"
        "  build:\n"
        "    steps:\n"
        "      - name: Assert full vector (native leg)\n"
        "        if: matrix.cross == 'false'\n"
        "        run: probe.py --expect avif:encode=1\n"
        "      - name: Assert full vector (cross leg)\n"
        "        if: matrix.cross == 'true'\n"
        "        run: echo 'avif:encode=0 (configure-log honest zero)'\n"
    )
    assert re_mod.check(ledger=ledger, workflows_dir=workflows_dir) == []


def test_check_step_anchor_catches_drift_within_shared_file(tmp_path):
    """Positive control for the fix above: a leg wired to the WRONG value
    inside its own anchored step must still be caught."""
    ledger = {
        "leg-native": {"instrument": "capability-probe", "workflow": "shared.yml",
                       "step_anchor": "native leg", "expect": {"avif:encode": 1}},
    }
    workflows_dir = tmp_path / "workflows"
    workflows_dir.mkdir()
    (workflows_dir / "shared.yml").write_text(
        "      - name: Assert full vector (native leg)\n"
        "        run: probe.py --expect avif:encode=0\n"
        "      - name: Assert full vector (cross leg)\n"
        "        run: echo 'avif:encode=1'\n"
    )
    disagreements = re_mod.check(ledger=ledger, workflows_dir=workflows_dir)
    assert any("leg-native" in d for d in disagreements), disagreements


def test_tokens_in_file_unknown_anchor_returns_empty(tmp_path):
    path = tmp_path / "f.yml"
    path.write_text("      - name: Something else\n        run: probe.py --expect avif:encode=1\n")
    assert re_mod._tokens_in_file(path, step_anchor="not present") == set()


def test_cli_leg_help_exits_zero():
    import subprocess
    result = subprocess.run(
        [sys.executable, str(MODULE_PATH), "--help"],
        capture_output=True, text=True,
    )
    assert result.returncode == 0
