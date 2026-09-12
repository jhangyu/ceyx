#!/usr/bin/env python3
"""Generate native/deps/linkage_table.md -- the S-D1 linkage/shipped-files
table -- from native/deps/manifest.toml and native/deps/shipped_files.toml.

Spec: Halcyon/docs/logs/2026-09-12/platform-parity-plan.md, WI-15 step 15.6.
`docs/` is never committed in ceyx, so the generated table lands beside the
declarations it derives from at native/deps/linkage_table.md instead.

Three columns, per platform:
  1. Statically linked third-party -- manifest.toml components with
     `linkage = "static"`, plus the policy facts the plan names (libjpeg-turbo
     static everywhere by App-Sandbox policy, third_party.cmake:65-230; zlib
     built from source statically on Windows, third_party.cmake:25-59, vs
     the OS libz on macOS/Linux).
  2. Dynamically shipped companions -- read from shipped_files.toml. THIS
     COLUMN MUST EQUAL that platform's pin `libraries[]` artifact names
     (S-D1's literal check); see COLUMN2_EQUALS_PIN below.
  3. Capabilities compiled in -- a cross-reference ONLY, "see
     native/deps/codec_expectations.toml [<leg>.capabilities]", no values
     duplicated here (duplicating would recreate the two-places drift this
     campaign removes; the lead ruling scopes capabilities to WI-5's S-E2
     table).

`--check` regenerates and diffs against the committed file (same
regenerate-and-diff pattern as gen_shipped_files_cmake.py), so the checked-in
table cannot go stale.

COLUMN2_EQUALS_PIN: when a sibling Halcyon checkout is present
(../../Halcyon/scripts/ceyx_release_pin.json relative to this repo root, i.e.
ceyx and Halcyon as sibling directories per Halcyon's own CLAUDE.md), this
script reads its `assets.<platform-fetch-target>.libraries[].artifact` set
and compares it against shipped_files.toml's [decoder] + [companions] set for
that platform, printing `COLUMN2_EQUALS_PIN=<bool>`. When absent, prints
`COLUMN2_EQUALS_PIN=SKIP (no sibling Halcyon checkout)` -- a silent skip is a
FAIL, so this line is always printed, never omitted.

The windows/android platform keys map 1:1 to a single pin fetch-target key
of the same name. linux and macos do not: the Windows/Android decoder +
companions are one archive, but linux's HEIF companions are not in any pin
fetch-target as of this writing (see the printed note when the comparison
finds no equivalent entry -- this is exactly the kind of drift this
declaration exists to expose, not a bug in this script), and macos's
"pin fetch-target" is arch-specific (macos-arm64 / macos-x86_64) while
shipped_files.toml has one macos entry for both arches (they ship identical
companion names).
"""
import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import read_shipped_files as rsf  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST_PATH = REPO_ROOT / "native" / "deps" / "manifest.toml"
DEFAULT_DECLARATION_PATH = REPO_ROOT / "native" / "deps" / "shipped_files.toml"
DEFAULT_OUTPUT_PATH = REPO_ROOT / "native" / "deps" / "linkage_table.md"
DEFAULT_PIN_PATH = REPO_ROOT.parent / "Halcyon" / "scripts" / "ceyx_release_pin.json"

# The codec_expectations.toml leg id(s) each shipped_files.toml platform maps
# to. macos maps to two legs (arm64 + x86_64 build the same companion names).
_PLATFORM_TO_LEGS = {
    "windows": ["windows-x86_64"],
    "linux": ["linux-x86_64"],
    "macos": ["macos-arm64", "macos-x86_64"],
    "android": ["android-arm64-v8a"],
}

# The pin's fetch-target key(s) each shipped_files.toml platform's decoder +
# companions correspond to, for the COLUMN2_EQUALS_PIN check. `None` means no
# equivalent pin entry names this platform's full companion set today (a
# drift the declaration surfaces, not something this script papers over).
_PLATFORM_TO_PIN_TARGET = {
    "windows": "windows",
    "android": "android",
    "linux": None,
    "macos": None,
}

# Policy facts (Statically linked third-party, column 1) not expressed as a
# `[component.*]` in manifest.toml, transcribed from third_party.cmake per
# the plan's step 15.6 text.
_STATIC_POLICY_FACTS = {
    "windows": [
        "libjpeg-turbo (static, App-Sandbox policy, third_party.cmake:65-230)",
        "zlib (static, built from source, third_party.cmake:25-59)",
    ],
    "linux": [
        "libjpeg-turbo (static, App-Sandbox policy, third_party.cmake:65-230)",
        "zlib (OS /usr/lib, dynamic -- not statically linked on Linux)",
    ],
    "macos": [
        "libjpeg-turbo (static, App-Sandbox policy, third_party.cmake:65-230)",
        "zlib (OS /usr/lib/libz.1, dynamic -- not statically linked on macOS)",
    ],
    "android": [
        "libjpeg-turbo (static, App-Sandbox policy, third_party.cmake:65-230)",
        "zlib (static, built from source, third_party.cmake:25-59)",
    ],
}


