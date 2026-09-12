"""Unit tests for native/scripts/check_workflow_bashisms.py.

Positive control: a fixture containing each detected bash-only construct
must be flagged. Negative control: the same fixture rewritten in POSIX sh
must pass clean. Run: python3 -m pytest native/scripts/tests/test_check_workflow_bashisms.py -q
"""
import importlib.util
import sys
from pathlib import Path

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
    after the round-6 fix (task #20) landed."""
    hits = cwb.find_bashisms(REPO_ROOT / ".github" / "workflows")
    assert hits == [], hits
