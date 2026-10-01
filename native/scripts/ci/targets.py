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
#   arch_tags            Phase 2 first item: the arch strings a leg actually
#                        builds, in the order the workflow declares them.
#                        Until now these literals existed ONLY hand-copied
#                        into YAML, which is the duplicated-description shape
#                        this phase exists to remove -- `requires_arch` said
#                        WHETHER a leg is per-arch but never WHICH arch, so
#                        the renderer had nothing to render `--arch` from.
#                        Transcribed from:
#                          linux   "x86_64"     linux_build.yml:431
#                          windows "x86_64","arm64"  windows_build.yml /
#                                               heif_dist_windows.yml /
#                                               jxl_dist_windows.yml /
#                                               webp_dist_windows.yml matrix
#                                               rows (windows-arm64 leg,
#                                               2026-09-30, contract
#                                               armci-contract.md R-6/R-7)
#                          android "arm64-v8a"  heif_dist_android.yml:104,
#                                               jxl_dist_android.yml:86,
#                                               webp_dist_android.yml:87
#                          macos   "arm64","x86_64"  macos_build.yml:92,106
#                                               (the `arch_tag` matrix values,
#                                               in declaration order)
#                        INVARIANT, enforced in _validate(): a leg is
#                        per-arch iff it has more than one arch tag, i.e.
#                        requires_arch == (len(arch_tags) > 1). The two keys
#                        state one fact; the assert is what keeps them from
#                        becoming two descriptions of it.
#   expected_companions  the companion shared libraries staged beside the
#                        decoder, per native/deps/shipped_files.toml
#                        (read_shipped_files.py is the single reader of that
#                        declaration; this tuple is NOT a duplicate source of
#                        truth, it names the FILENAME PATTERN a gate greps
#                        for in a staged-set listing, e.g.
#                        linux_build.yml:886-892 "libheif.so.1"/"libde265.so.0")
#   readobj_tools        the COFF header reader for the PE machine-type gate
#                        (windows only: "llvm-readobj --file-headers", LLVM's
#                        own format "Machine: IMAGE_FILE_MACHINE_<X> (0x..)",
#                        llvm/test/tools/llvm-readobj/COFF/file-headers.test);
#                        empty elsewhere
#   pe_machine_by_arch   windows only: canonical arch id -> the COFF machine
#                        constant every staged DLL must carry
#                        (IMAGE_FILE_MACHINE_AMD64 = 0x8664,
#                        IMAGE_FILE_MACHINE_ARM64 = 0xAA64); None elsewhere
#   arch_legs            windows only: canonical arch id -> the per-arch
#                        facts every rendered windows matrix row needs:
#                          runner       GitHub-hosted image the row runs on
#                                       (native build: the dist carriers and
#                                       the codec probes execute target-arch
#                                       code, so each arch runs on its own
#                                       machine -- windows-11-arm per the
#                                       actions/partner-runner-images
#                                       arm-windows-11-image.md listing)
#                          msvc_arch    ilammy/msvc-dev-cmd `arch:` input
#                                       (vcvarsall host/target spelling)
#                          dist_suffix  committed third-party dist dir suffix,
#                                       native/third_party/<c>-dist-<suffix>;
#                                       MUST equal native/cmake/heif.cmake's
#                                       CEYX_WINDOWS_DIST_SUFFIX for the same
#                                       arch (x86_64 keeps the historical
#                                       unsuffixed "windows")
#                        None elsewhere (their matrices, if any, live in
#                        hand-written workflows).
#   declaration_platform the key `read_shipped_files.py --platform <key>`
#                        expects (read_shipped_files.py:33,
#                        _KNOWN_PLATFORMS = ("windows","linux","macos","android"))
#   ndk_version          android only (None elsewhere): the NDK release every
#                        android workflow pins via nttld/setup-ndk
#                        `ndk-version:` (android_build.yml:72, and the rendered
#                        *_dist_android.yml via workflow_render.py); held
#                        equal by ci_conventions_check.py C11.
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
        "readobj_tools": (),
        "pe_machine_by_arch": None,
        "c_compiler": "clang",
        "probe_link_style": "posix",
        "requires_arch": False,
        "arch_tags": ("x86_64",),
        "arch_legs": None,
        "expected_companions": ("libheif.so.1", "libde265.so.0"),
        "declaration_platform": "linux",
        "min_runtime_source": "dump",
        "ndk_version": None,
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
        "readobj_tools": ("llvm-readobj",),
        "pe_machine_by_arch": {
            "x86_64": "IMAGE_FILE_MACHINE_AMD64",
            "arm64": "IMAGE_FILE_MACHINE_ARM64",
        },
        "c_compiler": "clang-cl",
        "probe_link_style": "clang-cl",
        "requires_arch": True,
        "arch_tags": ("x86_64", "arm64"),
        "arch_legs": {
            "x86_64": {"runner": "windows-latest", "msvc_arch": "x64", "dist_suffix": "windows"},
            # amd64_arm64, not a bare "arm64": it is the spelling Microsoft's
            # vcvarsall.bat reference documents for an ARM64 target
            # (MicrosoftDocs/cpp-docs building-on-the-command-line.md, the
            # `architecture` table); the x64-hosted tools it selects run under
            # Windows-on-ARM emulation. The compiler is clang-cl either way --
            # this input only chooses the ARM64 INCLUDE/LIB/linker environment.
            "arm64": {"runner": "windows-11-arm", "msvc_arch": "amd64_arm64", "dist_suffix": "windows-arm64"},
        },
        "expected_companions": ("heif.dll", "libde265.dll"),
        "declaration_platform": "windows",
        "min_runtime_source": "binary",
        "ndk_version": None,
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
        "readobj_tools": (),
        "pe_machine_by_arch": None,
        "c_compiler": "clang",
        "probe_link_style": "posix",
        "requires_arch": True,
        "arch_tags": ("arm64", "x86_64"),
        "arch_legs": None,
        "expected_companions": ("libheif.1.dylib", "libde265.0.dylib"),
        "declaration_platform": "macos",
        "min_runtime_source": "binary",
        "ndk_version": None,
    },
    "android": {
        "artifact_path": None,
        "staged_dir": "artifacts/native",
        "dist_dir": None,
        "shared_lib_glob": "*.so",
        "dump_format": "elf",
        "strings_tools": ("llvm-strings",),
        # llvm-readelf, not host readelf: registry completeness for
        # verify_artifact.import_closure()'s eventual android support --
        # dt_needed.py resolves the identical NDK-relative path itself
        # rather than reading this key (impl-18, WI-22 ask). android's
        # host is x86_64 but the .so is cross-compiled aarch64; every other
        # android tool key in this file (nm_tools, strings_tools) already
        # names the NDK's own binary for the same reason.
        "readelf_tools": ("llvm-readelf",),
        "nm_tools": ("llvm-nm",),
        "nm_tools_fallback": (),
        "objdump_tools": (),
        "dumpbin_tools": (),
        "readobj_tools": (),
        "pe_machine_by_arch": None,
        "c_compiler": None,
        "probe_link_style": None,
        "requires_arch": False,
        "arch_tags": ("arm64-v8a",),
        "arch_legs": None,
        "expected_companions": (),
        "declaration_platform": "android",
        "min_runtime_source": "declaration",
        "ndk_version": "r27c",
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
    "readobj_tools",
    "pe_machine_by_arch",
    "c_compiler",
    "probe_link_style",
    "requires_arch",
    "arch_tags",
    "arch_legs",
    "expected_companions",
    "declaration_platform",
    "min_runtime_source",
    "ndk_version",
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
        # arch_tags and requires_arch state ONE fact ("is this leg per-arch");
        # this is what stops them drifting into two descriptions of it, which
        # is the exact shape Phase 2 exists to delete. A leg with two or more
        # arch tags is per-arch and its calls carry --arch; a single-tag leg
        # is not, and passing --arch there is an argparse error (C-G9).
        tags = entry["arch_tags"]
        if not isinstance(tags, tuple) or not tags:
            raise ValueError(
                f"platform {name!r}: arch_tags must be a non-empty tuple, got {tags!r}"
            )
        legs = entry["arch_legs"]
        if legs is not None and sorted(legs) != sorted(tags):
            raise ValueError(
                f"platform {name!r}: arch_legs keys {sorted(legs)} != arch_tags {sorted(tags)}"
            )
        if entry["requires_arch"] != (len(tags) > 1):
            raise ValueError(
                f"platform {name!r}: requires_arch={entry['requires_arch']} "
                f"contradicts arch_tags={tags!r} (per-arch iff more than one tag)"
            )


_validate()
