"""Tests for assert_exports.py (WI-13, S-G2/S-G3/S-G4)."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import assert_exports as ae  # noqa: E402
import gen_export_manifest as gem  # noqa: E402


def _macos_dump(names):
    lines = [f"0000000000001000 T _{n}" for n in names]
    return "\n".join(lines) + "\n"


def _linux_dump(names):
    lines = [f"0000000000001000 T {n}" for n in names]
    return "\n".join(lines) + "\n"


def _pe_dump(names):
    header = "    ordinal hint RVA      name\n\n"
    rows = [f"       {i + 1}    {i:X} 0000{1000 + i:X} {n}" for i, n in enumerate(names)]
    return header + "\n".join(rows) + "\n"


def test_parse_mach_o_style_strips_leading_underscore():
    text = _macos_dump(["ceyx_decode_into_buffer_oriented", "ceyx_encode_jpeg_rgba8"])
    present = ae.parse_dump(text, "macos")
    assert present == {"ceyx_decode_into_buffer_oriented", "ceyx_encode_jpeg_rgba8"}


def test_parse_elf_style():
    text = _linux_dump(["ceyx_decode_into_buffer_oriented", "ceyx_encode_jpeg_rgba8"])
    present = ae.parse_dump(text, "linux")
    assert present == {"ceyx_decode_into_buffer_oriented", "ceyx_encode_jpeg_rgba8"}


def test_parse_pe_dumpbin_style():
    text = _pe_dump(["ceyx_decode_into_buffer_oriented", "ceyx_encode_jpeg_rgba8"])
    present = ae.parse_dump(text, "windows")
    assert present == {"ceyx_decode_into_buffer_oriented", "ceyx_encode_jpeg_rgba8"}


def test_empty_dump_is_unverified():
    assert ae.parse_dump("", "macos") is None
    assert ae.parse_dump("   \n  \n", "linux") is None


def test_unparseable_dump_is_unverified():
    assert ae.parse_dump("this is not a symbol table\nat all\n", "macos") is None


def test_full_positive_control_passes_against_the_real_manifest(capsys):
    """Red-before-green positive control: dump EVERY symbol the manifest
    (scanned live from Dart) names, for one platform -- must be PASS."""
    records, _ = gem.scan()
    names = sorted(records)
    text = _linux_dump(names)
    rc = ae.run(gem.MANIFEST_PATH, "linux", text)
    captured = capsys.readouterr()
    assert rc == 0
    assert "EXPORTS_RESULT=PASS" in captured.out
    assert f"EXPORTS_CHECKED={len(names)}" in captured.out


def test_missing_mandatory_symbol_fails_red_before_green(capsys):
    """S-G2 red: remove ceyx_decode_into_buffer_oriented (the positive
    control, group='oriented' -- a guarded cluster, so its ABSENCE alone
    is reported via the GROUP line, not a bare SYMBOL-driven failure route;
    this exercises that the group's ABSENT-on-an-expecting-leg IS a
    failure) from the dump and confirm EXPORTS_RESULT=FAIL."""
    records, _ = gem.scan()
    names = [n for n in records if n != "ceyx_decode_into_buffer_oriented"]
    text = _linux_dump(names)
    rc = ae.run(gem.MANIFEST_PATH, "linux", text)
    captured = capsys.readouterr()
    assert rc == 1
    assert "SYMBOL ceyx_decode_into_buffer_oriented -> MISSING" in captured.out
    assert "GROUP oriented expected_on=" in captured.out
    assert "-> ABSENT" in captured.out
    assert "EXPORTS_RESULT=FAIL" in captured.out

    # GREEN: restore it and confirm the same leg now passes.
    text_green = _linux_dump(list(records))
    rc_green = ae.run(gem.MANIFEST_PATH, "linux", text_green)
    captured_green = capsys.readouterr()
    assert rc_green == 0
    assert "EXPORTS_RESULT=PASS" in captured_green.out


def test_missing_unguarded_mandatory_symbol_fails():
    """dng_free_result has group="" (unguarded, unconditional lookup) --
    its absence must fail directly, not via a GROUP line."""
    records, _ = gem.scan()
    names = [n for n in records if n != "dng_free_result"]
    text = _linux_dump(names)
    rc = ae.run(gem.MANIFEST_PATH, "linux", text)
    assert rc == 1


def test_partial_group_is_always_a_failure():
    """A guarded cluster with SOME but not all members present is PARTIAL
    -- a broken build, never silently accepted regardless of expected_on."""
    records, _ = gem.scan()
    # heif group has 3 members; drop only one.
    names = [n for n in records if n != "heif_release"]
    text = _linux_dump(names)
    rc = ae.run(gem.MANIFEST_PATH, "linux", text)
    import io
    import contextlib

    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        ae.run(gem.MANIFEST_PATH, "linux", text)
    out = buf.getvalue()
    assert "GROUP heif" in out
    assert "-> PARTIAL" in out
    assert rc == 1


def test_unverified_dump_is_non_zero_exit():
    rc = ae.run(gem.MANIFEST_PATH, "linux", "")
    assert rc == 1
