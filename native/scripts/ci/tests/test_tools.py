"""Unit tests for native/scripts/ci/tools.py (WI-3)."""

from __future__ import annotations

import unittest
from unittest import mock

from .. import tools


class FakeTargets:
    """A minimal stand-in for targets.spec(), so this suite does not
    depend on WI-1's targets.py landing first or on real host tools."""

    def __init__(self, spec_map):
        self._spec_map = spec_map

    def spec(self, platform):
        return self._spec_map[platform]


class ResolveOrderTests(unittest.TestCase):
    def test_declared_preference_order_honoured(self):
        fake = FakeTargets({"linux": {"strings_tools": ("llvm-strings", "strings")}})
        with mock.patch.object(tools, "targets", fake):
            # Only the SECOND candidate resolves -- must still pick the
            # first one that resolves, in the declared order, not just any.
            def which(name):
                return "/usr/bin/strings" if name == "strings" else None

            with mock.patch.object(tools.shutil, "which", side_effect=which):
                found = tools.resolve("strings", "linux")
        self.assertEqual(found, "/usr/bin/strings")

    def test_first_candidate_wins_when_both_resolve(self):
        fake = FakeTargets({"linux": {"nm_tools": ("nm", "llvm-nm")}})
        with mock.patch.object(tools, "targets", fake):
            with mock.patch.object(
                tools.shutil, "which", side_effect=lambda n: f"/usr/bin/{n}"
            ):
                found = tools.resolve("nm", "linux")
        self.assertEqual(found, "/usr/bin/nm")

    def test_single_string_value_is_one_candidate(self):
        fake = FakeTargets({"linux": {"c_compiler": "clang"}})
        with mock.patch.object(tools, "targets", fake):
            with mock.patch.object(
                tools.shutil, "which", return_value="/usr/bin/clang"
            ):
                found = tools.resolve("cc", "linux")
        self.assertEqual(found, "/usr/bin/clang")


class NotFoundTests(unittest.TestCase):
    def test_not_found_names_the_full_search_list(self):
        fake = FakeTargets(
            {"windows": {"dumpbin_tools": ("dumpbin", "llvm-dumpbin", "lib")}}
        )
        with mock.patch.object(tools, "targets", fake):
            with mock.patch.object(tools.shutil, "which", return_value=None):
                with self.assertRaises(tools.ToolNotFound) as ctx:
                    tools.resolve("dumpbin", "windows")
        message = str(ctx.exception)
        for candidate in ("dumpbin", "llvm-dumpbin", "lib"):
            self.assertIn(candidate, message)
        # Order preserved in the message too.
        self.assertLess(message.index("dumpbin"), message.index("llvm-dumpbin"))
        self.assertLess(message.index("llvm-dumpbin"), message.rindex("lib"))

    def test_unknown_kind_raises_with_known_kinds_listed(self):
        with self.assertRaises(tools.ToolNotFound) as ctx:
            tools.resolve("nonexistent-kind", "linux")
        self.assertIn("strings", str(ctx.exception))

    def test_missing_spec_key_raises(self):
        fake = FakeTargets({"android": {}})
        with mock.patch.object(tools, "targets", fake):
            with self.assertRaises(tools.ToolNotFound):
                tools.resolve("readelf", "android")


class ResolveOrReportTests(unittest.TestCase):
    def test_resolve_or_report_returns_none_and_reports_on_failure(self):
        fake = FakeTargets({"linux": {"strings_tools": ("strings",)}})
        with mock.patch.object(tools, "targets", fake):
            with mock.patch.object(tools.shutil, "which", return_value=None):
                with mock.patch("ci.report.error") as mock_error:
                    result = tools.resolve_or_report(
                        "strings", "linux", "custom error text"
                    )
        self.assertIsNone(result)
        mock_error.assert_called_once_with("custom error text")

    def test_resolve_or_report_returns_path_on_success(self):
        fake = FakeTargets({"linux": {"strings_tools": ("strings",)}})
        with mock.patch.object(tools, "targets", fake):
            with mock.patch.object(
                tools.shutil, "which", return_value="/usr/bin/strings"
            ):
                result = tools.resolve_or_report("strings", "linux", "unused")
        self.assertEqual(result, "/usr/bin/strings")


if __name__ == "__main__":
    unittest.main()
