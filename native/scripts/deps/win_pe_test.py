"""Tests for win_pe.py -- every one runs on macOS/Linux (spec §8.1).

Named ``win_pe_test.py`` rather than ``test_win_pe.py`` for two reasons:
pytest discovers ``*_test.py`` by default just the same, and the Windows
port's file-ownership boundary for this round is ``deps/win_*.py``. A
consequence worth knowing: ``test_no_shell_lint.py`` skips files whose name
starts with ``test_``, so THIS file is linted as production code. It
therefore contains no bad-pattern fixture strings (no pipe character, no
matcher-tool token inside a list literal); the fixtures below are all
tool OUTPUT samples, never argv.

Each assertion helper is demonstrated RED as well as green -- a check that
has never been seen to fail is not evidence (project convention, spec §5).
"""
from __future__ import annotations

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from win_pe import (
    PeAssertionFailed,
    PeInspectionError,
    assert_absent,
    assert_depends_on,
    assert_machine_x86_64,
    assert_symbol_exported,
    count_exported_symbols,
    resolve_existing,
    symbol_present,
    token_present_ci,
)

# A realistic dumpbin -exports fragment for heif.dll (trimmed).
DUMPBIN_EXPORTS = """Microsoft (R) COFF/PE Dumper Version 14.44
Dump of file heif.dll

File Type: DLL

  Section contains the following exports for heif.dll

    ordinal hint RVA      name

          1    0 0002B1F0 heif_check_filetype
          2    1 0002B210 heif_context_alloc
          3    2 0002B230 heif_decode_image
          4    3 0002B250 heif_decode_image_handle
          5    4 0002B270 heif_get_version
"""

# The same DLL's import table.
DUMPBIN_DEPENDENTS = """Microsoft (R) COFF/PE Dumper Version 14.44
Dump of file heif.dll

  Image has the following dependencies:

    libde265.dll
    KERNEL32.dll
"""

# A PE whose export table could not be read / exports nothing.
EMPTY_EXPORTS = """Microsoft (R) COFF/PE Dumper Version 14.44
Dump of file heif.dll

File Type: DLL
"""


class TestSymbolMatching(unittest.TestCase):
    def test_whole_word_match_is_green(self) -> None:
        self.assertTrue(symbol_present(DUMPBIN_EXPORTS, "heif_decode_image"))

    def test_substring_does_not_satisfy_a_longer_symbol(self) -> None:
        """The whole-word rule matters in the other direction: a table that
        holds ONLY `heif_decode_image_handle` must not satisfy a check for
        `heif_decode_image`."""
        only_handle = "          1    0 0002B250 heif_decode_image_handle\n"
        self.assertFalse(symbol_present(only_handle, "heif_decode_image"))

    def test_case_insensitive_token_match(self) -> None:
        self.assertTrue(token_present_ci(DUMPBIN_DEPENDENTS, "DE265"))
        self.assertFalse(token_present_ci(DUMPBIN_DEPENDENTS, "x265"))


