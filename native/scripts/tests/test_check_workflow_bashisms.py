"""Unit tests for native/scripts/check_workflow_bashisms.py.

Positive control: a fixture containing each detected bash-only construct
must be flagged. Negative control: the same fixture rewritten in POSIX sh
must pass clean. Run: python3 -m pytest native/scripts/tests/test_check_workflow_bashisms.py -q
"""
import importlib.util
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
CHECK_PATH = REPO_ROOT / "native" / "scripts" / "check_workflow_bashisms.py"

spec = importlib.util.spec_from_file_location("check_workflow_bashisms", CHECK_PATH)
cwb = importlib.util.module_from_spec(spec)
sys.modules["check_workflow_bashisms"] = cwb
spec.loader.exec_module(cwb)


def _write(dirpath, name, content):
    path = dirpath / name
    path.write_text(content)
    return path


def test_negative_control_posix_sh_is_clean(tmp_path):
    _write(
        tmp_path,
        "linux_build.yml",
        "name: Linux Build\n"
        "jobs:\n"
        "  build:\n"
        "    steps:\n"
        "      - run: |\n"
        "          count=0\n"
        "          for f in native/*.so; do\n"
        "            [ -e \"$f\" ] || continue\n"
        "            count=$((count + 1))\n"
        "          done\n"
        "          echo \"SHARED_LIB_COUNT=${count}\"\n",
    )
    assert cwb.find_bashisms(tmp_path) == []


def test_positive_control_shopt_and_array_flagged(tmp_path):
    _write(
        tmp_path,
        "linux_build.yml",
        "name: Linux Build\n"
        "jobs:\n"
        "  build:\n"
        "    steps:\n"
        "      - run: |\n"
        "          shopt -s nullglob\n"
        "          libs=(native/*.so)\n"
        "          if [ ${#libs[@]} -eq 0 ]; then exit 1; fi\n",
    )
    hits = cwb.find_bashisms(tmp_path)
    hit_lines = {lineno for _, lineno, _ in hits}
    # shopt (line 6), array assignment (line 7), ${#libs[ (line 8) all flagged.
    assert 6 in hit_lines
    assert 7 in hit_lines
    assert 8 in hit_lines


def test_double_bracket_flagged(tmp_path):
    _write(
        tmp_path,
        "linux_build.yml",
        "name: Linux Build\n"
        "jobs:\n"
        "  build:\n"
        "    steps:\n"
        "      - run: |\n"
        "          if [[ -n \"$FOO\" ]]; then echo yes; fi\n",
    )
    hits = cwb.find_bashisms(tmp_path, target_files=("linux_build.yml",))
    assert any(lineno == 6 for _, lineno, _ in hits)


def test_comment_lines_ignored(tmp_path):
    _write(
        tmp_path,
        "linux_build.yml",
        "name: Linux Build\n"
        "jobs:\n"
        "  build:\n"
        "    steps:\n"
        "      - run: |\n"
        "          # shopt -s nullglob (example, not executed)\n"
        "          echo ok\n",
    )
    assert cwb.find_bashisms(tmp_path) == []


def test_real_workflow_dir_is_clean_after_round6_fix():
    """Regression guard: the repo's real .github/workflows/ must be clean
    after the round-6 fix (task #20) landed. WI-25 widened the default
    target set from ("linux_build.yml",) to every workflow file, so this
    call (target_files omitted -> _default_target_files scans the whole
    directory) is the live proof the widening didn't turn up a false
    positive on android_build.yml/macos_build.yml/windows_build.yml."""
    hits = cwb.find_bashisms(REPO_ROOT / ".github" / "workflows")
    assert hits == [], hits


def test_flags_bashism_in_any_workflow_file(tmp_path):
    """WI-25: the bash-only-construct scan is no longer scoped to
    linux_build.yml -- a bashism planted in a DIFFERENT workflow file
    (macos_build.yml, never scanned before this push) must be caught when
    target_files is omitted (the widened default)."""
    _write(
        tmp_path,
        "macos_build.yml",
        "name: macOS Build\n"
        "jobs:\n"
        "  build:\n"
        "    steps:\n"
        "      - run: |\n"
        "          declare -a libs\n",
    )
    hits = cwb.find_bashisms(tmp_path)  # target_files omitted: widened default
    assert any(lineno == 6 for _, lineno, _ in hits), hits


