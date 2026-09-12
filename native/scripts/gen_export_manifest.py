#!/usr/bin/env python3
"""Scan ceyx/plugin/lib/src/*.dart for native symbol lookup call sites and
write/validate native/deps/export_manifest.toml (WI-13, S-G1).

Spec: Halcyon/docs/logs/2026-09-12/platform-parity-plan.md, WI-13 step 13.1.

What this scrapes: every `<receiver>.lookupFunction<...>('name')` and
`<receiver>.lookup<...>('name')` call site across the binding files, where
`name` is a string literal (a dynamically-looked-up symbol carried in a
variable is never a manifest candidate -- there is nothing to record).

Scope exclusion (load-bearing): only lookups against the ceyx decoder handle
are scraped. The receiver expression is resolved TEXTUALLY -- this is a
scanner, not a Dart parser -- and only `_lib` (DngNativeBindings' own field)
or `lib` (the `fromLibrary`/constructor parameter every sibling binding class
uses) qualify. Two real non-ceyx receivers exist today and MUST be excluded:
`ffi.DynamicLibrary.process()` (the `dladdr` diagnostics lookup,
dng_bindings.dart) and `kernel32` (the `GetModuleHandleExW`/
`GetModuleFileNameW` diagnostics lookups added by WI-11). Every excluded
call site is reported on stderr as `SKIPPED <symbol> (receiver=<expr>)` --
visible, never silent.

Guarded-optional groups (S-G3): a lookup inside a `try { ... } catch` block
is part of a guarded-optional cluster and MUST carry a non-empty `group` id
-- an ungrouped entry inside a guarded try would silently enter the manifest
as mandatory. Group id is the PLAN-NAMED id where the plan names one
(`still`, `heif`, `encode_v2`, `sized_decode`, `slot_config`); every other
guarded cluster gets a derived id from its first symbol so the "non-empty"
invariant holds universally, not only for the five plan calls out by name.

Usage:
    gen_export_manifest.py               # regenerate and overwrite
    gen_export_manifest.py --check        # verify manifest matches scan + Dart;
                                           # non-zero exit names any mismatch
"""
import argparse
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC_DIR = REPO_ROOT / "plugin" / "lib" / "src"
NATIVE_SRC_DIR = REPO_ROOT / "native" / "src"
MANIFEST_PATH = REPO_ROOT / "native" / "deps" / "export_manifest.toml"

ALL_PLATFORMS = ["macos", "linux", "windows", "android"]

# Groups whose own Dart comments say they are NOT present on any current
# dylib -- only on pinned OLD dylibs kept for backward-compat tests
# (dng_bindings.dart:442-474 "WP5: guarded. Absent on every current dylib,
# present on the pinned old dylibs the symbol-absence tests load"; the
# `raw_decode_and_process` comment at :479-481 is the same shape: "Symbol
# absent -> rawDecodeAvailable stays false"). expected_on=[] for these means
# ABSENT is never a failure on any leg -- the opposite of every other
# guarded group, whose absence on a leg it should ship on IS a failure.
# Verified empirically against plugin/macos/Libraries/libdng_decoder_native.dylib
# (wi13-local-assert-green.txt): legacy_decode/debug_pool/sized_decode/raw
# are ABSENT there today; treating them as expected-everywhere was a bug
# caught by that run, not a hypothetical.
LEGACY_OPTIONAL_GROUPS = {"legacy_decode", "debug_pool", "sized_decode", "raw"}

ALLOWED_RECEIVERS = {"_lib", "lib"}

# Known group ids, keyed by the FIRST symbol looked up inside the cluster's
# try block. The five named directly in platform-parity-plan.md WI-13 step
# 13.2 ("still", "heif", "encode_v2", "sized_decode", "slot_config") plus
# every other currently-guarded cluster in dng_bindings.dart, so the
# generator never has to invent an id for a cluster that already has a
# canonical name in this codebase's own comments.
KNOWN_GROUP_IDS = {
    "ceyx_still_decode_supports": "still",
    "heif_probe": "heif",
    "ceyx_encode_rgba8": "encode_v2",
    "ceyx_encode_jpeg_rgba8": "encode",
    "dng_decode_and_process_sized": "sized_decode",
    "dng_decode_configure_slots": "slot_config",
    "dng_decode_and_process": "legacy_decode",
    "dng_debug_pool_checked_out": "debug_pool",
    "raw_decode_and_process": "raw",
    "ceyx_probe_output_size": "wp10_decode_into_buffer",
    "ceyx_decode_into_buffer_oriented": "oriented",
    "ceyx_pool_aligned_alloc": "pool_aligned",
    "ceyx_pool_pressure_relief": "pool_pressure_relief",
}

