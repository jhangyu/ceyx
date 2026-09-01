"""Unix (macOS + Linux) HEIF distribution assembly, ported from
``native/scripts/fetch_heif_deps.sh``.

Produces an ENCODE-ENABLED, dynamically linked (libheif/libde265)
distribution under ``native/third_party/heif-dist[-<arch>]/``. Idempotent.

Every fact this module needs -- versions, URLs, SHA-256 pins, cmake options,
licence globs, output artefact names -- is read from
``native/deps/manifest.toml`` rather than restated here. That is the point of
the migration: the shell script and the manifest used to be two copies of the
same data, and a divergence between them was invisible until a build shipped
the wrong thing.

WHAT IS PRESERVED VERBATIM FROM THE SHELL SCRIPT (do not "simplify"):

* **The stamp format.** ``${DIST}/.pins`` still contains exactly
  ``libheif=<v>:<sha> libde265=<v>:vcpkg kvazaar=<v>:<sha> aom=<v>:vcpkg
  arch=<a>``. A dist produced by the old script is therefore still recognised
  as current by this one and vice versa. The arch is part of the stamp
  because an arm64 and an x86_64 dist are not interchangeable; kvazaar's and
  aom's versions are part of it because a pre-expansion decode-only dist
  would otherwise match on libheif/libde265 alone, report "already at the
  pinned versions" and let the encoders silently never appear -- the
  highest-probability silent failure in the original. The literal ``vcpkg``
  token invalidates every pre-D5 dist, which is what forces the copy path to
  actually run once instead of being skipped by a coincidentally-matching
  stamp.

* **Every assertion, and its fail-loud semantics.** A libheif built without a
  working codec configures and installs perfectly happily and then encodes or
  decodes nothing for that codec, so each capability is PROVEN against the
  artefact's own symbol table, never inferred from the flags we passed.
  ``WITH_AOM_ENCODER`` and ``WITH_AOM_DECODER`` are independent flags, hence
  two independent aom assertions (``aom_codec_av1_cx`` / ``aom_codec_av1_dx``)
  rather than one. A missing instrument (``readelf`` on Linux) is a FAILURE,
  not a skip: a check that passes when its instrument is absent is worse than
  no check.

* **Capture-then-match.** The shell script had to capture ``nm``/``otool``
  output into a variable and match it with a here-string, because
  ``nm ... | grep -q ...`` under ``set -o pipefail`` INVERTS the result (grep
  exits at its first match, ``nm`` dies of SIGPIPE, the pipeline reports 141;
  the check then fails precisely BECAUSE the symbol is present). In Python
  the inversion cannot occur at all -- :mod:`deps.run` refuses any argv
  element containing a pipe and returns ``CompletedProcess.returncode`` --
  but the structure is kept identical so the two implementations read the
  same way.

WHAT DELIBERATELY DIFFERS:

* Parallelism comes from ``os.cpu_count()`` instead of a
  ``sysctl``/``nproc`` branch, and extraction from stdlib ``tarfile``
  instead of ``tar`` -- see :mod:`deps.execute`.

Windows is NOT handled here: ``build_heif_dist_windows.sh`` keeps building
libde265 from source for the three durable blockers recorded in
``native/deps/manifest.toml [component.libde265.source.windows]``.
"""
from __future__ import annotations

import os
import platform as platform_module
import shutil
from pathlib import Path
from typing import Any, Optional

try:  # pragma: no cover - import style depends on how the caller invokes us
    from . import execute as execute_mod
    from . import fetch as fetch_mod
    from .run import run
except ImportError:  # pragma: no cover - fallback for direct script execution
    import execute as execute_mod  # type: ignore[no-redef]
    import fetch as fetch_mod  # type: ignore[no-redef]
    from run import run  # type: ignore[no-redef]


COMPONENT = "heif-stack"
STAGES = ("all", "libde265", "kvazaar", "aom", "libheif", "assemble")

# Platform-dependent artefact spellings and tools. The shell script derived
# these from `uname -s`; here they are a table keyed by the caller's already
# resolved platform, so a macOS developer can read the Linux column without
# running Linux.
_PLATFORM_TRAITS = {
    "macos": {
        "heif_lib": "lib/libheif.1.dylib",
        "de265_lib": "lib/libde265.0.dylib",
        "de265_versioned_suffix": "0.dylib",
        "de265_unversioned_suffix": "dylib",
        "nm_flags": ["-gU"],
        "deps_argv": ["otool", "-L"],
    },
    "linux": {
        "heif_lib": "lib/libheif.so.1",
        "de265_lib": "lib/libde265.so.0",
        "de265_versioned_suffix": "so.0",
        "de265_unversioned_suffix": "so",
        "nm_flags": ["-D"],
        "deps_argv": ["ldd"],
    },
    # android (A-T4) is an NDK CROSS-COMPILE, and it differs from linux in two
    # ways that are not cosmetic:
    #
    # 1. UNVERSIONED SONAMEs. The NDK toolchain file sets
    #    CMAKE_PLATFORM_NO_VERSIONED_SONAME, because Android's packaging only
    #    ships files whose name ends in ".so" -- "libheif.so.1" is not packed
    #    into an APK at all, and a versioned SONAME therefore becomes a
    #    load failure on a device rather than on the build machine. The
    #    artefact names below are consequently the plain ".so" spellings, and
    #    assemble() PROVES the recorded SONAME matches (a toolchain that
    #    silently produced a versioned name must fail here, loudly, naming the
    #    variable).
    # 2. NDK-SUPPLIED INSTRUMENTS. Host `nm`/`readelf` may not exist (macOS)
    #    or may not be the toolchain that produced the binary; the checks use
    #    the NDK's own llvm-nm / llvm-readelf, resolved from the --android-ndk
    #    root passed explicitly (never from the environment).
    "android": {
        "heif_lib": "lib/libheif.so",
        "de265_lib": "lib/libde265.so",
        "de265_versioned_suffix": "so",
        "de265_unversioned_suffix": "so",
        "nm_flags": ["-D"],
        "nm_tool": "llvm-nm",
        "deps_argv": ["llvm-readelf", "-d"],
        "cross": True,
    },
}

_KVAZAAR_LIB = "lib/libkvazaar.a"
_AOM_LIB = "lib/libaom.a"

