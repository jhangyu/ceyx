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
  `python3 native/scripts/build_deps.py fetch halide` into
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
  `python3 native/scripts/build_deps.py fetch libraw`) and its provenance is
  recorded in `native/third_party/libraw/PROVENANCE.md`; the
  source offer is this repository plus that command.
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

## LLVM OpenMP runtime (libomp)

- Source: LLVM OpenMP runtime https://openmp.llvm.org/, version **22.1.8**,
  taken from the Homebrew `libomp` bottle and **committed directly** to this
  repository at `native/third_party/libomp/` (756 KB: `lib/libomp.dylib`,
  `include/omp.h`, `LICENSE.TXT`). No fetch script — see that directory's
  `PROVENANCE.md` for digests and the arm64-only caveat.
- License: **Apache License v2.0 with LLVM Exceptions**, per the bundled
  `native/third_party/libomp/LICENSE.TXT`. Note `brew info libomp` reports
  "License: MIT", which does not match the shipped licence text; the file
  bundled with the binary is authoritative and is what is redistributed here.
- Redistribution obligations (Apache-2.0 §4): the licence text ships in-tree at
  `native/third_party/libomp/LICENSE.TXT`, and the binary is unmodified — no
  "Modifications" notice is required. The LLVM Exception removes the
  obligation to attribute in object-code form when the runtime is merely
  linked, but the licence file is retained regardless. No source-offer
  obligation (not a copyleft licence).
- Build policy: desktop only. `CEYX_ENABLE_DESKTOP_OPENMP`
  (`native/cmake/tests.cmake`) is ON for macOS/Linux/Windows and OFF for
  iOS/Android, so **libomp is not redistributed in mobile builds**; those use
  the `std::thread` pool from project patch 09 instead.
- **Shipping note:** `libdng_decoder_native.dylib` links
  `@rpath/libomp.dylib`, so a packaged macOS app must include `libomp.dylib`
  in its `Frameworks/` directory alongside `liblcms2`/`libjpeg`. The build
  stages this copy automatically for dev/test builds; app packaging is
  tracked separately.

## libheif

- Used for: HEIF/AVIF container parsing, primary-item selection, `irot`/`imir`
  transform handling, and YUV to RGBA colour conversion for `.heic`/`.heif`.
- Version: **1.23.2**
- Source: <https://github.com/strukturag/libheif/releases/download/v1.23.2/libheif-1.23.2.tar.gz>
- SHA-256: `8bd5d41d19dc84536d118b04774709f244df6104ef66d623dad5fa4650143405`
- License: **LGPL-3.0-or-later** (`docs/legal/LGPL-3.0.txt`). The sample
  applications and the Go/C++ wrappers are MIT, and none of them are built or
  shipped.
- Linkage: **dynamic**. Built as a separate shared library `libheif.1.dylib`,
  vendored by `python3 native/scripts/build_deps.py --component heif-stack`
  (the legacy `fetch_heif_deps.sh` shell script was deleted) into
  `native/third_party/heif-dist/` and staged next to
  `libdng_decoder_native.dylib`. `@rpath/libheif.1.dylib` install name.

## libde265

- Used for: HEVC (H.265) intra decoding of the coded image item behind libheif.
- Version: **1.1.1**
- Source: <https://github.com/strukturag/libde265/releases/download/v1.1.1/libde265-1.1.1.tar.gz>
- SHA-256: `fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219`
- License: **LGPL-3.0-or-later** (`docs/legal/LGPL-3.0.txt`).
- Linkage: **dynamic**. Built as `libde265.0.dylib`, `@rpath/libde265.0.dylib`
  install name, `ENABLE_ENCODER=OFF` (decode-only).

## kvazaar

- Used for: HEVC encoding, statically linked into the shipped `libheif` (not a
  separate library file) so `.heic` files can be produced, not just read.
