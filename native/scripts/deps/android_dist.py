"""Post-install step for android dists: vendor licences, then assert the
artefacts mechanically with the NDK's own ELF instruments (A-T2).

Why this exists as its own module rather than inside ``execute.py``: the
generic component build is platform-neutral, and the android assertions need
an NDK path plus a per-component expectation table. Keeping them here means
``build_deps.py`` has one android call site, and the expectation table is a
single reviewable block of DATA rather than code spread across branches.

Every check follows the capture-then-grep discipline (see
``assertions.py``'s ELF section): the tool's output is written to
``<dist>/.assertions/`` first and searched in Python afterwards, so a failed
assertion always leaves on disk exactly the bytes it judged, and no pipeline
(and therefore no SIGPIPE-under-pipefail inversion) can exist.

A-T3/A-T4 (libjxl / heif stack on android) extend EXPECTATIONS with their own
row rather than adding another code path.
"""
from __future__ import annotations

from pathlib import Path
from typing import Any

from . import assertions as assertions_mod
from . import fetch_libjxl as fetch_libjxl_mod

# Per-component android dist expectations.
#   archives: paths under <dist> that must exist; each maps to the symbols
#             that must appear in that archive's llvm-nm dump. The symbol is
#             chosen to stand for a CAPABILITY (encode, mux writing), not for
#             a file being non-empty.
#   headers:  public headers a consumer compiles against.
#   licence_dir: must exist and hold at least one file (A-LICENCE flavour).
#   machine_probe: the archive whose ELF header is checked for the target
#             machine; one is enough because they come out of one toolchain
#             invocation, and this check exists to catch a HOST build
#             mislabelled as android, which would mislabel all of them.
EXPECTATIONS: dict[str, dict[str, Any]] = {
    "libwebp": {
        "archives": {
            # WebPEncodeRGBA: the encoder is present, not just the decoder --
            # a decode-only libwebp still produces a plausible libwebp.a.
            "lib/libwebp.a": ["WebPEncodeRGBA", "WebPDecodeRGBA"],
            # SharpYuvConvert lives here in 1.6; its absence surfaces as an
            # undefined symbol at the consumer's link, far from the cause.
            "lib/libsharpyuv.a": ["SharpYuvConvert"],
            # The mux WRITER (EXIF/XMP/ICC embedding) and the demux READER.
            "lib/libwebpmux.a": ["WebPMuxCreateInternal"],
            "lib/libwebpdemux.a": ["WebPDemuxInternal"],
        },
        "headers": [
            "include/webp/encode.h",
            "include/webp/decode.h",
            "include/webp/mux.h",
            "include/webp/demux.h",
        ],
        "licence_dir": "share/licenses/libwebp",
        "machine_probe": "lib/libwebp.a",
    },
    # A-T3. Symbol choice note: the plan's own Task 3 constraint warns that
    # JXL_STATIC_DEFINE makes JXL_EXPORT expand to nothing, so a symbol-table
    # check "can never appear ... regardless of correctness" -- that claim is
    # about the DYNAMIC symbol table of a linked .so. Verified directly
    # against the desktop libjxl-dist precedent (the same static .a produced
    # by the same upstream build): `nm -g libjxl.a` DOES list
    # JxlEncoderProcessOutput/JxlDecoderProcessInput/JxlEncoderAddBox as
    # externally-defined ("T") -- static-archive member object files are not
    # subject to the shared-library export-table hiding, only the final
    # linked .so is. These three are exactly REQUIRED_SYMBOLS in
    # deps/fetch_libjxl.py (the desktop carrier this dist's pin is copied
    # from) -- imported, not re-picked, so the two platforms can never assert
    # a different capability set for the same pinned source. JxlGetDefaultCms
    # in libjxl_cms.a is the plan's own link-order trap made mechanical
    # (undefined in libjxl.a, defined only in libjxl_cms.a -- a consumer
    # linking libjxl.a without libjxl_cms.a fails at final link, not here).
    # The remaining four archives (threads/hwy/brotli*) get a presence-only
    # check (empty symbol list): they are dependencies libjxl.a needs at
    # final link, not capability surfaces in their own right.
    "libjxl": {
        "archives": {
            "lib/libjxl.a": list(fetch_libjxl_mod.REQUIRED_SYMBOLS),
            "lib/libjxl_cms.a": ["JxlGetDefaultCms"],
            "lib/libjxl_threads.a": [],
            "lib/libhwy.a": [],
            "lib/libbrotlicommon.a": [],
            "lib/libbrotlidec.a": [],
            "lib/libbrotlienc.a": [],
        },
        "headers": [
            "include/jxl/encode.h",
            "include/jxl/decode.h",
        ],
        # NOTE: the plan calls for share/licenses/{libjxl,highway,brotli,skcms}
        # (four dirs -- highway/brotli/skcms are vendored submodules under
        # third_party/, not the top-level source). The generic
        # android_dist.vendor_licences() call site in build_deps.py only
        # copies ONE component's own licence globs into ONE directory
        # (share/licenses/<component>), so only share/licenses/libjxl is
        # populated by the current pipeline -- the submodule licences are a
        # known gap, flagged rather than silently left unshipped (raised to
        # the team lead alongside this row).
        "licence_dir": "share/licenses/libjxl",
        "machine_probe": "lib/libjxl.a",
    },
}

