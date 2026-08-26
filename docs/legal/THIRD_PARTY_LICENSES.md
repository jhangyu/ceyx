# Third-Party Licenses

Components whose code is compiled or statically linked into the shipped binaries
(`libdng_decoder_native.dylib`, the Android `.so`, and any app bundle embedding them).
Redistributing those binaries carries the attribution obligations below.

Full license texts live with each component; this file is an index, not a substitute.

Completeness for LibRaw, RawSpeed, pugixml, zlib and libjpeg is confirmed by Phase 17
Task 14 (see `docs/logs/2026-08-24/Task_P17_acceptance_report.md`) and is gated
mechanically by `native/scripts/verify_raw_provenance.py`.

## Adobe DNG SDK

- Used for: DNG parsing, LJPEG tile decompression, OpcodeList2/3 (Stages 1–2).
- Linkage: compiled from source into the native library.
- License: Adobe DNG SDK License Agreement (royalty-free use, reproduction and
  distribution; see the agreement for the restrictions that apply).
- Text: `native/third_party/dng_sdk/LICENSE`.

## Halide

- Used for: AOT-compiled Stage 3/4 GPU kernels; the Halide runtime is linked into
  the native library.
- Linkage: statically linked runtime. The Halide binary distribution itself is not
  tracked in git — it is fetched by
  `native/scripts/fetch_halide_v21_dist.sh` into
  `native/third_party/halide/`.
- License: MIT. Text ships inside the fetched distribution; upstream copy at
  <https://github.com/halide/Halide/blob/main/LICENSE.txt>.

## LibRaw

- Source: https://github.com/LibRaw/LibRaw.git
- Revision: `df226ea4178ccd74245f4f13c23adddfa01411c9`
- License: dual-licensed LGPL-2.1 / CDDL-1.0; this project elects **LGPL-2.1**.
- Linking: statically linked into `dng_decoder_native` (and the standalone
  `libraw_smoke` CPU-only diagnostic binary) when
  `-DDNG_ENABLE_GENERIC_RAW=ON` (default). LGPL-2.1 static linking requires
  making relinkable object files, or the complete corresponding source,
  available to recipients of the built library. The full LibRaw source at
  the pinned revision is retained (untracked, fetched by
  `native/scripts/fetch_libraw_dist.sh`) and its provenance is
  recorded in `native/third_party/libraw/PROVENANCE.md`; the
  source offer is this repository plus that script.
- Full license text: `native/third_party/libraw/LICENSE.LGPL`
  (fetched into the vendored tree).

## RawSpeed (bundled RawSpeed3)

- Source: https://github.com/darktable-org/rawspeed.git
- Revision: `de70ef5fbc62cde91009c8cff7a206272abe631e` (see
  `native/third_party/libraw/PROVENANCE.md` for the pin
  substitution rationale vs. the originally planned revision)
- License: LGPL-2.1
- Linking: RawSpeed3 is compiled as a static library and linked only into
  the LibRaw target (`raw`), consumed via LibRaw's `open_file()`/`unpack()`;
  it is never exposed as a standalone target to `dng_decoder_native` or any
  Halide/AOT target (spec section 6.6). Same static-linking source-offer
  treatment as LibRaw above applies.

## pugixml

- Bundled by RawSpeed at the above revision; fetched at build configure
  time as a hash-pinned tarball (`pugixml-1.9.tar.gz`,
  SHA512-verified — see
  `native/third_party/libraw/RawSpeed3/rawspeed/cmake/Modules/Pugixml.cmake.in`).
- License: MIT.

## LibRaw-cmake

- Source: https://github.com/LibRaw/LibRaw-cmake.git
- Revision: `eb98e4325aef2ce85d2eb031c2ff18640ca616d3`
- License: MIT.
- Role: community-maintained CMake build overlay for LibRaw (LibRaw itself
  ships no CMakeLists.txt at the pinned revision, see
  `native/third_party/libraw/PROVENANCE.md`). Build-time only,
  contributes no source to the shipped library.

## libjpeg-turbo

- Used for: baseline/lossy JPEG decoding inside the Adobe DNG SDK (`qDNGUseLibJPEG=1`).
- Vendored at `native/third_party/libjpeg-turbo/`.
- Linkage: **statically linked** on every platform.
  - macOS/host: Homebrew's `libjpeg.a` (`brew --prefix jpeg-turbo`).
    Static linking is deliberate — see `native/CMakeLists.txt` (`elseif(APPLE)` branch);
    linking the shared library stamps an absolute `/opt/homebrew` path that makes the dylib
    unloadable inside App-Sandboxed host apps.
  - Android/Windows: built from the vendored source (NEON SIMD on arm64;
    x86 SIMD needs NASM, falls back to `WITH_SIMD=OFF` when absent).
    See `native/CMakeLists.txt`, JPEG section (~line 110).
- License: IJG (Independent JPEG Group) License + Modified 3-clause BSD License;
  the SIMD sources are zlib-licensed. Permissive; no source-offer obligation.
- Text: `native/third_party/libjpeg-turbo/LICENSE.md`
  and `native/third_party/libjpeg-turbo/README.ijg`.
  For the macOS host build the corresponding text ships with the formula at
  `$(brew --prefix jpeg-turbo)/LICENSE.md`.

## zlib

- macOS/Android: linked dynamically against the platform's `/usr/lib/libz.1.dylib`
  (macOS) or the NDK sysroot copy (Android) — not redistributed by this project.
  Probe at `native/CMakeLists.txt` `if(ANDROID)` zlib block.
- Windows: user-supplied `-DDNG_ZLIB_ROOT` prefix wins; otherwise fetched
  and statically built from source at configure time —
  `zlib-1.3.1.tar.gz` from https://github.com/madler/zlib, SHA256-pinned
  in `FetchContent_Declare(dng_zlib ...)` (no `zlib1.dll` runtime
  dependency, consistent with the static libjpeg-turbo policy).
- License: zlib License (permissive, no source-offer obligation).

## x3f-tools (Foveon X3F support, bundled by LibRaw)

- Source: Kalpanika x3f-tools, redistributed inside LibRaw at the pinned
  LibRaw revision as `native/third_party/libraw/src/x3f/`
  (`x3f_parse_process.cpp`, `x3f_utils_patched.cpp`) and
  `internal/x3f_tools.h`. No separate fetch: the code is part of the
  LibRaw source tree already vendored per the LibRaw entry above.
- License: BSD-3-Clause, as redistributed by LibRaw (see
  `native/third_party/libraw/LICENSE.LGPL` /
  `native/third_party/libraw/COPYRIGHT` for LibRaw's own
  bundling notice).
- Build policy: dead code prior to Phase 19 W2 (compiled but entirely
  inside `#ifdef USE_X3FTOOLS`, never defined). `ENABLE_X3FTOOLS=ON` (set
  in `native/CMakeLists.txt`, see
  `native/third_party/libraw/PROVENANCE.md` "Foveon X3F
  support (Phase 19 W2)") makes the LibRaw-cmake overlay define
  `USE_X3FTOOLS`, so this code now ships live in `dng_decoder_native`.
  Permissive license; no source-offer obligation beyond what LibRaw's own
  entry already provides.