- Version: **2.3.1**
- Source: <https://github.com/ultravideo/kvazaar/releases/download/v2.3.1/kvazaar-2.3.1.tar.gz>
- SHA-256: `2510b8ecc2bf384bbc7b8fc2756bbfa8a8c173b57634c8dfdd8bea6733e56c46`
- License: **BSD-3-Clause**.
- Linkage: **static**, built as `libkvazaar.a` and linked into `libheif`
  (`ENABLE_PLUGIN_LOADING=OFF`, so every enabled codec compiles directly into
  `libheif` rather than being dlopen-ed from a plugin directory).

## aom (libaom)

- Used for: AV1 encode and decode, i.e. AVIF import and export. Statically
  linked into the shipped `libheif`.
- Version: **3.12.1**
- Source: <https://storage.googleapis.com/aom-releases/libaom-3.12.1.tar.gz>
- SHA-256: `9e9775180dec7dfd61a79e00bda3809d43891aee6b2e331ff7f26986207ea22e`
- License: **BSD-2-Clause AND Alliance-for-Open-Media-Patent-License-1.0**. The
  patent grant is a separate document layered on top of the BSD-2 copyright
  license, not a substitute for it — the vendored licence files include both
  `LICENSE*` (BSD-2) and `PATENTS*` (the AOM patent grant) for this reason.
- Linkage: **static**, built as `libaom.a` and linked into `libheif`.

## libwebp

- Used for: WebP still-image decode (`CeyxStillDecoderService`) and encode
  (`CeyxEncodeService`, gated by `CEYX_ENABLE_WEBP`).
- Version: **1.6.0**
- License: **BSD-3-Clause**.
- Linkage: **static**, on every desktop platform (`native/cmake/encode.cmake`,
  `find_package(WebP CONFIG)`).

## libjxl

- Used for: JPEG XL still-image decode (`CeyxStillDecoderService`) and encode
  (`CeyxEncodeService`, gated by `CEYX_ENABLE_JXL`).
- Version: **0.12.0**
- Source: <https://github.com/libjxl/libjxl.git> (tag `v0.12.0`, built from
  source with its `third_party/brotli`, `third_party/highway` and
  `third_party/skcms` submodules; no upstream release tarball packages those
  submodules, so the pin is the tag/commit rather than a tarball SHA-256).
- License: **BSD-3-Clause**.
- Linkage: **static** (`native/cmake/jxl.cmake`).

### Why dynamic linking (libheif/libde265) and static linking (kvazaar/aom/libwebp/libjxl)

libheif and libde265 are LGPL-3.0-or-later. Shipping them as separate,
replaceable `.dylib` files satisfies LGPL-3 section 4(d)(1) directly: a user
can replace `libheif.1.dylib` / `libde265.0.dylib` in
`<App>.app/Contents/Frameworks/` with their own build. Static linking of
libheif/libde265 into `dng_decoder_native` is deliberately NOT done, because
it would trigger section 4(d)(0)'s duty to ship relinkable object files with
every release.

kvazaar and aom are the encoders that make the shipped `libheif` build
encode-capable (`WITH_KVAZAAR=ON`, `WITH_AOM_ENCODER=ON`); both are
permissively licensed, so linking them statically into `libheif` creates no
source-availability obligation beyond the existing LGPL-3 one for
libheif/libde265 itself. `WITH_X265=OFF` stays off — x265 is GPL-2.0, and
letting it in would contaminate the whole binary; `aom_codec_av1_cx`/
`aom_codec_av1_dx` and `kvz_api_get` are asserted present, and `x265_encoder`
is asserted absent, by the build's own symbol checks (`native/scripts/deps/
heif.py`). libwebp and libjxl are independent static dependencies (not part
of the HEIF stack) linked in for their own encode/decode paths; both are
permissively licensed and carry no linkage restrictions either way.

Complete corresponding source for the HEIF-stack components is the tarballs
at the URLs and hashes above, built with the flags recorded in
`native/scripts/deps/heif.py` (Python carrier; the legacy
`fetch_heif_deps.sh` shell script was deleted) and
`native/third_party/heif-dist/PROVENANCE.md`.