# Symbol assertions, ported one-for-one from fetch_heif_deps.sh's assemble().
# (symbol, human explanation of what its absence means).
_REQUIRED_HEIF_SYMBOLS = (
    ("heif_decode_image", "libheif exports no heif_decode_image"),
    (
        "heif_context_get_encoder_for_format",
        "libheif exports no heif_context_get_encoder_for_format -- the dist was "
        "built decode-only; HEIC/AVIF encode cannot work",
    ),
    (
        "kvz_api_get",
        "no kvazaar symbols in libheif -- WITH_KVAZAAR did not take effect. "
        "libheif configures and installs happily without an HEVC encoder and "
        "then encodes nothing",
    ),
    ("aom_codec_av1_cx", "no aom ENCODER symbols in libheif (AVIF encode dead)"),
    ("aom_codec_av1_dx", "no aom DECODER symbols in libheif (AVIF decode dead)"),
)

# Absence assertions: a symbol whose PRESENCE is the failure.
_FORBIDDEN_HEIF_SYMBOLS = (
    ("x265_encoder", "x265 encoder symbols present (GPL-2.0 contamination)"),
)


class HeifError(RuntimeError):
    """Raised when any stage of the HEIF dist assembly fails. Message always
    names the artefact and the concrete missing fact, in the same shape the
    shell script's ``[heif] FAILED:`` lines used."""


def _fail(message: str) -> "HeifError":
    return HeifError(f"[heif] FAILED: {message}")


def _log(message: str) -> None:
    print(f"[heif] {message}")


def traits(platform: str) -> dict[str, Any]:
    if platform not in _PLATFORM_TRAITS:
        raise HeifError(
            f"[heif] the HEIF stack assembly covers macOS, Linux and Android "
            f"(NDK cross-compile); {platform!r} is built by "
            f"deps/win_heif_dist.py (see manifest.toml "
            f"[component.libde265.source.windows])"
        )
    return _PLATFORM_TRAITS[platform]


def ndk_tool(ndk: Optional[str], tool: str) -> str:
    """Absolute path to one of the NDK's own LLVM binutils.

    The host-tag directory (darwin-x86_64 / linux-x86_64 / ...) is DISCOVERED
    rather than assumed: r27c ships a single prebuilt per host and the name
    differs by host, and hard-coding one spelling would make every assertion
    unrunnable on the other host -- an assertion that cannot run is worse than
    no assertion, because its absence reads as a pass.
    """
    if not ndk:
        raise _fail(
            f"the android HEIF dist needs the NDK to resolve {tool}; "
            f"pass --android-ndk <path> (no host is Android, so this is always "
            f"an explicit cross-compile argument, never an environment lookup)"
        )
    root = Path(ndk) / "toolchains" / "llvm" / "prebuilt"
    candidates = sorted(p for p in root.glob("*") if (p / "bin" / tool).is_file()) if root.is_dir() else []
    if not candidates:
        raise _fail(
            f"{tool} not found under {root}/*/bin -- is {ndk} an Android NDK root?\n"
            f"  A missing instrument is a FAILURE, not a skip: a capability check "
            f"that passes when its tool is absent is worse than no check."
        )
    return str(candidates[0] / "bin" / tool)


def ndk_revision(ndk: Optional[str]) -> str:
    """The NDK's own recorded ``Pkg.Revision`` (e.g. ``27.2.12479018``).

    Part of the android ``.pins`` stamp: two dists built from identical
    component pins by different NDKs are NOT interchangeable (different libc
    symbol sets, different default page alignment), and a stamp that cannot
    tell them apart reports a stale dist as current -- the highest-probability
    silent failure this stamp exists to prevent, in its android form.
    """
    if not ndk:
        raise _fail("android pin stamp needs the NDK root (--android-ndk) to read its revision")
    properties = Path(ndk) / "source.properties"
    if not properties.is_file():
        raise _fail(f"{properties} missing -- cannot record the NDK revision in the pin stamp")
    for line in properties.read_text(encoding="utf-8").splitlines():
        if line.strip().startswith("Pkg.Revision"):
            return line.split("=", 1)[1].strip()
    raise _fail(f"{properties} declares no Pkg.Revision line")


def host_arch() -> str:
    machine = platform_module.machine().lower()
    return "arm64" if machine in ("arm64", "aarch64") else machine


def default_dist(native_dir: Path, arch: str, platform: str = "linux") -> Path:
    """The host-architecture dist keeps its historical path so an ordinary
    local build is unaffected; a cross-architecture dist goes to a SUFFIXED
    directory instead of overwriting it. This tree is shared, and silently
    replacing the arm64 dylibs with x86_64 ones would break the next link
    with an error pointing at the decoder rather than at this script.

    Android is never the host, so it always takes the suffixed, ABI-named
    committed-dist path (``heif-dist-android-arm64-v8a``) -- the spelling
    publish_release.py parses out of an asset name."""
    if platform == "android":
        return Path(native_dir) / "third_party" / f"heif-dist-android-{arch}"
    if arch == host_arch():
        return Path(native_dir) / "third_party" / "heif-dist"
    return Path(native_dir) / "third_party" / f"heif-dist-{arch}"


def _component(loaded: dict[str, Any], name: str) -> dict[str, Any]:
    comp = loaded["manifest"].get("component", {}).get(name)
    if comp is None:
        raise HeifError(f"[heif] manifest has no component.{name}")
    return comp


def android_pin_string(loaded: dict[str, Any], arch: str, ndk: Optional[str]) -> str:
    """The android ``.pins`` stamp. Names EVERY component plus the ABI and the
    NDK revision.

    This is the round's highest-risk defect made mechanical (plan Step 4.3): a
    stamp that omits a component reports a pre-existing decode-only dist as
    "already at the pinned versions", the encoder stages never run, and the
    dist ships without them while every log line says it is current. Each
    token below is read from the manifest's ANDROID source blocks, not from
    the desktop ones, because android acquires libde265 and aom from tarballs
    where macOS/Linux take them from vcpkg -- a stamp quoting the desktop
    provenance would describe bytes this dist does not contain.
    """
    tokens = []
    for name in ("libheif", "libde265", "kvazaar", "aom"):
        block = execute_mod.resolve_source(loaded, name, "android")
        version = str(_component(loaded, name)["version"])
        if block.get("kind") == "git":
            pin = "git:" + str(block["tag"]).replace("{version}", version)
        else:
            pin = str(block["sha256"])
        tokens.append(f"{name}={version}:{pin}")
    tokens.append(f"arch={arch}")
    tokens.append(f"abi={arch}")
    tokens.append(f"ndk={ndk_revision(ndk)}")
    return " ".join(tokens)


