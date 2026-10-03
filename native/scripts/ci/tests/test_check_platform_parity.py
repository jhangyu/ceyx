import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_platform_parity as cpp  # noqa: E402


def _lister(files):
    return lambda root, prefix: [p for p in files if p.startswith(prefix)]


class TestCheckPlatformParity(unittest.TestCase):
    def _tree(self, tmp, files):
        for rel, text in files.items():
            path = Path(tmp) / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        return cpp.scan(Path(tmp), lister=_lister(list(files)))

    def test_unregistered_guard_fails(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            found = self._tree(tmp, {"native/src/x.cpp": "#if defined(_WIN32)\n#endif\n"})
            problems, _ = cpp.check(found, {}, require_closed=False)
        self.assertEqual(len(problems), 1)
        self.assertIn("UNREGISTERED", problems[0])

    def test_count_change_fails(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            found = self._tree(tmp, {"native/src/x.cpp": "#ifdef _WIN32\n#endif\n#if defined(__APPLE__)\n#endif\n"})
            reg = {"native/src/x.cpp": {"guard_lines": 1, "role": "adapter", "contract": "c"}}
            problems, _ = cpp.check(found, reg, require_closed=False)
        self.assertTrue(any("GUARD COUNT CHANGED" in p for p in problems))

    def test_stale_entry_fails(self):
        reg = {"native/src/gone.cpp": {"guard_lines": 1, "role": "adapter", "contract": "c"}}
        problems, _ = cpp.check({}, reg, require_closed=False)
        self.assertTrue(any("STALE ENTRY" in p for p in problems))

    def test_illegal_role_fails(self):
        found = {"native/src/x.cpp": 1}
        reg = {"native/src/x.cpp": {"guard_lines": 1, "role": "CLASSIFY", "contract": "CLASSIFY"}}
        problems, _ = cpp.check(found, reg, require_closed=False)
        self.assertTrue(any("ILLEGAL ROLE" in p for p in problems))

    def test_require_closed_fails_on_open_forks(self):
        found = {"native/src/x.cpp": 1}
        reg = {"native/src/x.cpp": {"guard_lines": 1, "role": "fork-host", "contract": "c", "open_forks": ["B1"]}}
        problems, open_ids = cpp.check(found, reg, require_closed=True)
        self.assertEqual(open_ids, ["B1"])
        self.assertTrue(any("OPEN FORKS" in p for p in problems))

    def test_dart_patterns(self):
        for line in ("if (Platform.isWindows) {", "    if (dart.library.ffi) 'a.dart';", "!kIsWeb &&"):
            self.assertTrue(cpp.DART_GUARD.search(line), line)
        self.assertIsNone(cpp.DART_GUARD.search("final platformName = 'x';"))

    def test_real_tree_matches_registry(self):
        found = cpp.scan(cpp.REPO_ROOT)
        problems, _ = cpp.check(found, cpp.REGISTRY, require_closed=cpp.REQUIRE_CLOSED)
        self.assertEqual(problems, [])


if __name__ == "__main__":
    unittest.main()
