#!/usr/bin/env python3
"""Read native/deps/shipped_files.toml -- the single "what ships beside the
decoder" declaration -- and print the requested field(s) for one platform.

Spec: Halcyon/docs/logs/2026-09-12/platform-parity-plan.md, WI-15 step 15.3.
This is the ONE reader every consumer (CI staging assertions today; CMake
staging goes through the generated native/cmake/shipped_files.cmake instead,
since CMake cannot invoke Python mid-configure without extra machinery) uses
to turn the declaration into shell-consumable output, so no second copy of
"how to read this TOML" exists.

Usage:
    read_shipped_files.py --platform macos --decoder
    read_shipped_files.py --platform windows --companions
    read_shipped_files.py --platform linux --all
    read_shipped_files.py --platform android --json
"""
import argparse
import json
import pathlib
import sys
import tomllib

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_DECLARATION_PATH = REPO_ROOT / "native" / "deps" / "shipped_files.toml"

# Explicit dict of known platforms -- no fall-through. Same defect class
# WI-8 removes from Halcyon's _export_listing_commands: an unrecognised
# platform must fail loudly naming the known keys, not silently resolve to
# an empty/wrong table.
_KNOWN_PLATFORMS = ("windows", "linux", "macos", "android")


class DeclarationError(Exception):
    """The declaration file is malformed or missing a required key."""


def load_declaration(path=None):
    """Parse native/deps/shipped_files.toml. Returns {platform: {...}}.

    Every entry must carry decoder, companions, staged_from, placed, source.
    When placed is false, not_placed_reason is required.
    """
    path = pathlib.Path(path) if path is not None else DEFAULT_DECLARATION_PATH
    with open(path, "rb") as f:
        raw = tomllib.load(f)

    platforms = {}
    for platform, table in raw.items():
        for required in ("decoder", "companions", "staged_from", "placed", "source"):
            if required not in table:
                raise DeclarationError(
                    f"platform {platform!r} is missing required key {required!r}"
                )
        if not isinstance(table["companions"], list):
            raise DeclarationError(f"platform {platform!r}: 'companions' must be a list")
        if table["placed"] is False and not table.get("not_placed_reason"):
            raise DeclarationError(
                f"platform {platform!r}: placed=false requires a non-empty "
                f"'not_placed_reason'"
            )
        platforms[platform] = table
    return platforms


def _fail_unknown_platform(platform):
    known = ", ".join(sorted(_KNOWN_PLATFORMS))
    print(
        f"error: unknown platform {platform!r}. Known platforms: {known}",
        file=sys.stderr,
    )
    return 2


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", required=True, help="one of: " + ", ".join(_KNOWN_PLATFORMS))
    parser.add_argument("--declaration", default=None, help="override path to shipped_files.toml")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--decoder", action="store_true", help="print the decoder filename")
    group.add_argument("--companions", action="store_true", help="print space-separated companion filenames")
    group.add_argument("--all", action="store_true", help="print decoder + companions, space-separated")
    group.add_argument("--json", action="store_true", help="print the full platform entry as JSON")
    args = parser.parse_args(argv)

    if args.platform not in _KNOWN_PLATFORMS:
        return _fail_unknown_platform(args.platform)

    try:
        platforms = load_declaration(args.declaration)
    except (DeclarationError, FileNotFoundError, tomllib.TOMLDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.platform not in platforms:
        print(
            f"error: platform {args.platform!r} is a known platform but has no "
            f"entry in {args.declaration or DEFAULT_DECLARATION_PATH}",
            file=sys.stderr,
        )
        return 1

    entry = platforms[args.platform]

    if args.json:
        print(json.dumps(entry))
    elif args.decoder:
        print(entry["decoder"])
    elif args.companions:
        print(" ".join(entry["companions"]))
    elif args.all:
        print(" ".join([entry["decoder"]] + entry["companions"]))

    return 0


if __name__ == "__main__":
    sys.exit(main())