def want_pins(loaded: dict[str, Any], arch: str) -> str:
    """Reproduce the shell script's ``WANT_PINS`` string EXACTLY, from the
    manifest rather than from restated constants. Byte-compatible with a
    ``.pins`` file written by fetch_heif_deps.sh."""
    heif = _component(loaded, "libheif")
    de265 = _component(loaded, "libde265")
    kvazaar = _component(loaded, "kvazaar")
    aom = _component(loaded, "aom")
    heif_sha = str(heif["source"]["default"]["sha256"])
    kvz_sha = str(kvazaar["source"]["default"]["sha256"])
    return (
        f"libheif={heif['version']}:{heif_sha} "
        f"libde265={de265['version']}:vcpkg "
        f"kvazaar={kvazaar['version']}:{kvz_sha} "
        f"aom={aom['version']}:vcpkg "
        f"arch={arch}"
    )


def pin_string(
    loaded: dict[str, Any], platform: str, arch: str, ndk: Optional[str] = None
) -> str:
    """Platform-dispatched pin stamp: android names every component + ABI +
    NDK revision; macOS/Linux keep the byte-compatible legacy string."""
    if platform == "android":
        return android_pin_string(loaded, arch, ndk)
    return want_pins(loaded, arch)


def stamp_is_current(
    dist: Path,
    loaded: dict[str, Any],
    arch: str,
    platform: str,
    ndk: Optional[str] = None,
) -> bool:
    """True when ``.pins`` matches AND both dynamic artefacts are on disk.
    Both halves matter: a stamp alone can outlive a deleted dylib."""
    t = traits(platform)
    stamp = Path(dist) / ".pins"
    if not stamp.is_file():
        return False
    if stamp.read_text(encoding="utf-8") != pin_string(loaded, platform, arch, ndk):
        return False
    return (Path(dist) / t["heif_lib"]).is_file() and (Path(dist) / t["de265_lib"]).is_file()


def _vcpkg_prefix(what: str, feature_hint: str) -> Path:
    """Resolve ``CEYX_VCPKG_PREFIX``, hard-failing when unset.

    There is deliberately no fallback to a source build: a silent fallback
    would hide exactly the wiring the vcpkg migration exists to prove.
    """
    prefix = os.environ.get("CEYX_VCPKG_PREFIX", "")
    if not prefix or not Path(prefix).is_dir():
        raise _fail(
            f"{what} comes from vcpkg since D5, but CEYX_VCPKG_PREFIX is unset or "
            f"not a directory (value: {prefix!r}).\n"
            f"  Install it first, e.g.:\n"
            f"    <vcpkg>/vcpkg install --x-manifest-root=native/vcpkg \\\n"
            f"        --x-install-root=<root> --triplet=<triplet> \\\n"
            f"        --x-no-default-features {feature_hint}\n"
            f"  then export CEYX_VCPKG_PREFIX=<root>/<triplet>.\n"
            f"  There is deliberately no fallback to a source build: a silent "
            f"fallback would hide exactly the wiring this change exists to prove."
        )
    return Path(prefix)


def _listing(directory: Path) -> str:
    try:
        return "\n".join(sorted(p.name for p in Path(directory).iterdir()))
    except OSError as exc:
        return f"<cannot list {directory}: {exc}>"


# --------------------------------------------------------------------------
# Stage: libde265 (copied out of the vcpkg prefix, NOT built here)
# --------------------------------------------------------------------------
def build_libde265_android(
    loaded: dict[str, Any],
    arch: str,
    dist: Path,
    stage: Path,
    ndk: Optional[str],
) -> None:
    """Android has no vcpkg leg (manifest [component.libde265.source.android]),
    so libde265 is BUILT here from the same upstream 1.1.1 release tarball the
    overlay port pins -- still SHARED, which is an LGPL-3 §4(d)(1) obligation
    rather than a packaging preference.

    The SONAME is then PROVEN against the artefact, not assumed from the flags
    we passed: libheif records its runtime dependency from libde265's SONAME,
    and Android's packaging drops any file not ending in ".so", so a versioned
    SONAME here is a device-only load failure that no build machine reproduces.
    """
    dist = Path(dist)
    target = dist / _PLATFORM_TRAITS["android"]["de265_lib"]
    if target.is_file():
        _log(f"libde265 already installed at {target}, skipping")
        return
    execute_mod.build_component(
        loaded, "libde265", "android", arch, dist, stage, ndk=ndk
    )
    if not target.is_file():
        produced = sorted(p.name for p in (dist / "lib").glob("libde265.so*"))
        raise _fail(
            f"{target} missing after install (found: {produced or 'nothing'}).\n"
            f"  Android packaging only ships files whose name ends in '.so', so a "
            f"VERSIONED spelling here would never reach a device. The NDK toolchain "
            f"file normally guarantees the plain name via "
            f"CMAKE_PLATFORM_NO_VERSIONED_SONAME; if this fired, that guarantee did "
            f"not hold and the dist must not be shipped as-is."
        )
    _assert_android_soname(target, "libde265.so", ndk)
    _log(f"libde265 {_component(loaded, 'libde265')['version']} built for android into {dist}")


def _assert_android_soname(library: Path, expected: str, ndk: Optional[str]) -> None:
    """Prove the ELF's recorded SONAME is exactly ``expected``.

    Two distinct failures are covered: a versioned spelling (invisible on the
    build machine, fatal inside an APK) and an absolute host path baked into
    the name (the dist would only load on the machine that produced it).
    Capture-then-match, never ``readelf | grep -q`` (2026-08-28).
    """
    dyn = run([ndk_tool(ndk, "llvm-readelf"), "-d", str(library)]).stdout
    soname_lines = [line for line in dyn.splitlines() if "SONAME" in line]
    if not soname_lines:
        raise _fail(f"{library.name} records no SONAME at all\n{dyn}")
    if not any(f"[{expected}]" in line for line in soname_lines):
        raise _fail(
            f"{library.name} SONAME is not {expected!r}: {soname_lines}\n"
            f"  Android's loader resolves DT_NEEDED against lib/<abi>/ inside the "
            f"APK by exact file name, and a name carrying a version suffix or a "
            f"host path cannot be satisfied there."
        )
    if "/" in "".join(soname_lines):
        raise _fail(f"{library.name} SONAME contains a path separator: {soname_lines}")
    _log(f"ASSERT ok      {library.name} SONAME is {expected}")