def test_non_compliant_body_flagged_when_not_allowlisted(tmp_path, monkeypatch):
    """WI-25's new instrument: a `run:` body that is neither a one-line
    python3 call nor allowlisted is a failure, independent of the bashism
    regex (this body contains no bash-only construct at all)."""
    monkeypatch.syspath_prepend(str(REPO_ROOT / "native" / "scripts"))
    from ci import allowlist  # noqa: E402

    monkeypatch.setattr(allowlist, "MUST_STAY", ())
    _write(
        tmp_path,
        "windows_build.yml",
        "name: Windows Build\n"
        "jobs:\n"
        "  build:\n"
        "    steps:\n"
        "      - name: Multi-line shell, not allowlisted\n"
        "        run: |\n"
        "          echo one\n"
        "          echo two\n",
    )
    hits = cwb.find_non_compliant_bodies(tmp_path)
    assert len(hits) == 1
    _, _lineno, step_name, n = hits[0]
    assert step_name == "Multi-line shell, not allowlisted"
    assert n == 2


def test_compliant_one_line_python_body_not_flagged(tmp_path):
    """A step whose body is exactly one `python3 ...` line is compliant on
    its own merits, with no allowlist entry needed."""
    _write(
        tmp_path,
        "windows_build.yml",
        "name: Windows Build\n"
        "jobs:\n"
        "  build:\n"
        "    steps:\n"
        "      - name: Compliant\n"
        "        run: python3 native/scripts/ci.py provision locate-clang-cl\n",
    )
    assert cwb.find_non_compliant_bodies(tmp_path) == []


def test_allowlisted_non_compliant_body_not_flagged(tmp_path, monkeypatch):
    """The same non-compliant shape as
    test_non_compliant_body_flagged_when_not_allowlisted, but with a
    matching (workflow, step_name) entry on MUST_STAY -- must NOT be
    flagged, proving the allowlist lookup (not just the compliance check)
    is exercised."""
    monkeypatch.syspath_prepend(str(REPO_ROOT / "native" / "scripts"))
    from ci import allowlist  # noqa: E402

    monkeypatch.setattr(
        allowlist,
        "MUST_STAY",
        (allowlist.Entry("windows_build.yml", "Allowlisted multi-line", "test fixture reason"),),
    )
    _write(
        tmp_path,
        "windows_build.yml",
        "name: Windows Build\n"
        "jobs:\n"
        "  build:\n"
        "    steps:\n"
        "      - name: Allowlisted multi-line\n"
        "        run: |\n"
        "          echo one\n"
        "          echo two\n",
    )
    assert cwb.find_non_compliant_bodies(tmp_path) == []


_CONTAINER_JOB = (
    "jobs:\n"
    "  build:\n"
    "    runs-on: ubuntu-24.04\n"
    "    container: ubuntu:22.04\n"
    "    steps:\n"
    "      - name: Probe\n"
    "{shell}"
    "        run: |\n"
    "          {line}\n"
)
_PLAIN_JOB = _CONTAINER_JOB.replace("    container: ubuntu:22.04\n", "")

# Fatal or silently wrong under dash (Ubuntu 22.04's /bin/sh).
_DASH_FATAL_LINES = [
    'grep -q x <<< "$y"',
    "printf $'\\t'",
    "make &> build.log",
    "make |& tee build.log",
    'echo "${name//-/_}"',
    'echo "${name^^}"',
    "source env.sh",
]


@pytest.mark.parametrize("line", _DASH_FATAL_LINES)
def test_dash_fatal_construct_in_container_keyless_step_flagged(tmp_path, line):
    _write(tmp_path, "linux_build.yml", _CONTAINER_JOB.format(shell="", line=line))
    hits = cwb.find_dash_fatal(tmp_path)
    assert [h[1] for h in hits] == [8], hits


@pytest.mark.parametrize("line", _DASH_FATAL_LINES)
def test_same_construct_under_shell_bash_is_clean(tmp_path, line):
    _write(tmp_path, "linux_build.yml",
           _CONTAINER_JOB.format(shell="        shell: bash\n", line=line))
    assert cwb.find_dash_fatal(tmp_path) == []


@pytest.mark.parametrize("line", _DASH_FATAL_LINES)
def test_same_construct_in_non_container_keyless_step_is_clean(tmp_path, line):
    _write(tmp_path, "linux_build.yml", _PLAIN_JOB.format(shell="", line=line))
    assert cwb.find_dash_fatal(tmp_path) == []


def test_local_is_not_flagged(tmp_path):
    """dash supports `local`; flagging it would be a false positive."""
    _write(tmp_path, "linux_build.yml", _CONTAINER_JOB.format(shell="", line="local x=1"))
    assert cwb.find_dash_fatal(tmp_path) == []
