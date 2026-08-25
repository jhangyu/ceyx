# Third-Party Licenses

Components whose code is compiled or statically linked into the shipped binaries
(`libdng_decoder_native.dylib`, the Android `.so`, and any app bundle embedding them).
Redistributing those binaries carries the attribution obligations below.

Full license texts live with each component; this file is an index, not a substitute.

## libjpeg-turbo

- Used for: baseline/lossy JPEG decoding inside the Adobe DNG SDK (`qDNGUseLibJPEG=1`).
- Linkage: **statically linked** on every platform.
  - macOS/host: Homebrew's `libjpeg.a` (`brew --prefix jpeg-turbo`).
    Static linking is deliberate — see `native/CMakeLists.txt` (`elseif(APPLE)` branch);
    linking the shared library stamps an absolute `/opt/homebrew` path that makes the dylib
    unloadable inside App-Sandboxed host apps.
  - Android: built from `native/third_party/libjpeg-turbo`.
- License: IJG (Independent JPEG Group) License + Modified 3-clause BSD License;
  the SIMD sources are zlib-licensed.
- Text: `native/third_party/libjpeg-turbo/LICENSE.md`
  and `native/third_party/libjpeg-turbo/README.ijg`.
  For the macOS host build the corresponding text ships with the formula at
  `$(brew --prefix jpeg-turbo)/LICENSE.md`.

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

## zlib

- Linked dynamically against the platform's `/usr/lib/libz.1.dylib` (macOS) or the
  NDK sysroot copy (Android) — not redistributed by this project.
