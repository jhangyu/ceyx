#!/usr/bin/env python3
"""Measure the minimum runtime floor of a shipped platform artifact (WI-14 S-F1).

Spec: Halcyon/docs/logs/2026-09-12/platform-parity-plan.md, WI-14 step 14.1.

Per platform, the *property* measured is "the oldest runtime that can load
this file", and each reader states which load command / header field it read
so the emitted value is never an assumption dressed as a measurement:

  macos:   Mach-O LC_BUILD_VERSION (`minos`), falling back to
           LC_VERSION_MIN_MACOSX on older files. Read in pure Python from the
           file's own load commands (the same "structural read, no external
           tool can invert it" argument Halcyon's scripts/ci/assertions.py
           makes for H-ARCH), so it works identically on a non-macOS runner
           inspecting a fetched dylib. The floor is the MAX over the WHOLE
           shipped group, not the decoder alone (the macOS pin comment
           records exactly this: the bundled OpenMP runtime, not the
           decoder, set the real floor at macOS 15) -- pass every shipped
           file with --artifact (repeatable) and the max plus a per-file
           breakdown is emitted.
  windows: PE optional-header MajorSubsystemVersion.MinorSubsystemVersion.
           Emits PROVENANCE=linker-default when the value equals the linker
           default (6.0) observed today (rootcause-native-capability.md
           Item F) -- a default recorded as if measured is the failure this
           item exists to remove.
  linux:   the maximum GLIBC_x.y / GLIBCXX_x.y.z version reference in the
           dynamic symbol table. --artifact must be an ALREADY-CAPTURED
           `readelf --dyn-syms <so>` (or `nm -D`) dump FILE, never a pipe --
           this script only ever reads a file, it never shells out.
  android: minSdk read from plugin/android/build.gradle (a DECLARED floor,
           not a binary property -- labelled SOURCE=gradle-declaration, not
           SOURCE=binary).

Output: one line `MIN_RUNTIME_<platform>=<value>` to stdout AND to --out,
plus per-file breakdown lines. A malformed/unreadable file exits non-zero
rather than emitting an empty value -- never treat "could not read" as "0".

Usage:
    read_min_runtime.py --artifact <path> [--artifact <path> ...] \
        --platform macos --out min_runtime.txt
    read_min_runtime.py --artifact dng_decoder_native.dll --platform windows --out min_runtime.txt
    read_min_runtime.py --artifact readelf_dynsyms.txt --platform linux --out min_runtime.txt
    read_min_runtime.py --gradle plugin/android/build.gradle --platform android --out min_runtime.txt
"""
import argparse
import re
import struct
import sys
from pathlib import Path

# Mach-O load command constants.
LC_SEGMENT_64 = 0x19
LC_VERSION_MIN_MACOSX = 0x24
LC_BUILD_VERSION = 0x32
MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF


def _version_from_packed(value: int) -> str:
    """X.Y.Z packed as (X << 16) | (Y << 8) | Z, per Mach-O load-command spec."""
    major = (value >> 16) & 0xFFFF
    minor = (value >> 8) & 0xFF
    patch = value & 0xFF
    if patch:
        return f"{major}.{minor}.{patch}"
    return f"{major}.{minor}"


def read_macho_minos(data: bytes) -> tuple[str, str]:
    """Return (version, source_field) for one Mach-O (thin, 64-bit) file's slice."""
    if len(data) < 32:
        raise ValueError("file too short to carry a Mach-O header")
    magic = struct.unpack_from(">I", data, 0)[0]
    if magic not in (MH_MAGIC_64, 0xCFFAEDFE):
        raise ValueError(f"not a thin Mach-O 64-bit file (magic 0x{magic:x})")
    # mach_header_64: magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved
    ncmds = struct.unpack_from("<I", data, 16)[0]
    offset = 32  # sizeof(mach_header_64)
    found_min_macosx = None
    found_build_version = None
    for _ in range(ncmds):
        if offset + 8 > len(data):
            break
        cmd, cmdsize = struct.unpack_from("<II", data, offset)
        if cmd == LC_BUILD_VERSION:
            # cmd, cmdsize, platform, minos, sdk, ntools
            minos = struct.unpack_from("<I", data, offset + 12)[0]
            found_build_version = _version_from_packed(minos)
        elif cmd == LC_VERSION_MIN_MACOSX:
            # cmd, cmdsize, version, sdk
            version = struct.unpack_from("<I", data, offset + 8)[0]
            found_min_macosx = _version_from_packed(version)
        offset += cmdsize
        if cmdsize == 0:
            break
    if found_build_version is not None:
        return found_build_version, "LC_BUILD_VERSION"
    if found_min_macosx is not None:
        return found_min_macosx, "LC_VERSION_MIN_MACOSX"
    raise ValueError("no LC_BUILD_VERSION or LC_VERSION_MIN_MACOSX load command found")


def _ver_key(v: str) -> tuple:
    return tuple(int(p) for p in v.split("."))


def macos_min_runtime(paths: list[Path]) -> tuple[str, list[str]]:
    breakdown = []
    best = None
    for p in paths:
        data = p.read_bytes()
        magic = struct.unpack_from(">I", data, 0)[0] if len(data) >= 4 else 0
        if magic in (FAT_MAGIC, FAT_MAGIC_64):
            raise ValueError(
                f"{p}: universal (fat) binaries are not supported by this reader; "
                "pass thin per-arch slices"
            )
        version, source = read_macho_minos(data)
        breakdown.append(f"{p.name}={version} (source={source})")
        if best is None or _ver_key(version) > _ver_key(best):
            best = version
    if best is None:
        raise ValueError("no artifacts given")
    return best, breakdown


