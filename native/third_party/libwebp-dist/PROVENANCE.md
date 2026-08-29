# libwebp encode distribution — provenance

Built by `native/scripts/fetch_libwebp_dist.sh`. Nothing under this directory is
tracked except this file.

| Component | Version | Source | SHA-256 |
|---|---|---|---|
| libwebp | 1.6.0 | https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz | `e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564` |

Hash verified against a fresh download on 2026-08-30 and re-checked by the fetch
script on every run; a mismatch is a hard failure, never a warning.

## Licence and linkage

BSD-3-Clause. Unlike the HEIF dist (LGPL-3, therefore dynamically linked),
libwebp is linked **statically** into `libdng_decoder_native`:

* there is no relinking obligation pushing us to a separate shared library, and
* `third_party.cmake`'s 2026-08-17 App-Sandbox rule applies — an absolute
  `LC_LOAD_DYLIB` into a Homebrew prefix makes the decoder dylib unloadable
  inside a sandboxed host app. Static linking removes the dependency entirely.

Consequently `plugin/macos/Libraries/` gains **no** new dylib.

## Build shape

Encode + decode core only: `WEBP_BUILD_{CWEBP,DWEBP,GIF2WEBP,IMG2WEBP,VWEBP,
WEBPINFO,WEBPMUX,ANIM_UTILS,EXTRAS}=OFF`, `BUILD_SHARED_LIBS=OFF`,
`CMAKE_POSITION_INDEPENDENT_CODE=ON` (required: the archive is linked into a
shared library). Two archives are installed and both are needed —
`libwebp.a` and `libsharpyuv.a`; omitting the latter surfaces as undefined
`_SharpYuvConvert` at dylib link time.

The fetch script asserts mechanically after the build that `WebPEncodeRGBA` is
present in the archive and that the archive's architecture matches the one
requested (`CEYX_WEBP_ARCH`, defaulting to the host arch).

## Consumer

`native/src/ffi/encode_ffi_api.cpp` (`ceyx_encode_webp_rgba8`). If this dist is
absent, `cmake/encode.cmake` warns and compiles that entry with
`CEYX_ENABLE_WEBP=0`, so the symbol stays exported and returns
`kCeyxEncodeErrUnsupported` instead of the build failing or the Dart lookup
missing.