class TestExportAssertion(unittest.TestCase):
    def test_green(self) -> None:
        assert_symbol_exported(DUMPBIN_EXPORTS, "heif_decode_image", dll_name="heif.dll")

    def test_red_when_table_is_populated_but_symbol_missing(self) -> None:
        """Real capability failure: the message must say the table WAS read,
        so the reader does not go hunting for an instrument problem."""
        with self.assertRaises(PeAssertionFailed) as ctx:
            assert_symbol_exported(DUMPBIN_EXPORTS, "heif_nonexistent_fn", dll_name="heif.dll")
        message = str(ctx.exception)
        self.assertIn("genuine capability failure", message)
        self.assertNotIn("INSTRUMENT", message)

    def test_red_when_nothing_is_exported_names_the_instrument(self) -> None:
        """Windows exports nothing by default -- an empty table must be
        reported as a suspected INSTRUMENT failure, not a capability one.
        This is the distinction that cost two CI rounds on 2026-08-30."""
        with self.assertRaises(PeAssertionFailed) as ctx:
            assert_symbol_exported(EMPTY_EXPORTS, "heif_decode_image", dll_name="heif.dll")
        message = str(ctx.exception)
        self.assertIn("INSTRUMENT", message)

    def test_export_count_separates_the_two_diagnoses(self) -> None:
        self.assertEqual(count_exported_symbols(DUMPBIN_EXPORTS), 5)
        self.assertEqual(count_exported_symbols(""), 0)

    def test_banner_prose_is_not_counted_as_exports(self) -> None:
        """Regression: an earlier token-scraping counter scored dumpbin's own
        banner as 10 exports for a DLL exporting nothing, which silently
        destroyed the empty-vs-populated distinction above."""
        self.assertEqual(count_exported_symbols(EMPTY_EXPORTS), 0)

    def test_llvm_nm_format_is_parsed_too(self) -> None:
        nm_text = "0002b230 T heif_decode_image\n0002b250 T heif_get_version\n"
        self.assertEqual(count_exported_symbols(nm_text), 2)
        self.assertTrue(symbol_present(nm_text, "heif_decode_image"))


class TestDependencyAssertion(unittest.TestCase):
    def test_green_on_the_literal_upstream_spelling(self) -> None:
        """heif.dll imports the literal name `libde265.dll` while its import
        library is `de265.lib`. That asymmetry is upstream convention and is
        deliberately NOT tidied -- a renamed DLL would simply never load."""
        assert_depends_on(DUMPBIN_DEPENDENTS, "de265", dll_name="heif.dll", consequence="x")
        self.assertIn("libde265.dll", DUMPBIN_DEPENDENTS)

    def test_red_when_hevc_decoder_absent(self) -> None:
        no_de265 = "  Image has the following dependencies:\n\n    KERNEL32.dll\n"
        with self.assertRaises(PeAssertionFailed):
            assert_depends_on(
                no_de265, "de265", dll_name="heif.dll", consequence="it would decode nothing."
            )


class TestContaminationScan(unittest.TestCase):
    def test_green_when_clean(self) -> None:
        assert_absent(DUMPBIN_EXPORTS, "x265", where="heif.dll", why="GPL-2.0")

    def test_red_fires_on_presence(self) -> None:
        """The direction that silently never fired in the shell original."""
        tainted = DUMPBIN_EXPORTS + "          6    5 0002B290 x265_encoder_open\n"
        with self.assertRaises(PeAssertionFailed):
            assert_absent(tainted, "x265", where="heif.dll", why="GPL-2.0 contamination")


class TestArchitectureCheck(unittest.TestCase):
    def test_green(self) -> None:
        self.assertTrue(
            assert_machine_x86_64("heif.dll: PE32+ executable (DLL) x86-64, for MS Windows", dll_names="heif.dll")
        )

    def test_unavailable_tool_returns_false_not_true(self) -> None:
        """Skipped must be distinguishable from passed."""
        self.assertFalse(assert_machine_x86_64(None, dll_names="heif.dll"))

    def test_red_on_wrong_architecture(self) -> None:
        with self.assertRaises(PeAssertionFailed):
            assert_machine_x86_64("heif.dll: PE32 executable (DLL) Intel 80386", dll_names="heif.dll")


class TestResolveExisting(unittest.TestCase):
    def test_picks_the_file_that_exists_not_the_first_candidate(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "lib").mkdir()
            (root / "lib" / "libde265.lib").write_text("x", encoding="utf-8")
            resolved = resolve_existing(
                root, ["lib/de265.lib", "lib/libde265.lib"], what="libde265 import library"
            )
            self.assertEqual(resolved, "lib/libde265.lib")

    def test_red_lists_what_was_installed(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "lib").mkdir()
            (root / "lib" / "something_else.lib").write_text("x", encoding="utf-8")
            with self.assertRaises(PeInspectionError) as ctx:
                resolve_existing(root, ["lib/de265.lib"], what="libde265 import library")
            self.assertIn("something_else.lib", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