# Receiver: a dotted identifier, optionally call-suffixed (`.process()`),
# immediately preceding `.lookupFunction<...>(` / `.lookup<...>(`. `\s`
# already matches newlines, which is required: every multi-line call in
# this codebase wraps the receiver and the call onto separate lines.
# The generic argument list is restricted to characters that cannot appear
# outside a type signature (no `;`, `{`, `}`) -- without this restriction a
# failed match (e.g. `lib.lookup<ffi.Void>(symbol)`, whose arg is a variable,
# not a string literal) backtracks the lazy `.*?` across statement
# boundaries into an unrelated, later call site and misattributes its
# receiver/name. `(?!\w)` after the kind alternative stops "lookupFunction"
# degrading to a same-position "lookup" partial match.
CALL_RE = re.compile(
    r"(?P<receiver>[A-Za-z_][A-Za-z0-9_.]*(?:\(\))?)\s*\.\s*"
    r"(?P<kind>lookupFunction|lookup)(?!\w)\s*<[^;{}]*?>\s*\(\s*'(?P<name>[A-Za-z0-9_]+)'",
    re.DOTALL,
)

# A guarded cluster: `try { ... } catch`. Every binding file in this repo
# places the catch clause immediately after the try block's closing brace,
# with no nested try/catch inside -- verified by inspection of
# dng_bindings.dart, still_bindings.dart, heif_bindings.dart,
# encode_bindings.dart, encode_bindings_v2.dart at WI-13 authorship time.
TRY_BLOCK_RE = re.compile(r"try\s*\{(?P<body>.*?)\}\s*catch", re.DOTALL)


def _dart_files():
    return sorted(SRC_DIR.glob("*.dart"))


def _find_source(symbol):
    """Best-effort: the native/src file that DEFINES `symbol`, so the
    manifest's `source` field is more than a guess. Returns "" (and the
    caller records UNRESOLVED) when nothing matches -- never a wrong path."""
    if not NATIVE_SRC_DIR.is_dir():
        return ""
    pattern = re.compile(r"\b" + re.escape(symbol) + r"\s*\(")
    for path in sorted(NATIVE_SRC_DIR.rglob("*.cpp")) + sorted(
        NATIVE_SRC_DIR.rglob("*.h")
    ):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if pattern.search(text):
            return str(path.relative_to(REPO_ROOT))
    return ""


def scan():
    """Returns (records, skipped) where records is a dict name -> record
    dict, and skipped is a list of (symbol, receiver, file, kind) tuples for
    every excluded-receiver call site (dladdr, kernel32)."""
    records = {}
    skipped = []

    for path in _dart_files():
        text = path.read_text(encoding="utf-8")
        rel = str(path.relative_to(REPO_ROOT))

        # Map each character offset inside a try-block to that block's group
        # id, computed from the FIRST allowed-receiver call inside it (a
        # try block guarding only excluded receivers -- e.g. the dladdr /
        # GetModuleFileNameW diagnostics helpers, which have their own
        # try/catch -- yields no group and is simply not a guarded cluster
        # for manifest purposes).
        group_spans = []  # list of (start, end, group_id)
        for try_match in TRY_BLOCK_RE.finditer(text):
            body = try_match.group("body")
            body_start = try_match.start("body")
            first_allowed_name = None
            for call in CALL_RE.finditer(body):
                if call.group("receiver") in ALLOWED_RECEIVERS:
                    first_allowed_name = call.group("name")
                    break
            if first_allowed_name is None:
                continue  # guarded cluster has no ceyx-handle lookups
            group_id = KNOWN_GROUP_IDS.get(
                first_allowed_name, f"grp_{first_allowed_name}"
            )
            group_spans.append((body_start, try_match.end("body"), group_id))

        def group_for(offset):
            for start, end, gid in group_spans:
                if start <= offset < end:
                    return gid
            return ""

        for call in CALL_RE.finditer(text):
            receiver = call.group("receiver")
            name = call.group("name")
            if receiver not in ALLOWED_RECEIVERS:
                skipped.append((name, receiver, rel, call.group("kind")))
                continue
            group = group_for(call.start())
            rec = records.get(name)
            if rec is None:
                rec = {
                    "name": name,
                    "platforms": "all",
                    "group": group,
                    "source": _find_source(name),
                    "dart_sites": [],
                }
                records[name] = rec
            elif group and not rec["group"]:
                rec["group"] = group
            if rel not in rec["dart_sites"]:
                rec["dart_sites"].append(rel)

    return records, skipped


def _toml_escape(value):
    return value.replace("\\", "\\\\").replace('"', '\\"')


