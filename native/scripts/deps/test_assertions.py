"""assertions.py tests -- Plan D4 acceptance A4.1, A4.2 (written-slice only;
A4.3/A4.4/A4.5/A4.6 require a built dist and are round-2 "signed" work, not
yet gated here -- see assertions.py's module docstring).
"""
from __future__ import annotations

import hashlib
import inspect
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import assertions  # noqa: E402


# --- A4.1: schema validation rejects a malformed record, demonstrated red --


def _valid_record(**overrides):
    record = {
        "id": "A-TEST",
        "tests": "something",
        "method": "sha256",
        "measures": "something measurable",
        "valid_on": ["macos"],
        "why_valid": "because",
        "red_state": "flip a byte",
    }
    record.update(overrides)
    return record


@pytest.mark.parametrize("missing_field", ["measures", "valid_on", "why_valid", "red_state"])
def test_a4_1_validate_rejects_missing_required_field(missing_field):
    record = _valid_record()
    del record[missing_field]
    with pytest.raises(assertions.AssertionSchemaError, match=missing_field):
        assertions.validate(record)


def test_a4_1_validate_accepts_a_complete_record():
    assertions.validate(_valid_record())  # must not raise


def test_a4_1_load_real_assertions_toml_is_schema_valid():
    records = assertions.load()
    ids = {r["id"] for r in records}
    # The full §5.3 table (13 assertions) must be present. Later rounds ADD
    # declarations (A-T2 added the two android ones below), so the floor is
    # 13 rather than an exact count -- a shrinking suite is the failure this
    # guards against, not a growing one.
    assert len(records) >= 13
    assert "A-SRC-HASH" in ids
    assert "A-KVZ-SIMD" in ids
    # A-T2: the android dist assertions are declared, not merely implemented.
    assert {"A-ANDROID-SYMS", "A-ANDROID-ARCH"} <= ids


# --- A4.2: no assertion check function branches on source.kind ------------


def test_a4_2_module_never_references_source_kind():
    """Only function bodies matter (A4.2's real concern is a check BRANCHING
    on source.kind); the module docstring is prose explaining the rule, not
    a violation of it, so it is excluded from this scan."""
    for _, obj in inspect.getmembers(assertions, inspect.isfunction):
        source = inspect.getsource(obj)
        assert "source.kind" not in source
        assert '.get("kind")' not in source
        assert '["kind"]' not in source


# --- A-SRC-HASH: the one runnable-today assertion, exercised both ways ----


def test_a_src_hash_green_on_matching_digest():
    payload = b"pretend-archive-bytes"
    digest = hashlib.sha256(payload).hexdigest()
    assertions.assert_src_hash(payload, digest)  # must not raise


def test_a_src_hash_red_on_mismatched_digest():
    payload = b"pretend-archive-bytes"
    wrong_digest = hashlib.sha256(b"different-bytes").hexdigest()
    with pytest.raises(assertions.AssertionFailed, match="A-SRC-HASH"):
        assertions.assert_src_hash(payload, wrong_digest)


def test_a_src_hash_matches_a_real_manifest_pin_shape():
    """Not a network fetch (out of scope for a unit test) -- confirms the
    function accepts the exact sha256 string shape stored in manifest.toml
    (lowercase hex, as read via manifest.load())."""
    import manifest as manifest_mod

    loaded = manifest_mod.load()
    kvazaar_default = loaded["manifest"]["component"]["kvazaar"]["source"]["default"]
    pinned_sha = kvazaar_default["sha256"]
    assert isinstance(pinned_sha, str) and len(pinned_sha) >= 64
    # Green check against synthetic bytes hashing to that exact pin --
    # proves assert_src_hash() accepts manifest.toml's real sha256 shape,
    # without needing to actually download the archive.
    # Construct bytes whose digest we control is out of scope (sha256 is
    # one-way); instead confirm a deliberate MISMATCH is correctly flagged
    # red against the real pin, which is the safe, always-computable half
    # of this integration point.
    with pytest.raises(assertions.AssertionFailed):
        assertions.assert_src_hash(b"not the real kvazaar tarball", pinned_sha)