def build_libde265(loaded: dict[str, Any], platform: str, dist: Path) -> None:
    """Copy vcpkg's libde265 into ``dist`` and PROVE its recorded install
    name / SONAME.

    Why a copy rather than pointing libheif at the vcpkg prefix directly: the
    shipped product loads libde265 at runtime from BESIDE the decoder dylib
    (cmake/heif.cmake stages ``lib/libde265.0.dylib`` next to
    dng_decoder_native and build_apps.py copies every sibling dylib into the
    app bundle). Leaving the dylib in the vcpkg prefix and only re-aiming the
    LIBDE265_* hints would produce a build that links on the build machine
    and fails to load anywhere else -- a failure invisible on CI.

    The install name is the load-bearing part: libheif records the DEPENDENCY
    name from this field, not from the file name it linked against. Round-3
    failure trace: a verbatim copy of vcpkg's ``libde265.0.2.1.dylib``
    records an install name that does not match the staged
    ``libde265.0.dylib`` spelling, and the runtime load failure is invisible
    on build machines. Hence rename, rewrite, then RE-READ -- setting a value
    and the artefact carrying it are different facts.
    """
    t = traits(platform)
    dist = Path(dist)
    target = dist / t["de265_lib"]
    if target.is_file():
        _log(f"libde265 already installed at {target}, skipping")
        return

    prefix = _vcpkg_prefix("libde265", "--x-feature=de265")
    versioned = f"libde265.{t['de265_versioned_suffix']}"
    unversioned = f"libde265.{t['de265_unversioned_suffix']}"

    # Accept either spelling in the prefix: CMake's SOVERSION handling
    # normally produces a versioned real file plus an unversioned symlink,
    # but which of the two is the real file is a property of the port's
    # build, not a contract.
    src = next(
        (c for c in (prefix / "lib" / versioned, prefix / "lib" / unversioned) if c.is_file()),
        None,
    )
    if src is None:
        raise _fail(
            f"no libde265 shared library under {prefix / 'lib'}\n"
            f"  looked for {versioned} and {unversioned}\n"
            f"  contents:\n{_listing(prefix / 'lib')}\n"
            f"  A STATIC libde265 here means the overlay triplet's dynamic "
            f"exception did not apply -- that is an LGPL-3 4(d)(1) breach, not a "
            f"packaging detail."
        )
    _log(f"using vcpkg-supplied libde265: {src}")

    (dist / "lib").mkdir(parents=True, exist_ok=True)
    (dist / "include").mkdir(parents=True, exist_ok=True)
    # shutil.copy follows symlinks (the shell script's `cp -L`): whichever
    # spelling was found becomes the REAL file at the versioned name, and the
    # unversioned name becomes the symlink pointing at it. That is the shape
    # everything downstream assumes -- libheif is pointed at the unversioned
    # name while heif.cmake stages the versioned one.
    shutil.copy(src, target)
    target.chmod(target.stat().st_mode | 0o200)
    link = dist / "lib" / unversioned
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(versioned)

    include_dst = dist / "include" / "libde265"
    if include_dst.exists():
        shutil.rmtree(include_dst)
    shutil.copytree(prefix / "include" / "libde265", include_dst)
    if not (include_dst / "de265.h").is_file():
        raise _fail(f"{include_dst / 'de265.h'} missing after the copy")

    _assert_de265_recorded_name(platform, target, versioned)
    _log(f"libde265 {_component(loaded, 'libde265')['version']} installed into {dist} from the vcpkg prefix")


def _assert_de265_recorded_name(platform: str, target: Path, versioned: str) -> None:
    want = f"@rpath/{versioned}"
    if platform == "macos":
        run(["install_name_tool", "-id", want, str(target)])
        id_out = run(["otool", "-D", str(target)]).stdout
        if want not in id_out.splitlines():
            raise _fail(f"libde265 install name is not {want}\n{id_out}")
        return

    # Linux: no equivalent rewrite exists without patchelf, so assert instead
    # -- the vcpkg build must already carry the right SONAME. A missing
    # readelf is a FAILURE, not a skip.
    if shutil.which("readelf") is None:
        raise _fail(
            "readelf not found; cannot prove libde265's SONAME.\n"
            "  Install binutils (the SONAME is what libheif records as its runtime "
            "dependency; an unverified one is a load failure on the user's machine, "
            "not on this one)."
        )
    dyn_out = run(["readelf", "-d", str(target)]).stdout
    if not any("SONAME" in line and versioned in line for line in dyn_out.splitlines()):
        raise _fail(f"libde265 SONAME is not {versioned}\n{dyn_out}")


# --------------------------------------------------------------------------
# Stage: kvazaar (built from source, static, linked INTO libheif)
# --------------------------------------------------------------------------
def build_kvazaar(
    loaded: dict[str, Any],
    platform: str,
    arch: str,
    dist: Path,
    stage: Path,
    ndk: Optional[str] = None,
) -> None:
    """kvazaar and aom are built/linked STATIC into libheif because
    ``ENABLE_PLUGIN_LOADING=OFF`` -- a dlopen-ed plugin directory does not
    survive app-bundle packaging. ``CMAKE_POSITION_INDEPENDENT_CODE=ON``
    (in the manifest's platform overlays) is not optional: without PIC the
    link fails on Linux with a relocation error that names libheif rather
    than the archive."""
    target = Path(dist) / _KVAZAAR_LIB
    if target.is_file():
        _log(f"kvazaar already installed at {target}, skipping")
        return
    execute_mod.build_component(loaded, "kvazaar", platform, arch, dist, stage, ndk=ndk)


# --------------------------------------------------------------------------
# Stage: aom (copied out of the vcpkg prefix, NOT built here)
# --------------------------------------------------------------------------
def build_aom_android(
    loaded: dict[str, Any],
    arch: str,
    dist: Path,
    stage: Path,
    ndk: Optional[str],
) -> None:
    """Android has no vcpkg leg, so aom is built from its upstream release
    tarball (manifest [component.aom.source.android]) as a STATIC archive
    linked into libheif.

    ``CMAKE_POSITION_INDEPENDENT_CODE=ON`` (android overlay) is mandatory and
    not stylistic: a non-PIC archive linked into libheif.so fails with a
    relocation error that names LIBHEIF rather than this archive, sending the
    reader to the wrong file.
    """
    dist = Path(dist)
    target = dist / _AOM_LIB
    if target.is_file():
        _log(f"aom already installed at {target}, skipping")
        return
    execute_mod.build_component(loaded, "aom", "android", arch, dist, stage, ndk=ndk)
    for header in ("aom_encoder.h", "aom_decoder.h"):
        installed = dist / "include" / "aom" / header
        if not installed.is_file():
            raise _fail(
                f"aom headers incomplete: {installed} missing (libheif's encoder and "
                f"decoder plugins include them separately, and its FindAOM probe is "
                f"NON-FATAL -- a missing header here becomes a silently dropped "
                f"capability, not a build error)"
            )
    _log(f"aom {_component(loaded, 'aom')['version']} built for android into {dist}")


