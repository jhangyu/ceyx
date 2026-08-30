# libwebp distribution (Windows x86-64) — provenance

Built by `native/scripts/build_libwebp_dist_windows.sh`, run on a
`windows-latest` GitHub Actions runner via
`.github/workflows/webp_dist_windows.yml`. Like the Windows HEIF dist, this
tree is **committed**: no contributor machine in this project can build
Windows binaries, so the built bytes are a reviewed input, pinned once and
changed only by a visible diff.

| Component | Version | Source | SHA-256 (upstream tarball) |
|---|---|---|---|
| libwebp | 1.6.0 | https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz | `e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564` |

Same version and SHA-256 as the macOS/Linux dist
(`native/third_party/libwebp-dist/PROVENANCE.md` and
`native/scripts/fetch_libwebp_dist.sh`) — one pin, two platforms.

**Static**, unlike the HEIF Windows dist: libwebp is BSD-3-Clause with no
LGPL relink duty, so this adds no DLL to the shipped set.

## Configure flags

```
-G Ninja -DCMAKE_BUILD_TYPE=Release
-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
-DBUILD_SHARED_LIBS=OFF
-DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF
-DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF
-DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF
```

`WEBP_BUILD_*=OFF` disables only the command-line tools; the mux and demux
**libraries** are built regardless.

## Archive naming (observed for real, not assumed)

CMake's install step keeps the `lib` prefix on the archive names even under
the clang-cl + Ninja toolchain (`CMAKE_STATIC_LIBRARY_PREFIX` follows the
compiler ID — Clang, not MSVC's `cl.exe` — so it defaults to `lib` here). The
committed files are `lib/libwebp.lib`, `lib/libwebpmux.lib`,
`lib/libwebpdemux.lib`, `lib/libsharpyuv.lib` (plus `lib/libwebpdecoder.lib`,
a decode-only convenience archive CMake also installs). `cmake/encode.cmake`'s
`find_library(NAMES webp)` already searches both the prefixed and unprefixed
spellings, so no consumer-side change was needed.

Produced by run
https://github.com/jhangyu/ceyx/actions/runs/33307183409 (branch
`ci/webp-dist-windows`, commit `5b5dff9`), which logged
`WEBP_DIST_WINDOWS_RC=0` and all four encoder/mux symbol assertions passing.

## Committed artifact SHA-256 (downloaded bytes, verified before commit)

| File | SHA-256 |
|---|---|
| `lib/libwebp.lib` | `9b241e55e0ba5eeca6f903042a0f6818ed9c72a891b4b4b34acaa21dcc174943` |
| `lib/libwebpmux.lib` | `ea066a98af315e825ef07f81187b58f72436373fc6dd451b6642554ca3b3b822` |
| `lib/libwebpdemux.lib` | `9ec52007ee7c90031bbfa3e1f362f7ff3e9691968fde4132541e9d57d145a55a` |
| `lib/libsharpyuv.lib` | `d6d7df9186de7388d0af8bf8b4b9aa61c4eae5c4315d9f60e2907dbb9a31890f` |
| `lib/libwebpdecoder.lib` | `fb9650c03ca7e40b3ad50acba27a2faf1ef3e9fe409950586fca6d16f097755c` |

## Symbol verification (local, against the downloaded bytes)

Using `/Library/Developer/CommandLineTools/usr/bin/llvm-nm --defined-only`
(the release-channel `llvm-nm` is not on `PATH` on this machine):

- `lib/libwebp.lib` contains `WebPEncodeRGBA` and `WebPEncodeLosslessRGBA`.
- `lib/libwebpmux.lib` contains `WebPMuxSetChunk` and `WebPMuxAssemble`.

## Licence

BSD-3-Clause. Vendored at `share/licenses/libwebp/COPYING` (identical to the
macOS/Linux dist's licence file).

## .gitignore note

`native/third_party/libwebp-dist-*/` is a blanket ignore rule covering
architecture-suffixed macOS/Linux dist variants (e.g.
`libwebp-dist-arm64/`), which are always locally rebuilt and never tracked.
This Windows dist is the opposite case — no local machine can rebuild it — so
`.gitignore` carries a narrow `!native/third_party/libwebp-dist-windows/`
exception immediately below that rule, mirroring how `heif-dist-windows/` is
already exempted from its own family's ignore pattern.
