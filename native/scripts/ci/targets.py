"""Per-platform CI facts. DATA ONLY.

Frozen interface: docs/logs/2026-09-13/pyci-plan.md WI-1. Mirrors Halcyon's
own rule verbatim (Halcyon `scripts/ci/targets.py:1-7`, its own G-5): this is
the ONLY file under ``native/scripts/ci/`` allowed to state a per-platform
fact -- everywhere else platform is a parameter and the difference is a dict
lookup. No subprocess spawning here, and no behaviour: a reader must be
able to audit every value on this one screen against the workflow file:line
it was transcribed from, without running anything.

This module is designed for ADDITIVE extension across pushes (leader ruling
D2, carried into WI-1): a later push that needs a new per-platform fact (e.g.
WI-19's Windows ``dumpbin_tools`` companion key) adds a new dict entry here,
in the push that owns this file, never a refactor of the existing shape.
"""

from __future__ import annotations

# Keys and provenance, one screen:
#   artifact_path       the built shared library, relative to repo root
#                        linux_build.yml:497,728,795,832 ("$SO")
#                        windows_build.yml:498,769 ("$DLL")
#                        macos_build.yml:1029 (path: .../artifacts/native, the
#                          dylib itself is resolved from staged_dir at runtime
#                          by the caller since macOS builds two arch legs into
#                          the same build dir name, see the ``macos`` entry)
#                        android_build.yml:258 (glob, resolved at runtime)
#   staged_dir           the directory the built artifact + companions are
#                        collected into before upload
#                        linux/windows/macos: "${{ github.workspace }}/artifacts/native"
#                        (linux_build.yml:1026, windows_build.yml:1008,
#                        macos_build.yml:1029); android: ARTIFACT_DIR/native
#                        (android_build.yml:258)
#   dist_dir             the native/build-<platform> compiler output directory
#                        linux_build.yml:497 ("native/build-linux");
#                        windows_build.yml:441,484,498 ("native/build-windows");
#                        macos has no single dist dir name in-tree (per-arch
#                        build dirs), so this key is None on macos and the
#                        caller resolves it from CMake's own output.
#   shared_lib_glob      the glob used to find every companion shared library
#                        staged beside the decoder
#                        linux_build.yml: "*.so" (android_build.yml:231 uses
#                        the same shape for the NDK cross build);
#                        windows_build.yml:984 ("*.dll");
#                        macos_build.yml:951 ("*.dylib")
#   dump_format          which binary-format dumper reads the artifact
#                        ("elf"|"pe"|"macho")
#   strings_tools        ordered preference tuple for a literal string scan
#                        (C-G6): linux_build.yml:729-732 and
#                        windows_build.yml:770 both try llvm-strings first,
#                        falling back to the plain "strings" binary;
#                        android_build.yml uses the NDK's llvm-strings only
#                        (cross-arch host, no plain "strings" fallback makes
#                        sense against a target-arm64 .so)
#   readelf_tools        linux_build.yml:549,571 ("readelf -d" / "--dyn-syms");
#                        empty elsewhere (readelf is an ELF-only tool)
#   nm_tools             linux_build.yml:490,519 ("nm -D");
#                        macos_build.yml:820 ("nm -gU");
#                        android_build.yml:280-292 (NDK's own llvm-nm, not the
#                        host nm, because the .so is target-arm64 cross-built
#                        on an x86_64 host); empty on windows (dumpbin is
#                        preferred there, llvm-nm is windows_build.yml's own
#                        FALLBACK for export listing -- see nm_tools_fallback)
#   nm_tools_fallback    windows_build.yml:552-554 ("llvm-nm --extern-only
#                        --defined-only", used only if dumpbin fails)
#   objdump_tools        linux_build.yml:666-674 (objdump, AVX-512 gate);
#                        windows_build.yml:621-623 ("llvm-objdump -p", the
#                        dumpbin -dependents fallback)
#   dumpbin_tools        windows_build.yml:547 ("dumpbin -exports"),
#                        :616 ("dumpbin -dependents") -- single-dash spelling
#                        is deliberate (C-G8): a leading-slash argv gets
#                        rewritten into a Windows path under MSYS bash
#   c_compiler           the compiler the codec probe is compiled with
#                        linux_build.yml:858 ("clang"); macos_build.yml:474
#                        ("clang"); windows_build.yml:326,402 ("clang-cl",
#                        MANDATORY per windows_build.yml:114-119, cl.exe has
#                        no -ffp-contract=off equivalent); android has no
#                        local compile probe (cross-arch, dlopen-based probe
#                        is unreachable from the x86_64 runner, see
#                        android_build.yml:726 in the macOS-cross-leg comment
#                        family and the equivalent Android reasoning)
#   probe_link_style     ("posix"|"clang-cl"): linux/macos use -I/-L/-l/-o
#                        (posix); windows uses positional .lib + -I/-o only,
#                        no -L/-l (C-G7, windows_build.yml:858-869)
#   requires_arch        C-G9: True only for macos (per-arch legs,
#                        `--arch "${{ matrix.arch_tag }}"`); False elsewhere
#                        (single-arch legs, --arch is an argparse error)
#   expected_companions  the companion shared libraries staged beside the
#                        decoder, per native/deps/shipped_files.toml
#                        (read_shipped_files.py is the single reader of that
#                        declaration; this tuple is NOT a duplicate source of
#                        truth, it names the FILENAME PATTERN a gate greps
#                        for in a staged-set listing, e.g.
#                        linux_build.yml:886-892 "libheif.so.1"/"libde265.so.0")
#   declaration_platform the key `read_shipped_files.py --platform <key>`
#                        expects (read_shipped_files.py:33,
#                        _KNOWN_PLATFORMS = ("windows","linux","macos","android"))
TARGETS: dict = {
    "linux": {
        "artifact_path": "native/build-linux/libdng_decoder_native.so",
        "staged_dir": "artifacts/native",
        "dist_dir": "native/build-linux",
        "shared_lib_glob": "*.so",
        "dump_format": "elf",
        "strings_tools": ("llvm-strings", "strings"),
        "readelf_tools": ("readelf",),
        "nm_tools": ("nm",),
        "nm_tools_fallback": (),
        "objdump_tools": ("objdump",),
        "dumpbin_tools": (),
        "c_compiler": "clang",
        "probe_link_style": "posix",
        "requires_arch": False,
        "expected_companions": ("libheif.so.1", "libde265.so.0"),
        "declaration_platform": "linux",
    },
    "windows": {
        "artifact_path": "native/build-windows/dng_decoder_native.dll",
        "staged_dir": "artifacts/native",
        "dist_dir": "native/build-windows",
        "shared_lib_glob": "*.dll",
        "dump_format": "pe",
        "strings_tools": ("llvm-strings", "strings"),
        "readelf_tools": (),
        "nm_tools": (),
        "nm_tools_fallback": ("llvm-nm",),
        "objdump_tools": ("llvm-objdump",),
        "dumpbin_tools": ("dumpbin",),
        "c_compiler": "clang-cl",
        "probe_link_style": "clang-cl",
        "requires_arch": False,
        "expected_companions": ("heif.dll", "libde265.dll"),
        "declaration_platform": "windows",
    },
    "macos": {
        "artifact_path": None,
        "staged_dir": "artifacts/native",
        "dist_dir": None,
        "shared_lib_glob": "*.dylib",
        "dump_format": "macho",
        "strings_tools": (),
        "readelf_tools": (),
        "nm_tools": ("nm",),
        "nm_tools_fallback": (),
        "objdump_tools": (),
        "dumpbin_tools": (),
        "c_compiler": "clang",
        "probe_link_style": "posix",
        "requires_arch": True,
        "expected_companions": ("libheif.1.dylib", "libde265.0.dylib"),
        "declaration_platform": "macos",
    },
    "android": {
        "artifact_path": None,
        "staged_dir": "artifacts/native",
        "dist_dir": None,
        "shared_lib_glob": "*.so",
        "dump_format": "elf",
        "strings_tools": ("llvm-strings",),
        "readelf_tools": (),
        "nm_tools": ("llvm-nm",),
        "nm_tools_fallback": (),
        "objdump_tools": (),
        "dumpbin_tools": (),
        "c_compiler": None,
        "probe_link_style": None,
        "requires_arch": False,
        "expected_companions": (),
        "declaration_platform": "android",
    },
}