def build_aom(loaded: dict[str, Any], dist: Path) -> None:
    """Same shape and same reasoning as :func:`build_libde265`: ``dist``
    stays the one place everything downstream reads, so libheif's
    AOM_INCLUDE_DIR/AOM_LIBRARY pre-seeding is unchanged.

    ``CONFIG_AV1_ENCODER``/``CONFIG_AV1_DECODER`` are not passed to the port
    (it leaves both at aom's default ON). That is exactly why assemble()'s
    two independent symbol assertions are load-bearing rather than
    belt-and-braces: they are the only thing proving both halves survived
    the version bump, and they check libheif's symbol table, not our intent.
    """
    dist = Path(dist)
    target = dist / _AOM_LIB
    if target.is_file():
        _log(f"aom already installed at {target}, skipping")
        return

    prefix = _vcpkg_prefix("aom", "--x-feature=de265 --x-feature=aom")
    src = prefix / "lib" / "libaom.a"
    if not src.is_file():
        raise _fail(
            f"{src} missing\n"
            f"  contents:\n{_listing(prefix / 'lib')}\n"
            f"  aom must be a STATIC archive: it is linked INTO libheif "
            f"(ENABLE_PLUGIN_LOADING=OFF), and a dylib here would have to be "
            f"shipped and resolved at runtime instead."
        )

    (dist / "lib").mkdir(parents=True, exist_ok=True)
    (dist / "include").mkdir(parents=True, exist_ok=True)
    shutil.copy(src, target)
    include_dst = dist / "include" / "aom"
    if include_dst.exists():
        shutil.rmtree(include_dst)
    shutil.copytree(prefix / "include" / "aom", include_dst)
    for header in ("aom_encoder.h", "aom_decoder.h"):
        if not (include_dst / header).is_file():
            raise _fail(
                f"aom headers incomplete under {include_dst}: {header} missing\n"
                f"  (both aom_encoder.h and aom_decoder.h are required -- libheif's "
                f"encoder and decoder plugins include them separately)\n"
                f"  contents:\n{_listing(include_dst)}"
            )
    _log(f"aom {_component(loaded, 'aom')['version']} installed into {dist} from the vcpkg prefix")


# --------------------------------------------------------------------------
# Stage: libheif (built from source against the three above)
# --------------------------------------------------------------------------
def build_libheif(
    loaded: dict[str, Any],
    platform: str,
    arch: str,
    dist: Path,
    stage: Path,
    ndk: Optional[str] = None,
) -> None:
    t = traits(platform)
    dist = Path(dist)
    # libheif's FindLIBDE265/Findkvazaar/FindAOM probes are NON-FATAL: a
    # configure with any of these inputs missing SUCCEEDS and silently drops
    # the capability. Checking they exist on disk BEFORE configuring is
    # therefore the only place the missing input can still be named.
    for dep in (dist / t["de265_lib"], dist / _KVAZAAR_LIB, dist / _AOM_LIB):
        if not dep.is_file():
            raise _fail(f"{dep} missing -- build libde265/kvazaar/aom first")
    execute_mod.build_component(loaded, "libheif", platform, arch, dist, stage, ndk=ndk)


# --------------------------------------------------------------------------
# Stage: assemble (assertions + licences + stamp + cleanup)
# --------------------------------------------------------------------------
def assemble(
    loaded: dict[str, Any],
    platform: str,
    arch: str,
    dist: Path,
    stage: Path,
    ndk: Optional[str] = None,
) -> None:
    t = traits(platform)
    dist = Path(dist)
    stage = Path(stage)
    heif_lib = dist / t["heif_lib"]
    de265_lib = dist / t["de265_lib"]
    for dep in (heif_lib, de265_lib, dist / _KVAZAAR_LIB, dist / _AOM_LIB):
        if not dep.is_file():
            raise _fail(f"{dep} missing -- run libde265/kvazaar/aom/libheif stages first")

    if platform == "android":
        # STRIP FIRST, ASSERT SECOND. The assertions must run against the bytes
        # that ship, not against a richer intermediate: checking an unstripped
        # library and shipping a stripped one measures a binary nobody
        # receives. --strip-debug keeps .symtab/.dynsym/SONAME/DT_NEEDED, so
        # every check below still has its evidence.
        strip_android_artefacts(dist, [heif_lib, de265_lib, dist / _KVAZAAR_LIB, dist / _AOM_LIB], ndk)

    assert_libheif_capabilities(platform, heif_lib, ndk=ndk)
    assert_arch(platform, arch, [heif_lib, de265_lib], ndk=ndk)
    if platform == "android":
        _assert_android_soname(heif_lib, "libheif.so", ndk)
        measure_android_alignment(dist, [heif_lib, de265_lib], ndk)
    vendor_licences(loaded, dist, stage, platform=platform)

    (dist / ".pins").write_text(pin_string(loaded, platform, arch, ndk), encoding="utf-8")
    cleanup_stage(loaded, stage, platform=platform)
    _log(f"dist ready at {dist} (arch {arch})")
    for name in ("libheif", "libde265", "kvazaar", "aom"):
        _log(f"  {name:<8} {_component(loaded, name)['version']}")


def read_symbols(
    platform: str, library: Path, *, ndk: Optional[str] = None, dynamic: bool = True
) -> str:
    """Capture the symbol table as text. See the module docstring on why this
    is a capture-then-match and never a pipeline.

    ``dynamic=False`` asks for the FULL table (``.symtab``) instead of the
    dynamic one. That distinction only matters where the library is built with
    reduced visibility and the symbols under test come from statically linked
    archives -- kvazaar's and aom's do, and they are deliberately not exported,
    so a dynamic-only dump would report them absent and the check would be
    measuring visibility rather than the capability it claims to measure.
    """
    t = traits(platform)
    tool = ndk_tool(ndk, t["nm_tool"]) if t.get("cross") else "nm"
    flags = list(t["nm_flags"]) if dynamic else []
    return run([tool, *flags, str(library)]).stdout


def read_dependencies(platform: str, library: Path, *, ndk: Optional[str] = None) -> str:
    t = traits(platform)
    argv = list(t["deps_argv"])
    if t.get("cross"):
        argv[0] = ndk_tool(ndk, argv[0])
    return run([*argv, str(library)]).stdout


def assert_libheif_capabilities(
    platform: str, heif_lib: Path, *, ndk: Optional[str] = None
) -> None:
    """Proof, not assumption. Every check below reads the ARTEFACT."""
    symbols = read_symbols(platform, heif_lib, ndk=ndk)
    dependencies = read_dependencies(platform, heif_lib, ndk=ndk)
    full_symbols = None
    if traits(platform).get("cross"):
        # WITH_REDUCED_VISIBILITY=ON (manifest cmake.base) keeps the merged
        # kvazaar/aom symbols out of .dynsym; they live in .symtab. Reading
        # both means each check runs against the table where its symbol would
        # actually appear if the capability is present.
        full_symbols = read_symbols(platform, heif_lib, ndk=ndk, dynamic=False)
    check_symbols(symbols, dependencies, full_symbols=full_symbols)


