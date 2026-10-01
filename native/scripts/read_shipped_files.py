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
    read_shipped_files.py --platform windows --arch arm64 --companions
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

    Every entry must carry decoder, staged_from, placed, source, and EXACTLY
    ONE of ``companions`` (one list for every arch the platform builds) or
    ``companions_by_arch`` (a table keyed by the canonical arch vocabulary of
    native/deps/arch_map.toml, for a platform whose companion NAMES differ by
    arch -- Windows: libomp140.x86_64.dll vs libomp140.aarch64.dll). Read the
    list through :func:`companions_for`, never by indexing ``companions``
    directly: a per-arch entry has no ``companions`` key, so a reader that
    ignores arch fails loudly (KeyError) instead of silently getting another
    arch's list. When placed is false, not_placed_reason is required.
    """
    path = pathlib.Path(path) if path is not None else DEFAULT_DECLARATION_PATH
    with open(path, "rb") as f:
        raw = tomllib.load(f)

    platforms = {}
    for platform, table in raw.items():
        for required in ("decoder", "staged_from", "placed", "source"):
            if required not in table:
                raise DeclarationError(
                    f"platform {platform!r} is missing required key {required!r}"
                )
        has_flat = "companions" in table
        has_by_arch = "companions_by_arch" in table
        if has_flat == has_by_arch:
            raise DeclarationError(
                f"platform {platform!r}: exactly one of 'companions' / "
                f"'companions_by_arch' is required (found {'both' if has_flat else 'neither'})"
            )
        lists = [table["companions"]] if has_flat else list(table["companions_by_arch"].values())
        if has_by_arch and not table["companions_by_arch"]:
            raise DeclarationError(f"platform {platform!r}: 'companions_by_arch' is empty")
        if not all(isinstance(v, list) for v in lists):
            raise DeclarationError(f"platform {platform!r}: companion lists must be lists")
        if table["placed"] is False and not table.get("not_placed_reason"):
            raise DeclarationError(
                f"platform {platform!r}: placed=false requires a non-empty "
                f"'not_placed_reason'"
            )
        platforms[platform] = table
    return platforms


def companions_for(entry, arch=None):
    """The companion list for one arch of one platform entry.

    A flat ``companions`` entry is arch-independent and ignores ``arch``. A
    ``companions_by_arch`` entry REQUIRES ``arch`` and a matching key -- no
    default arch, so a caller that forgot to pass one cannot silently ship
    x86_64 names inside an arm64 artifact.
    """
    if "companions" in entry:
        return list(entry["companions"])
    by_arch = entry["companions_by_arch"]
    if arch is None:
        raise DeclarationError(
            f"companions differ by arch here; an arch is required (known: {sorted(by_arch)})"
        )
    if arch not in by_arch:
        raise DeclarationError(f"no companions declared for arch {arch!r} (known: {sorted(by_arch)})")
    return list(by_arch[arch])


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
    parser.add_argument("--arch", default=None, help="required when the platform declares companions_by_arch")
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
    else:
        try:
            companions = companions_for(entry, args.arch)
        except DeclarationError as exc:
            print(f"error: platform {args.platform!r}: {exc}", file=sys.stderr)
            return 1
        if args.companions:
            print(" ".join(companions))
        else:
            print(" ".join([entry["decoder"]] + companions))

    return 0


if __name__ == "__main__":
    sys.exit(main())