# PE optional-header Magic values.
PE32_MAGIC = 0x10B
PE32_PLUS_MAGIC = 0x20B
LINKER_DEFAULT_SUBSYSTEM_VERSION = "6.0"


def windows_min_runtime(path: Path) -> tuple[str, str]:
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE file (missing MZ signature)")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_offset : pe_offset + 4] != b"PE\x00\x00":
        raise ValueError(f"{path}: MZ header without a PE signature")
    opt_header_offset = pe_offset + 4 + 20  # Signature(4) + FileHeader(20)
    magic = struct.unpack_from("<H", data, opt_header_offset)[0]
    if magic not in (PE32_MAGIC, PE32_PLUS_MAGIC):
        raise ValueError(f"{path}: unrecognised optional-header magic 0x{magic:x}")
    # MajorSubsystemVersion/MinorSubsystemVersion sit at the same offset (48/50)
    # in both PE32 and PE32+: the only preceding size difference (ImageBase,
    # 4 vs 8 bytes) is compensated by PE32's extra 4-byte BaseOfData field, so
    # the region from offset 40 onward is identically laid out in both.
    major = struct.unpack_from("<H", data, opt_header_offset + 48)[0]
    minor = struct.unpack_from("<H", data, opt_header_offset + 50)[0]
    version = f"{major}.{minor}"
    provenance = "linker-default" if version == LINKER_DEFAULT_SUBSYSTEM_VERSION else "measured"
    return version, provenance


GLIBC_RE = re.compile(r"GLIBC_(\d+\.\d+(?:\.\d+)?)")
GLIBCXX_RE = re.compile(r"GLIBCXX_(\d+\.\d+(?:\.\d+)?)")
CXXABI_RE = re.compile(r"CXXABI_(\d+\.\d+(?:\.\d+)?)")


def linux_min_runtime(dump_path: Path) -> tuple[str, list[str]]:
    text = dump_path.read_text(errors="replace")
    versions = {"GLIBC": set(), "GLIBCXX": set(), "CXXABI": set()}
    for m in GLIBC_RE.finditer(text):
        versions["GLIBC"].add(m.group(1))
    for m in GLIBCXX_RE.finditer(text):
        versions["GLIBCXX"].add(m.group(1))
    for m in CXXABI_RE.finditer(text):
        versions["CXXABI"].add(m.group(1))
    if not any(versions.values()):
        raise ValueError(f"{dump_path}: no GLIBC_/GLIBCXX_/CXXABI_ version references found")
    breakdown = []
    max_glibc = None
    for family, vals in versions.items():
        if not vals:
            continue
        top = max(vals, key=_ver_key)
        breakdown.append(f"{family}_{top}")
        if family == "GLIBC" and (max_glibc is None or _ver_key(top) > _ver_key(max_glibc)):
            max_glibc = top
    if max_glibc is None:
        # No GLIBC_ symbol itself referenced (unusual but not impossible);
        # fall back to the highest of any family found so a value is still
        # emitted rather than silently treated as absent.
        max_glibc = max(
            (v for vals in versions.values() for v in vals), key=_ver_key
        )
    return max_glibc, breakdown


MINSDK_RE = re.compile(r"minSdk(?:Version)?\s*=?\s*(\d+)")


def android_min_runtime(gradle_path: Path) -> str:
    text = gradle_path.read_text(errors="replace")
    m = MINSDK_RE.search(text)
    if not m:
        raise ValueError(f"{gradle_path}: no minSdk/minSdkVersion declaration found")
    return m.group(1)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--artifact", action="append", default=[],
                     help="path to a shipped file to measure; repeatable "
                          "(macOS: every shipped dylib; windows: the single DLL; "
                          "linux: an already-captured readelf/nm dump file)")
    ap.add_argument("--gradle", help="path to plugin/android/build.gradle (android only)")
    ap.add_argument("--platform", required=True,
                     choices=("macos", "windows", "linux", "android"))
    ap.add_argument("--out", required=True, help="file to also write MIN_RUNTIME_<platform>=<value> to")
    args = ap.parse_args()

    try:
        if args.platform == "macos":
            if not args.artifact:
                raise ValueError("--artifact is required (one or more) for macos")
            value, breakdown = macos_min_runtime([Path(p) for p in args.artifact])
            lines = [f"MIN_RUNTIME_macos={value}"] + [f"  {b}" for b in breakdown]
        elif args.platform == "windows":
            if len(args.artifact) != 1:
                raise ValueError("--artifact (exactly one) is required for windows")
            value, provenance = windows_min_runtime(Path(args.artifact[0]))
            lines = [f"MIN_RUNTIME_windows={value}", f"PROVENANCE={provenance}"]
        elif args.platform == "linux":
            if len(args.artifact) != 1:
                raise ValueError("--artifact (exactly one dump file) is required for linux")
            value, breakdown = linux_min_runtime(Path(args.artifact[0]))
            lines = [f"MIN_RUNTIME_linux=GLIBC_{value}"] + [f"  {b}" for b in breakdown]
        else:  # android
            if not args.gradle:
                raise ValueError("--gradle is required for android")
            value = android_min_runtime(Path(args.gradle))
            lines = [f"MIN_RUNTIME_android={value}", "SOURCE=gradle-declaration"]
    except (ValueError, OSError, struct.error) as exc:
        print(f"READ_MIN_RUNTIME_RC=1", file=sys.stderr)
        print(f"error: {exc}", file=sys.stderr)
        return 1

    for line in lines:
        print(line)
    Path(args.out).write_text("\n".join(lines) + "\n")
    print("READ_MIN_RUNTIME_RC=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
