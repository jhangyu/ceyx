"""Unit tests for native/scripts/verify_raw_provenance.py's hunk-anchored
applied-patch detection (git bypassed so the fallback predicate is what runs).

Run: python3 -m pytest native/scripts/tests/test_verify_raw_provenance.py -q
"""
import importlib.util
import sys
from pathlib import Path

CHECK_PATH = Path(__file__).resolve().parents[1] / "verify_raw_provenance.py"
spec = importlib.util.spec_from_file_location("verify_raw_provenance", CHECK_PATH)
vrp = importlib.util.module_from_spec(spec)
sys.modules["verify_raw_provenance"] = vrp
spec.loader.exec_module(vrp)


def _no_git(monkeypatch):
    def boom(*args, **kwargs):
        raise OSError("git disabled")
    monkeypatch.setattr(vrp.subprocess, "run", boom)


# Removes the first `get()` call site; the second one legitimately stays.
PATCH_REMOVE = (
    "--- a/f.cpp\n+++ b/f.cpp\n"
    "@@ -1,5 +1,4 @@\n"
    " void a() {\n"
    "-  int d = get(x);\n"
    "   use(d);\n"
    " }\n"
    " void b() {\n"
)
APPLIED = "void a() {\n  use(d);\n}\nvoid b() {\n  int d = get(x);\n}\n"
UNAPPLIED = "void a() {\n  int d = get(x);\n  use(d);\n}\nvoid b() {\n  int d = get(x);\n}\n"

PATCH_ADD = (
    "--- a/f.cpp\n+++ b/f.cpp\n"
    "@@ -1,3 +1,4 @@\n"
    " #include <a>\n"
    "+#include <thread>\n"
    " }\n"
    " }\n"
)


def _write(tmp_path, patch_text, source):
    patch = tmp_path / "x.patch"
    patch.write_text(patch_text)
    (tmp_path / "f.cpp").write_text(source)
    return patch


def test_removed_line_elsewhere_in_file_is_not_a_false_positive(tmp_path, monkeypatch):
    _no_git(monkeypatch)
    patch = _write(tmp_path, PATCH_REMOVE, APPLIED)
    assert vrp.check_patch_applied(patch, False, root=tmp_path) == (True, "")


def test_unapplied_deletion_hunk_is_flagged(tmp_path, monkeypatch):
    _no_git(monkeypatch)
    patch = _write(tmp_path, PATCH_REMOVE, UNAPPLIED)
    ok, detail = vrp.check_patch_applied(patch, False, root=tmp_path)
    assert not ok and "un-patched hunk" in detail


def test_applied_addition_hunk_passes_and_missing_one_is_flagged(tmp_path, monkeypatch):
    _no_git(monkeypatch)
    patch = _write(tmp_path, PATCH_ADD, "#include <a>\n#include <thread>\n}\n}\n")
    assert vrp.check_patch_applied(patch, False, root=tmp_path) == (True, "")
    (tmp_path / "f.cpp").write_text("#include <a>\n}\n}\n")
    ok, detail = vrp.check_patch_applied(patch, False, root=tmp_path)
    assert not ok and "missing expected hunk" in detail


def test_hunk_superseded_by_later_patch_is_not_flagged(tmp_path, monkeypatch):
    _no_git(monkeypatch)
    patch = _write(tmp_path, PATCH_ADD, "#include <a>\n#include <other>\n}\n}\n")
    later = tmp_path / "later.patch"
    later.write_text(PATCH_ADD.replace("<thread>", "<other>"))
    assert vrp.check_patch_applied(patch, False, root=tmp_path)[0] is False
    assert vrp.check_patch_applied(patch, False, root=tmp_path, later_patches=[later]) == (True, "")


def test_removed_line_starting_with_dashes_is_not_read_as_a_file_header():
    text = (
        "--- a/f.txt\n+++ b/f.txt\n"
        "@@ -1,2 +1,1 @@\n"
        " keep\n"
        "--- old comment\n"
        "--- a/g.txt\n+++ b/g.txt\n"
        "@@ -1 +1 @@\n"
        "-x\n+y\n"
    )
    hunks = list(vrp.parse_patch_hunks(text))
    assert [(h[0], h[3], h[4]) for h in hunks] == [("f.txt", 0, 1), ("g.txt", 1, 1)]


def test_primary_scratch_git_judges_through_a_symlink_and_flags_unapplied(tmp_path, capsys):
    """Real git, vendor file behind a symlink (git refuses that in place):
    the scratch-repo primary check must still judge, both ways."""
    real = tmp_path / "real"
    real.mkdir()
    root = tmp_path / "vendor"
    root.mkdir()
    (root / "f.cpp").symlink_to(real / "f.cpp")
    patch = tmp_path / "x.patch"
    patch.write_text(PATCH_ADD)
    (real / "f.cpp").write_text("#include <a>\n#include <thread>\n}\n}\n")
    assert vrp.check_patch_applied(patch, False, root=root) == (True, "")
    assert "judged by primary-scratch-git" in capsys.readouterr().out
    (real / "f.cpp").write_text("#include <a>\n}\n}\n")
    ok, detail = vrp.check_patch_applied(patch, False, root=root)
    assert not ok and "missing expected hunk" in detail
    assert "primary-scratch-git rejected" in capsys.readouterr().out
