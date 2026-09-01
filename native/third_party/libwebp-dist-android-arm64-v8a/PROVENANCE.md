# libwebp distribution (Android arm64-v8a) — provenance

Built by `python3 native/scripts/build_deps.py build libwebp --platform
android --arch arm64-v8a --android-ndk "$ANDROID_NDK_HOME"`, run on a
GitHub Actions runner via `.github/workflows/webp_dist_android.yml`. Like the
Windows dist, this tree is **committed**: no contributor machine in this
project is expected to hold an NDK, so the built bytes are a reviewed input,
pinned once and changed only by a visible diff (plan D5).

There is no shell script behind this dist. `build_deps.py` is the single
entry point (ENTRY-POINT RULE); the configure flags below are rendered from
`native/deps/manifest.toml` rather than transcribed into a workflow, so the
flags in this file and the flags the build used cannot drift apart.

| Component | Version | Source | SHA-256 (upstream tarball) |
|---|---|---|---|
| libwebp | 1.6.0 | https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz | `e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564` |

Same version and SHA-256 as the macOS/Linux (vcpkg, `overrides` pin 1.6.0)
and Windows dists — one upstream release, four platforms. On android the
bytes are fetched directly (`[component.libwebp.source.android]`, kind
`tarball`) because this repo runs no vcpkg on the android leg; the url and
hash there are copied verbatim from the `historical_*` pair recorded in
`source.default`, so "same upstream release" is a checked fact, not a claim.

## Licence and linkage

BSD-3-Clause, **static**. No LGPL relink duty, so these archives are linked
into `libdng_decoder_native.so` rather than shipped as extra `.so` files —
the APK gains no library from libwebp. Licence text is vendored under
`share/licenses/libwebp/` by the same build command that produces the
archives (`deps/android_dist.py:vendor_licences`), which fails the build if
no licence file matched, so an unlicensed dist cannot be produced.

## Codec set / configure flags

Rendered from `[component.libwebp.cmake.base]` + `[component.libwebp.cmake.android]`:

```
-DCMAKE_BUILD_TYPE=Release
-DCMAKE_INSTALL_PREFIX=<dist>
-DCMAKE_POSITION_INDEPENDENT_CODE=ON
-DBUILD_SHARED_LIBS=OFF
-DWEBP_BUILD_ANIM_UTILS=OFF
-DWEBP_BUILD_CWEBP=OFF
-DWEBP_BUILD_DWEBP=OFF
-DWEBP_BUILD_GIF2WEBP=OFF
-DWEBP_BUILD_IMG2WEBP=OFF
-DWEBP_BUILD_VWEBP=OFF
-DWEBP_BUILD_WEBPINFO=OFF
-DWEBP_BUILD_EXTRAS=OFF
-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake
-DANDROID_ABI=arm64-v8a
-DANDROID_PLATFORM=android-24
-DWEBP_BUILD_WEBPMUX=ON
```

Notes on the two flags that are not "everything off":

- `CMAKE_POSITION_INDEPENDENT_CODE=ON` is mandatory: these archives link into
  a shared library, and a non-PIC archive fails that link with a relocation
  error naming the **shared library**, not the archive — a confusing failure
  three steps from its cause.
- `WEBP_BUILD_WEBPMUX=ON` (the one tool flag turned on) guarantees the mux
  writer needed for EXIF/XMP/ICC embedding. Every other `WEBP_BUILD_*` stays
  off: the CLI tools are useless in a cross dist and some do not
  cross-compile.
- `ANDROID_PLATFORM=android-24` matches `native/CMakePresets.json`'s
  `android-vulkan` preset. The dist and its consumer must target the same API
  level, or the dist can reference a newer libc than the decoder may link
  against.

## Contents

```
include/webp/{encode,decode,mux,demux}.h
lib/{libwebp.a,libsharpyuv.a,libwebpmux.a,libwebpdemux.a}
share/licenses/libwebp/
```

`libsharpyuv.a` is not optional in 1.6: the YUV conversion is factored out of
`libwebp.a`, and omitting it surfaces as an undefined `SharpYuvConvert` at the
consumer's link.

## Assertions

Run automatically at the end of the build command above
(`deps/android_dist.py:assert_dist`); a failure is a non-zero exit, so a
green build cannot mean "the archives exist but contain no encoder". Every
check **captures the tool's output to a file first and searches the file
afterwards** — never `llvm-nm x.a | grep -q SYM`, which under `pipefail`
reports 141 (SIGPIPE) precisely when the symbol IS present. The captured
evidence is written to the build's scratch directory
(`<dist>/.stage/assertions/`), deliberately outside the shipped tree: it is
evidence *about* the artefact, not part of it, and this dist is committed.

| Assertion | Instrument | Green condition |
|---|---|---|
| A-SYMS[lib/libwebp.a] | `llvm-nm lib/libwebp.a > libwebp.a.nm.txt` | `WebPEncodeRGBA` and `WebPDecodeRGBA` present |
| A-SYMS[lib/libsharpyuv.a] | `llvm-nm lib/libsharpyuv.a > libsharpyuv.a.nm.txt` | `SharpYuvConvert` present |
| A-SYMS[lib/libwebpmux.a] | `llvm-nm lib/libwebpmux.a > libwebpmux.a.nm.txt` | `WebPMuxCreateInternal` present |
| A-SYMS[lib/libwebpdemux.a] | `llvm-nm lib/libwebpdemux.a > libwebpdemux.a.nm.txt` | `WebPDemuxInternal` present |
| A-ARCH | `llvm-readelf -h lib/libwebp.a > machine.readelf.txt` | reports `Machine: AArch64` |
| A-LICENCE | file presence | `share/licenses/libwebp/` holds at least one file |
| A-SRC-HASH | `hashlib.sha256` before extraction | matches the pin in the table above |

`llvm-nm` / `llvm-readelf` are the NDK's own, resolved from the NDK root
passed to `--android-ndk` (`deps/assertions.py:ndk_tool`) — never the host's,
which would report on the wrong object format.

## Producer run and committed bytes

**PENDING** — this section is filled in the same commit that adds the built
bytes, and must name: the `webp_dist_android.yml` run URL, the commit it ran
against, the NDK version the runner used, and the SHA-256 of every committed
file. Until then this directory contains provenance only, no artefacts: a
PROVENANCE.md that describes bytes which are not here yet is a description of
intent, and is marked as such rather than left to look like a record.