# ELF machine name llvm-readelf prints for arm64-v8a.
ANDROID_MACHINE = {"arm64-v8a": "AArch64"}


class AndroidDistError(RuntimeError):
    """Raised when the android dist is not shaped the way the manifest and
    the plan say it must be."""


def vendor_licences(loaded: dict, component: str, dist: Path, stage: Path) -> list:
    """Copy the component's licence files into ``<dist>/share/licenses/<component>/``.

    Runs BEFORE any stage cleanup, and reuses ``heif.copy_licence_files`` so
    the pattern semantics (``licence_files`` globs, maxdepth 1) are defined in
    exactly one place rather than reimplemented per platform.
    """
    from . import heif as heif_mod  # local import: heif.py is heavy

    comp = loaded["manifest"].get("component", {}).get(component, {})
    patterns = comp.get("licence_files", [])
    src_dir = _find_source_dir(component, stage)
    dest = Path(dist) / "share" / "licenses" / component
    dest.mkdir(parents=True, exist_ok=True)  # copy_licence_files does not create it
    copied = heif_mod.copy_licence_files(src_dir, dest, patterns)
    if not copied:
        raise AndroidDistError(
            f"no licence file matched {patterns} in {src_dir} -- refusing to ship "
            f"a dist with no licence text (the dist is redistributed)"
        )
    return copied


def _find_source_dir(component: str, stage: Path) -> Path:
    """Locate the extracted/cloned source tree inside ``stage``.

    ``execute.acquire()`` names it ``<component>-<version>`` for tarballs and
    ``<component>`` for clones, so both shapes are accepted rather than
    hard-coding one and failing confusingly on the other.
    """
    stage = Path(stage)
    candidates = sorted(
        p for p in stage.iterdir()
        if p.is_dir() and (p.name == component or p.name.startswith(f"{component}-"))
    ) if stage.is_dir() else []
    if not candidates:
        raise AndroidDistError(
            f"no source directory for {component!r} under {stage} -- cannot vendor "
            f"its licence files"
        )
    return candidates[0]


def assert_dist(
    component: str,
    dist: Path,
    ndk: Path | str,
    arch: str,
    evidence_dir: Path | str | None = None,
) -> Path:
    """Run every android dist assertion for ``component``. Returns the
    directory the captured tool output was written to (the evidence).

    ``evidence_dir`` defaults to ``<dist>/.assertions``; callers building a
    dist that will be COMMITTED pass a path outside the shipped tree (build
    scratch) so the evidence does not become part of the artefact.
    """
    spec = EXPECTATIONS.get(component)
    if spec is None:
        raise AndroidDistError(
            f"no android dist expectations declared for {component!r} -- add a row to "
            f"deps/android_dist.py EXPECTATIONS rather than shipping an unasserted dist"
        )
    machine = ANDROID_MACHINE.get(arch)
    if machine is None:
        raise AndroidDistError(f"no ELF machine name known for arch {arch!r}")

    dist = Path(dist)
    evidence = Path(evidence_dir) if evidence_dir is not None else dist / ".assertions"
    nm = assertions_mod.ndk_tool(ndk, "llvm-nm")
    readelf = assertions_mod.ndk_tool(ndk, "llvm-readelf")

    for rel, symbols in spec["archives"].items():
        archive = dist / rel
        if not archive.is_file():
            raise AndroidDistError(f"required archive missing from the dist: {archive}")
        dump = assertions_mod.capture_tool_output(
            [nm, archive], evidence / f"{Path(rel).name}.nm.txt"
        )
        assertions_mod.assert_symbols_present(dump, symbols, label=f"A-SYMS[{rel}]")

    header_dump = assertions_mod.capture_tool_output(
        [readelf, "-h", dist / spec["machine_probe"]],
        evidence / "machine.readelf.txt",
    )
    assertions_mod.assert_elf_machine(header_dump, machine, label="A-ARCH")

    for rel in spec["headers"]:
        if not (dist / rel).is_file():
            raise AndroidDistError(f"required public header missing from the dist: {dist / rel}")

    assertions_mod.assert_dir_non_empty(dist / spec["licence_dir"], label="A-LICENCE")
    return evidence