def check_symbols(
    symbols: str, dependencies: str, *, full_symbols: Optional[str] = None
) -> None:
    """Pure function over already-captured text, so the whole assertion set
    is unit-testable without a built dist (and each one demonstrable red).

    Each check PRINTS a line on success as well as failing on absence. That
    is not decoration: a silently-passing gate and a gate that never ran
    produce identical logs (lesson 2026-08-25), and the CI verdict for this
    step is read at the level of individual assertion lines rather than the
    step's conclusion. ``ASSERT ok`` / ``ASSERT absent`` lines are the
    machine-greppable evidence that each one actually executed.
    """
    # Where a full table was captured, a REQUIRED symbol may legitimately live
    # in either table (public API in .dynsym, merged static-archive symbols in
    # .symtab), so presence is checked against their union. The FORBIDDEN
    # check is the opposite: it must see EVERY table, because a symbol hiding
    # in the one we did not read is exactly the contamination it exists to
    # catch.
    haystack = symbols if full_symbols is None else symbols + "\n" + full_symbols
    for symbol, explanation in _REQUIRED_HEIF_SYMBOLS:
        if symbol not in haystack:
            raise _fail(explanation)
        _log(f"ASSERT ok      present in libheif: {symbol}")
    for symbol, explanation in _FORBIDDEN_HEIF_SYMBOLS:
        if symbol in haystack:
            raise _fail(explanation)
        _log(f"ASSERT absent  correctly not in libheif: {symbol}")
    if "libde265" not in dependencies:
        raise _fail(
            "libheif has no libde265 dependency -- it was built WITHOUT an HEVC "
            "decoder and would silently decode nothing."
        )
    _log("ASSERT ok      libheif records a libde265 runtime dependency")


def assert_arch(
    platform: str, arch: str, libraries: list[Path], *, ndk: Optional[str] = None
) -> None:
    """A dist silently built for the wrong architecture links nowhere, and
    the failure surfaces much later as an opaque "building for macOS-x86_64
    but attempting to link file built for macOS-arm64"."""
    for library in libraries:
        if platform == "macos":
            archs = run(["lipo", "-archs", str(library)]).stdout
            if arch not in archs.split():
                raise _fail(f"{library.name} has archs {archs.strip()!r}, wanted {arch!r}")
            _log(f"ASSERT ok      {library.name} archs {archs.strip()!r} include {arch}")
        elif platform == "android":
            # A cross-built dist that silently came out host-arch is the whole
            # failure mode this check exists for, and "64-bit ELF" alone would
            # accept an x86-64 host build. Read the ELF header's machine field
            # from the NDK's own readelf: `file` is a host tool whose wording
            # varies, llvm-readelf's is stable and names the toolchain that
            # produced the binary.
            header = run([ndk_tool(ndk, "llvm-readelf"), "-h", str(library)]).stdout
            if "ELF64" not in header:
                raise _fail(f"{library} is not a 64-bit ELF:\n{header}")
            machine = next(
                (line for line in header.splitlines() if line.strip().startswith("Machine:")),
                "",
            )
            if "AArch64" not in machine:
                raise _fail(
                    f"{library.name} is not an AArch64 object (arch {arch!r} requested): "
                    f"{machine.strip()!r}"
                )
            _log(f"ASSERT ok      {library.name} is a 64-bit AArch64 ELF ({arch})")
        else:
            # Linux has no lipo/universal-binary concept; prove each output is
            # a real 64-bit ELF instead.
            file_out = run(["file", "-L", str(library)]).stdout
            if "ELF 64-bit" not in file_out:
                raise _fail(f"{library} is not a 64-bit ELF: {file_out.strip()}")
            _log(f"ASSERT ok      {library.name} is a 64-bit ELF")


ANDROID_MIN_PAGE_ALIGN = 16384


def strip_android_artefacts(dist: Path, artefacts: list[Path], ndk: Optional[str]) -> list[Path]:
    """``llvm-strip --strip-debug`` every android artefact this dist produces.

    An NDK cross-build embeds full debug info by default -- CMAKE_BUILD_TYPE=
    Release does NOT strip it -- and this dist is COMMITTED. Measured on CI run
    33460559016: libheif.so 90.5 MB and libaom.a 99.55 MB, the latter within
    half a megabyte of GitHub's 100 MB per-file hard limit (the same wall that
    rejected the libjxl android dist push outright at 199 MB).

    ``--strip-debug``, never ``--strip-all``, and the distinction is
    load-bearing here rather than conventional: it removes debug sections only,
    so ``.symtab``, ``.dynsym``, the SONAME and DT_NEEDED all survive and every
    assertion in this module still has the evidence it reads. assemble() calls
    this BEFORE the assertions for exactly that reason -- the checks must run
    on the bytes that ship.

    The generic android path has an equivalent helper
    (:func:`deps.android_dist.strip_archives`), which cannot be reused as-is
    because it resolves its file list from that module's per-component
    EXPECTATIONS table, and the heif stack has no row there. Consolidating the
    two is a recorded follow-up, not something to do while this path is being
    exercised by CI.
    """
    strip = ndk_tool(ndk, "llvm-strip")
    stripped: list[Path] = []
    for artefact in artefacts:
        path = Path(artefact)
        if not path.is_file():
            # Absence is not this function's failure to report: the assertions
            # that follow name every missing artefact precisely.
            continue
        before = path.stat().st_size
        run([strip, "--strip-debug", str(path)])
        after = path.stat().st_size
        _log(f"STRIP          {path.name}: {before} -> {after} bytes")
        stripped.append(path)
    if not stripped:
        raise _fail(f"nothing to strip under {dist} -- the build produced no artefacts")
    return stripped


