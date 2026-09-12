"""Tests for gen_export_manifest.py (WI-13, S-G1).

Spec: Halcyon/docs/logs/2026-09-12/platform-parity-plan.md, WI-13 step 13.1.
"""
import pathlib
import sys
import textwrap

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import gen_export_manifest as gem  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]


def test_every_scanned_name_in_committed_manifest():
    """S-G1: every name the scanner finds in the real Dart sources is
    present in the committed manifest (the --check invariant, exercised
    directly against the live scan rather than shelling out)."""
    records, _ = gem.scan()
    manifest = gem.load_manifest(gem.MANIFEST_PATH)
    missing = set(records) - set(manifest)
    assert not missing, f"manifest missing scanned symbol(s): {sorted(missing)}"


def test_check_fails_when_manifest_is_missing_a_looked_up_name(tmp_path):
    """(b) a name removed from the manifest but present in Dart fails."""
    records, _ = gem.scan()
    rendered = gem.render_toml(records)
    # Drop one [[symbol]] record wholesale by removing its block.
    victim = "ceyx_decode_into_buffer_oriented"
    assert f'name       = "{victim}"' in rendered
    # Split on blank-line-delimited records and drop the victim's block.
    blocks = rendered.split("\n\n")
    blocks = [b for b in blocks if f'name       = "{victim}"' not in b]
    stale = "\n\n".join(blocks)
    manifest_path = tmp_path / "export_manifest.toml"
    manifest_path.write_text(stale, encoding="utf-8")

    rc = gem.main(["--manifest", str(manifest_path), "--check"])
    assert rc != 0


def test_check_fails_on_stale_extra_entry(tmp_path):
    """A manifest entry for a symbol no longer looked up in Dart must also
    fail --check (the mirror image of the missing-name case)."""
    records, _ = gem.scan()
    rendered = gem.render_toml(records)
    fabricated = textwrap.dedent(
        """
        [[symbol]]
        name       = "ceyx_totally_fake_symbol_wi13"
        platforms  = "all"
        group      = ""
        source     = ""
        dart_sites = []
        """
    ).strip()
    manifest_path = tmp_path / "export_manifest.toml"
    manifest_path.write_text(rendered.rstrip() + "\n\n" + fabricated + "\n", encoding="utf-8")

    rc = gem.main(["--manifest", str(manifest_path), "--check"])
    assert rc != 0


def test_process_and_kernel32_receivers_are_skipped_and_reported(capsys):
    """(c) a process()/kernel32 receiver is skipped and reported."""
    records, skipped = gem.scan()
    skipped_names = {name for name, _receiver, _rel, _kind in skipped}
    assert "dladdr" in skipped_names
    assert "GetModuleHandleExW" in skipped_names
    assert "GetModuleFileNameW" in skipped_names
    # Neither excluded symbol may leak into the scraped record set.
    assert "dladdr" not in records
    assert "GetModuleHandleExW" not in records
    assert "GetModuleFileNameW" not in records

    skipped_receivers = {receiver for _name, receiver, _rel, _kind in skipped}
    assert "ffi.DynamicLibrary.process()" in skipped_receivers
    assert "kernel32" in skipped_receivers

    rc = gem.main(["--check"])
    captured = capsys.readouterr()
    assert rc == 0
    assert "SKIPPED dladdr (receiver=ffi.DynamicLibrary.process())" in captured.err
    assert "SKIPPED GetModuleHandleExW (receiver=kernel32)" in captured.err
    assert "SKIPPED GetModuleFileNameW (receiver=kernel32)" in captured.err


def test_absent_platforms_field_is_rejected(tmp_path):
    """(d) an absent `platforms` field is rejected -- schema error, not a
    default."""
    bad_toml = textwrap.dedent(
        """
        [[symbol]]
        name       = "ceyx_missing_platforms_field"
        group      = ""
        source     = ""
        dart_sites = []
        """
    ).strip() + "\n"
    manifest_path = tmp_path / "export_manifest.toml"
    manifest_path.write_text(bad_toml, encoding="utf-8")

    manifest = gem.load_manifest(manifest_path)
    rec = manifest["ceyx_missing_platforms_field"]
    assert "platforms" not in rec, (
        "fixture must omit the field for this test to be meaningful"
    )
    with pytest.raises(KeyError):
        _ = rec["platforms"]


def test_every_guarded_symbol_has_a_non_empty_group():
    """S-G3 structural invariant: a symbol found inside a try/catch cluster
    must never carry an empty group id."""
    records, _ = gem.scan()
    # The known guarded clusters in today's dng_bindings.dart / siblings --
    # every one of these must have a non-empty group.
    guarded = [
        "dng_decode_and_process",
        "dng_decode_and_process_sized",
        "dng_debug_pool_checked_out",
        "raw_decode_and_process",
        "dng_decode_configure_slots",
        "ceyx_probe_output_size",
        "ceyx_decode_into_buffer_oriented",
        "ceyx_pool_aligned_alloc",
        "ceyx_pool_pressure_relief",
        "ceyx_still_decode_supports",
        "ceyx_encode_jpeg_rgba8",
        "ceyx_encode_rgba8",
        "heif_probe",
    ]
    for name in guarded:
        assert records[name]["group"], f"{name} is inside a try/catch but has no group"


def test_ungrouped_symbols_are_the_unguarded_module_level_lookups():
    """Lookups outside any try/catch (constructed unconditionally) must
    keep an empty group -- non-empty would falsely mark them guarded."""
    records, _ = gem.scan()
    unguarded = [
        "dng_decoder_warmup_for_size",
        "dng_free_result",
        "dng_extract_preview_jpeg",
        "dng_free_buffer",
    ]
    for name in unguarded:
        assert records[name]["group"] == "", f"{name} should be unguarded"


def test_manifest_is_current_check_mode_passes():
    """Round-trip: --check against the committed manifest must pass when
    nothing has drifted -- the generator's own positive control."""
    rc = gem.main(["--check"])
    assert rc == 0