def render_toml(records):
    groups = sorted({rec["group"] for rec in records.values() if rec["group"]})
    lines = [
        "# export_manifest.toml -- GENERATED FILE, do not hand-edit.",
        "#",
        "# Derived from the Dart lookupFunction/lookup call sites in",
        "# plugin/lib/src/*.dart by native/scripts/gen_export_manifest.py",
        "# (WI-13, platform-parity-plan.md step 13.1). Regenerate with:",
        "#   python3 native/scripts/gen_export_manifest.py",
        "# native/scripts/tests/test_gen_export_manifest.py's --check test",
        "# asserts this file is up to date with the Dart source, so it",
        "# cannot drift from what the bindings actually look up.",
        "#",
        '# `platforms = "all"` is written explicitly on every record -- an',
        "# absent field is a schema error, not a default (the",
        "# four-places-drift lesson applies to schemas too).",
        "#",
        "# `group = \"\"` means mandatory (looked up unconditionally). A",
        "# non-empty group means the lookup sits inside a guarded try/catch",
        "# cluster (S-G3) -- see KNOWN_GROUP_IDS in the generator for the",
        "# plan-named ids and the derivation rule for the rest.",
        "#",
        "# [[group]] declares, per guarded cluster, which legs are expected",
        "# to have it (S-G3's expected_on). Default is all four legs --",
        "# assert_exports.py turns an ABSENT on an expecting leg into a",
        "# failure, never a silent degrade. EXCEPTION: legacy/backward-compat",
        "# groups (legacy_decode, debug_pool, sized_decode, raw) get an EMPTY",
        "# expected_on -- their own Dart comments say they are absent from",
        "# every CURRENT dylib and exist only for pinned OLD dylibs loaded by",
        "# symbol-absence tests, so ABSENT there is never a failure.",
        "",
    ]
    for group_id in groups:
        lines.append("[[group]]")
        lines.append(f'id          = "{_toml_escape(group_id)}"')
        platforms = [] if group_id in LEGACY_OPTIONAL_GROUPS else ALL_PLATFORMS
        expected = ", ".join(f'"{p}"' for p in platforms)
        lines.append(f"expected_on = [{expected}]")
        lines.append("")
    for name in sorted(records):
        rec = records[name]
        lines.append("[[symbol]]")
        lines.append(f'name       = "{_toml_escape(rec["name"])}"')
        lines.append(f'platforms  = "{_toml_escape(rec["platforms"])}"')
        lines.append(f'group      = "{_toml_escape(rec["group"])}"')
        lines.append(f'source     = "{_toml_escape(rec["source"])}"')
        sites = ", ".join(f'"{_toml_escape(s)}"' for s in rec["dart_sites"])
        lines.append(f"dart_sites = [{sites}]")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def load_manifest(path):
    import tomllib

    with open(path, "rb") as fh:
        data = tomllib.load(fh)
    return {rec["name"]: rec for rec in data.get("symbol", [])}


def load_groups(path):
    """Returns {group_id: expected_on_list} from the manifest's [[group]]
    table. Consumed by assert_exports.py (S-G3)."""
    import tomllib

    with open(path, "rb") as fh:
        data = tomllib.load(fh)
    return {g["id"]: g["expected_on"] for g in data.get("group", [])}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", default=None, help="override manifest path")
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the committed manifest matches the Dart source; do not write",
    )
    args = parser.parse_args(argv)

    manifest_path = pathlib.Path(args.manifest) if args.manifest else MANIFEST_PATH

    records, skipped = scan()
    for name, receiver, rel, kind in skipped:
        print(
            f"SKIPPED {name} (receiver={receiver}) [{kind}] in {rel}",
            file=sys.stderr,
        )

    rendered = render_toml(records)

    if args.check:
        if not manifest_path.exists():
            print(
                f"::error::{manifest_path} does not exist; run without --check to generate it.",
                file=sys.stderr,
            )
            return 1
        existing = load_manifest(manifest_path)
        scanned_names = set(records)
        existing_names = set(existing)
        missing = scanned_names - existing_names
        extra = existing_names - scanned_names
        if missing:
            for name in sorted(missing):
                sites = ", ".join(records[name]["dart_sites"])
                print(
                    f"::error::manifest missing '{name}', looked up in Dart at {sites}",
                    file=sys.stderr,
                )
        if extra:
            for name in sorted(extra):
                print(
                    f"::error::manifest has stale entry '{name}' -- no longer looked up in Dart",
                    file=sys.stderr,
                )
        if missing or extra:
            return 1
        current_text = manifest_path.read_text(encoding="utf-8")
        if current_text != rendered:
            print(
                "::error::export_manifest.toml is stale (regenerated content differs); "
                "run `python3 native/scripts/gen_export_manifest.py` and commit the result.",
                file=sys.stderr,
            )
            return 1
        print("export_manifest.toml is up to date.")
        return 0

    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(rendered, encoding="utf-8")
    print(f"Wrote {manifest_path} ({len(records)} symbols).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