def measure_android_alignment(dist: Path, libraries: list[Path], ndk: Optional[str]) -> Path:
    """Measure the LOAD-segment alignment of every shipped .so, record it in
    the dist, AND assert it is at least 16 KB.

    Android 15+ devices may use 16 KB pages; a library whose LOAD segments are
    aligned to 4 KB does not load there at all, and this project has no device
    or emulator instrument that would ever notice.

    The assertion was added only AFTER the measurement existed (plan F5): CI
    run 33460559016 produced both libraries with p_align 0x1000 on every LOAD
    segment, which falsified the handoff note claiming NDK r27c aligns to 16 KB
    by default. The remediation is CMAKE_SHARED_LINKER_FLAGS
    ``-Wl,-z,max-page-size=16384`` in the component's android overlay; this
    check exists so that flag cannot silently stop taking effect -- passing a
    linker flag and the artefact carrying its effect are different facts.

    The report file is written BEFORE the assertion fires, so a failing run
    still leaves the measurement behind to read.

    Returned path is committed with the dist, so the CI gate and PROVENANCE.md
    quote a measurement that travels with the binaries it describes.
    """
    report = Path(dist) / "share" / "provenance" / "android_so_alignment.txt"
    report.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    measured: list[tuple[str, list[str]]] = []
    for library in libraries:
        segments = run([ndk_tool(ndk, "llvm-readelf"), "-l", str(library)]).stdout
        lines.append(f"=== {library.name} ===")
        lines.append(segments)
        aligns = [
            line.split()[-1]
            for line in segments.splitlines()
            if line.strip().startswith("LOAD")
        ]
        lines.append(f"{library.name} LOAD alignments: {' '.join(aligns) or '<none parsed>'}")
        _log(f"MEASURE        {library.name} LOAD p_align: {' '.join(aligns) or '<none parsed>'}")
        measured.append((library.name, aligns))
    report.write_text("\n".join(lines) + "\n", encoding="utf-8")
    _log(f"MEASURE        alignment report written to {report}")

    for name, aligns in measured:
        if not aligns:
            # No LOAD line parsed at all means the INSTRUMENT failed, not that
            # the library is fine. Silence here would read as a pass.
            raise _fail(
                f"no LOAD segment alignment could be parsed for {name}; see {report}"
            )
        for value in aligns:
            if int(value, 16) < ANDROID_MIN_PAGE_ALIGN:
                raise _fail(
                    f"{name} has a LOAD segment aligned to {value} "
                    f"({int(value, 16)} bytes), below the {ANDROID_MIN_PAGE_ALIGN}-byte "
                    f"minimum. Android 15+ devices with 16 KB pages cannot load it, and "
                    f"no test in this project executes on such a device.\n"
                    f"  Remediation: CMAKE_SHARED_LINKER_FLAGS "
                    f"'-Wl,-z,max-page-size=16384' in that component's android overlay "
                    f"in native/deps/manifest.toml.\n"
                    f"  Full measurement: {report}"
                )
        _log(f"ASSERT ok      {name} LOAD segments are >= {ANDROID_MIN_PAGE_ALIGN}-byte aligned")
    return report


def vendor_licences(
    loaded: dict[str, Any], dist: Path, stage: Path, *, platform: str = "linux"
) -> None:
    """Vendor every shipped component's licence BEFORE the stage cleanup
    deletes the source trees they live in."""
    dist = Path(dist)
    stage = Path(stage)

    if platform == "android":
        _vendor_licences_android(loaded, dist, stage)
        return

    # aom's licence comes from the vcpkg prefix: aom is no longer downloaded
    # here, so there is no source tree to glob and no tarball hash this script
    # could honestly verify. vcpkg's copyright file is strictly MORE complete
    # than the glob it replaces -- the registry port's vcpkg_install_copyright
    # lists LICENSE, PATENTS *and* three third-party licences (fastfeat,
    # vector, x86inc) that a maxdepth-1 glob never picked up. PATENTS matters
    # specifically (K6): the Alliance for Open Media Patent License 1.0 is a
    # SEPARATE grant on top of BSD-2, and shipping only LICENSE would drop it.
    prefix = os.environ.get("CEYX_VCPKG_PREFIX", "")
    aom_copyright = Path(prefix) / "share" / "aom" / "copyright" if prefix else Path("share/aom/copyright")
    if not aom_copyright.is_file():
        raise _fail(
            f"aom copyright file not found at {aom_copyright}\n"
            f"  aom is supplied by vcpkg since D5, so its licence is vendored from "
            f"the install prefix. Set CEYX_VCPKG_PREFIX (see build_aom).\n"
            f"  Shipping the binary without it is an unmet attribution duty."
        )
    (dist / "share" / "licenses" / "aom").mkdir(parents=True, exist_ok=True)
    shutil.copy(aom_copyright, dist / "share" / "licenses" / "aom" / "copyright")

    for name in ("libheif", "libde265", "kvazaar"):
        comp = _component(loaded, name)
        version = str(comp["version"])
        src_dir = stage / f"{name}-{version}"
        # A resumed run (e.g. the libde265 stage was already installed and
        # returned early) never extracts the source tree the build stage's own
        # extraction step would have provided. assemble() runs independently of
        # which stages actually built anything, so re-fetch + re-extract on
        # demand rather than assuming the tree is present.
        if not src_dir.is_dir():
            _refetch_source_for_licence(comp, name, version, stage)
        dest = dist / "share" / "licenses" / name
        dest.mkdir(parents=True, exist_ok=True)
        copied = copy_licence_files(src_dir, dest, comp.get("licence_files", []))
        if not copied:
            raise _fail(f"no licence file found for {name} in {src_dir}")


def _vendor_licences_android(loaded: dict[str, Any], dist: Path, stage: Path) -> None:
    """Android builds all four components from source, so every licence comes
    from its own source tree -- including aom's, whose vcpkg `copyright` file
    does not exist on this platform.

    PATENTS* is load-bearing for aom (K6): the Alliance for Open Media Patent
    License 1.0 is a SEPARATE grant on top of BSD-2, and a dist shipping only
    LICENSE has an unmet attribution duty. It is required by name here rather
    than left to the glob's mercy, because "the glob copied something" and
    "the patent grant shipped" are different facts.

    Runs BEFORE cleanup_stage() deletes these trees (the ordering the desktop
    path also depends on).
    """
    for name in ("libheif", "libde265", "kvazaar", "aom"):
        comp = _component(loaded, name)
        version = str(comp["version"])
        block = execute_mod.resolve_source(loaded, name, "android")
        src_dir = stage / execute_mod.source_dirname(block, name, version)
        if not src_dir.is_dir():
            # A resumed run (an already-installed stage returned early) never
            # extracted the tree; re-acquire rather than assume it is there.
            execute_mod.acquire(loaded, name, "android", stage)
        dest = dist / "share" / "licenses" / name
        dest.mkdir(parents=True, exist_ok=True)
        copied = copy_licence_files(src_dir, dest, comp.get("licence_files", []))
        if not copied:
            raise _fail(f"no licence file found for {name} in {src_dir}")
        _log(f"ASSERT ok      {name} licences vendored: {[p.name for p in copied]}")
    patents = sorted((dist / "share" / "licenses" / "aom").glob("PATENTS*"))
    if not patents:
        raise _fail(
            "aom's PATENTS file was not vendored -- the Alliance for Open Media "
            "Patent License 1.0 is a separate grant on top of BSD-2, and shipping "
            "LICENSE alone drops it (K6)"
        )
    _log(f"ASSERT ok      aom patent grant vendored: {[p.name for p in patents]}")