def load_static_components(manifest_path=None):
    """Return the sorted list of manifest.toml component names with
    linkage = "static" (halide excluded: its linkage is "n/a", a download-only
    distribution, not a statically-linked codec)."""
    import tomllib

    path = pathlib.Path(manifest_path) if manifest_path else DEFAULT_MANIFEST_PATH
    with open(path, "rb") as f:
        raw = tomllib.load(f)
    names = []
    for name, table in raw.get("component", {}).items():
        if table.get("linkage") == "static":
            names.append(name)
    return sorted(names)


def load_pin(pin_path=None):
    path = pathlib.Path(pin_path) if pin_path else DEFAULT_PIN_PATH
    if not path.exists():
        return None
    with open(path, "r") as f:
        return json.load(f)


def column2_equals_pin(platform, entry, pin):
    """Returns (status_str, equal_or_none)."""
    if pin is None:
        return "SKIP (no sibling Halcyon checkout)", None
    target = _PLATFORM_TO_PIN_TARGET.get(platform)
    if target is None:
        return "SKIP (no single pin fetch-target names this platform's full companion set)", None
    asset = pin.get("assets", {}).get(target)
    if asset is None:
        return f"SKIP (pin has no asset {target!r})", None
    pin_artifacts = sorted(lib["artifact"] for lib in asset.get("libraries", []))
    declared = sorted([entry["decoder"]] + entry["companions"])
    equal = pin_artifacts == declared
    return str(equal), equal


def render(platforms, static_components, pin):
    lines = [
        "<!-- linkage_table.md -- GENERATED FILE, do not hand-edit. -->",
        "<!-- Generated by native/scripts/gen_linkage_table.py (WI-15 step 15.6) -->",
        "<!-- from native/deps/manifest.toml + native/deps/shipped_files.toml. -->",
        "<!-- Regenerate with: python3 native/scripts/gen_linkage_table.py -->",
        "",
        "# S-D1: Linkage and shipped-files table",
        "",
        "| Platform | Statically linked third-party | Dynamically shipped companions | Capabilities compiled in |",
        "|---|---|---|---|",
    ]
    for platform in sorted(platforms):
        entry = platforms[platform]
        static_cell = "<br>".join(static_components + _STATIC_POLICY_FACTS.get(platform, []))
        dynamic_cell = "<br>".join([entry["decoder"]] + entry["companions"])
        legs = _PLATFORM_TO_LEGS.get(platform, [])
        cap_cell = "<br>".join(
            f"see `native/deps/codec_expectations.toml` `[{leg}]`" for leg in legs
        )
        lines.append(f"| {platform} | {static_cell} | {dynamic_cell} | {cap_cell} |")

    lines.append("")
    lines.append("## COLUMN2_EQUALS_PIN")
    lines.append("")
    for platform in sorted(platforms):
        status, _ = column2_equals_pin(platform, platforms[platform], pin)
        lines.append(f"COLUMN2_EQUALS_PIN({platform})={status}")

    return "\n".join(lines).rstrip() + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", default=None)
    parser.add_argument("--declaration", default=None)
    parser.add_argument("--pin", default=None, help="override path to Halcyon's ceyx_release_pin.json")
    parser.add_argument("--output", default=None)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)

    output_path = pathlib.Path(args.output) if args.output else DEFAULT_OUTPUT_PATH

    try:
        static_components = load_static_components(args.manifest)
        platforms = rsf.load_declaration(args.declaration)
    except (rsf.DeclarationError, FileNotFoundError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    pin = load_pin(args.pin)
    rendered = render(platforms, static_components, pin)

    # Always print the COLUMN2_EQUALS_PIN lines to stdout too, unconditionally
    # -- a silent skip is a FAIL (plan wording), so this must never be
    # omitted even in --check mode.
    for platform in sorted(platforms):
        status, _ = column2_equals_pin(platform, platforms[platform], pin)
        print(f"COLUMN2_EQUALS_PIN({platform})={status}")

    if args.check:
        if not output_path.exists():
            print(f"error: {output_path} does not exist -- run without --check to generate it", file=sys.stderr)
            return 1
        current = output_path.read_text()
        if current != rendered:
            print(f"error: {output_path} is STALE relative to its sources", file=sys.stderr)
            return 1
        print(f"OK: {output_path} is up to date")
        return 0

    output_path.write_text(rendered)
    print(f"wrote {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