# Every entry must carry exactly these keys.
REQUIRED_KEYS = (
    "artifact_path",
    "staged_dir",
    "dist_dir",
    "shared_lib_glob",
    "dump_format",
    "strings_tools",
    "readelf_tools",
    "nm_tools",
    "nm_tools_fallback",
    "objdump_tools",
    "dumpbin_tools",
    "c_compiler",
    "probe_link_style",
    "requires_arch",
    "expected_companions",
    "declaration_platform",
)


def platform_names():
    """Sorted list of valid platform names."""
    return sorted(TARGETS)


def spec(platform: str, arch: str | None = None) -> dict:
    """Returns TARGETS[platform]; raises KeyError naming the known platforms.

    ``arch`` is accepted (not stored) so callers can pass it uniformly with
    ``--arch``; C-G9's requires_arch enforcement happens in ci.py's argparse
    wiring, not here -- this function states data, not validation behaviour.
    """
    try:
        return TARGETS[platform]
    except KeyError:
        raise KeyError(
            f"unknown platform {platform!r}; known platforms: {', '.join(platform_names())}"
        ) from None


def _validate():
    for name, entry in TARGETS.items():
        missing = [k for k in REQUIRED_KEYS if k not in entry]
        extra = [k for k in entry if k not in REQUIRED_KEYS]
        if missing or extra:
            raise ValueError(
                f"platform {name!r}: missing keys {missing}, unexpected keys {extra}"
            )


_validate()