def _refetch_source_for_licence(comp: dict[str, Any], name: str, version: str, stage: Path) -> None:
    """Re-acquire a source tarball purely to vendor its licence files.

    libde265 is supplied by vcpkg, so its ``source.default`` block carries
    ``historical_url``/``historical_sha256`` rather than ``url``/``sha256``:
    the manifest refuses to state a hash the build path no longer verifies.
    The version pin stays meaningful because the overlay port pins the SAME
    release asset, so these bytes are the licence text that matches the
    binary we ship -- and the hash IS verified right here, at the one place
    the tarball is actually consumed.
    """
    block = comp["source"]["default"]
    url = str(block.get("url") or block["historical_url"]).replace("{version}", version)
    sha256 = str(block.get("sha256") or block["historical_sha256"])
    tarball = stage / f"{name}-{version}.tar.gz"
    if not tarball.is_file():
        fetch_mod.fetch_tarball(url, sha256, tarball)
    else:
        fetch_mod.verify_sha256(tarball, sha256)
    execute_mod.extract_tarball(tarball, stage)


def copy_licence_files(src_dir: Path, dest: Path, patterns: list) -> list[Path]:
    """Case-insensitive, depth-1 glob over ``src_dir`` for the manifest's
    ``licence_files`` patterns (the shell script's ``find -maxdepth 1
    -iname``). Returns the files copied."""
    lowered = [str(p).lower() for p in patterns]
    copied: list[Path] = []
    if not Path(src_dir).is_dir():
        return copied
    for entry in sorted(Path(src_dir).iterdir()):
        if not entry.is_file():
            continue
        name = entry.name.lower()
        if any(_iglob_match(name, pattern) for pattern in lowered):
            shutil.copy(entry, Path(dest) / entry.name)
            copied.append(entry)
    return copied


def _iglob_match(lowered_name: str, lowered_pattern: str) -> bool:
    import fnmatch

    return fnmatch.fnmatch(lowered_name, lowered_pattern)


def cleanup_stage(loaded: dict[str, Any], stage: Path, *, platform: str = "linux") -> None:
    """Remove the build and source trees. On macOS/Linux there are no
    build-de265/build-aom entries: those stages build nothing since D5, so
    nothing of theirs is ever staged. Pre-D5 leftovers are removed by the stamp
    change forcing a fresh dist rather than by naming paths this script can no
    longer create.

    Android builds all four components from source, so its stage carries two
    extra build trees and aom's source tree (whose directory name is
    "libaom-<version>", not "aom-<version>" -- read from the manifest rather
    than guessed, or the tree would silently survive cleanup)."""
    stage = Path(stage)
    targets = [stage / "build-kvazaar", stage / "build-libheif"]
    names = ["libheif", "libde265", "kvazaar"]
    if platform == "android":
        targets += [stage / "build-libde265", stage / "build-aom"]
        names.append("aom")
    for name in names:
        version = str(_component(loaded, name)["version"])
        if platform == "android":
            block = execute_mod.resolve_source(loaded, name, "android")
            targets.append(stage / execute_mod.source_dirname(block, name, version))
        else:
            targets.append(stage / f"{name}-{version}")
    for target in targets:
        if target.is_dir():
            shutil.rmtree(target, ignore_errors=True)

    if platform == "android" and stage.name == ".stage":
        # The android dist is COMMITTED (D5), and the default stage directory
        # lives INSIDE it. Removing only the build/source trees would leave the
        # downloaded tarballs (~tens of MB) sitting in a directory git is told
        # to track -- source archives entering version control by accident,
        # which no reviewer would notice in a dist commit full of binaries.
        shutil.rmtree(stage, ignore_errors=True)


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------
def build(
    loaded: dict[str, Any],
    platform: str,
    arch: str,
    dist: Path,
    *,
    stage_arg: str = "all",
    stage_dir: Optional[Path] = None,
    ndk: Optional[str] = None,
) -> int:
    """Run one stage (or all five, in order) of the HEIF dist assembly.

    The per-component split exists so each build is its own foreground
    invocation short enough to fit inside a normal command timeout, instead
    of one call that has to survive both source builds back to back. Each
    stage is independently idempotent.
    """
    if stage_arg not in STAGES:
        raise HeifError(f"[heif] unknown stage {stage_arg!r} (expected one of {STAGES})")
    traits(platform)  # reject Windows early, with the pointer to the right script

    dist = Path(dist)
    stage = Path(stage_dir) if stage_dir is not None else dist / ".stage"

    if platform == "android" and not ndk:
        raise _fail(
            "the android HEIF stack is a cross-compile and needs --android-ndk "
            "<path>; refusing to fall back to a host toolchain under an android "
            "label (that would produce a dist whose name promises arm64-v8a and "
            "whose bytes are the host's)"
        )

    if stage_arg == "all" and stamp_is_current(dist, loaded, arch, platform, ndk):
        _log("dist already at the pinned versions:")
        for name in ("libheif", "libde265", "kvazaar", "aom"):
            _log(f"  {name:<8} {_component(loaded, name)['version']}")
        _log(f"  arch     {arch}")
        return 0

    _log(f"building dist for architecture: {arch} (stage: {stage_arg})")
    stage.mkdir(parents=True, exist_ok=True)

    # Build order is LOAD-BEARING, not stylistic: libheif's FindLIBDE265 does a
    # find_library() against a file on disk, and its kvazaar/aom probes read the
    # install prefix -- all three must be installed before libheif configures,
    # and all three probes are non-fatal, so getting the order wrong produces a
    # green build with silently missing codecs.
    if stage_arg in ("all", "libde265"):
        if platform == "android":
            build_libde265_android(loaded, arch, dist, stage, ndk)
        else:
            build_libde265(loaded, platform, dist)
    if stage_arg in ("all", "kvazaar"):
        build_kvazaar(loaded, platform, arch, dist, stage, ndk=ndk)
    if stage_arg in ("all", "aom"):
        if platform == "android":
            build_aom_android(loaded, arch, dist, stage, ndk)
        else:
            build_aom(loaded, dist)
    if stage_arg in ("all", "libheif"):
        build_libheif(loaded, platform, arch, dist, stage, ndk=ndk)
    if stage_arg in ("all", "assemble"):
        assemble(loaded, platform, arch, dist, stage, ndk=ndk)
    return 0
