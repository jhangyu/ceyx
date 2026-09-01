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

| Field | Value |
|---|---|
| Run | https://github.com/jhangyu/ceyx/actions/runs/33454853873 (`webp_dist_android.yml`, `workflow_dispatch`) |
| Commit built | `0061ef120bfaa0a422e10528a29c93a701a6457c` |
| Runner | `ubuntu-latest`, CMake 3.31.6 |
| NDK | **r27c**, pinned in the workflow (`nttld/setup-ndk`), resolved to `/opt/hostedtoolcache/ndk/r27c/x64` — not the runner image's preinstalled NDK, which can drift when GitHub bumps the image |
| Build step exit code | `WEBP_DIST_ANDROID_RC=0`, self-captured on the line after the command inside the step |
| Assertions | `[android-dist] ok: 1 licence file(s) vendored, assertions green` |

### Independently re-verified before committing

The run's assertions used the NDK's `llvm-nm`/`llvm-readelf`. The bytes were
then checked again on a second machine with a **different instrument** — the
ELF headers parsed directly out of the archive bytes (`e_machine` at offset
18 of each member), and the capability symbol names searched in the raw
archive — because two independent instruments agreeing is worth more than
trusting one twice. Result: every member of all four archives reports
`e_machine = 0xb7` (EM_AARCH64) and nothing else, and all five capability
symbols are present. Driver: `native/scripts/tmp/a-t2-harvest.py`.

### Not committed from the build output

- `.stage/` — build scratch (source tree, CMake cache, and the captured
  assertion dumps). Evidence about the artefact, not part of it.
- `bin/webpmux`, `share/man/man1/webpmux.1` — `WEBP_BUILD_WEBPMUX=ON` builds
  the CLI tool and its manpage as a side effect of building the mux library.
  Nothing consumes them, and unused CLI/manpages were already ruled out of
  dists (commit `903ce30`).

`lib/libwebpdecoder.a`, `lib/libcpufeatures-webp.a`, `lib/pkgconfig/` and
`share/WebP/cmake/` ARE kept: they are part of what `cmake --install`
produces, they are what `find_package(WebP CONFIG)` consumes, and the
committed Windows dist keeps its equivalents.

### SHA-256 of every committed file

| SHA-256 | File |
|---|---|
| `928e8d3ed502baa0f47ed10913ee018ed6172afcd4f308e0ea005a126b83f27e` | include/webp/encode.h |
| `e554551d085f234e930f36b5879a77ef58bfea34c48ebfd620426e63b224025c` | include/webp/decode.h |
| `c72cf593f8194c671efcade33f1678294380120d45a337511d612a3acb643f35` | include/webp/mux.h |
| `9b0d10c0fa1ac2dc750c4d687b038e40a685bb9240cb045759f5b9546017361d` | include/webp/demux.h |
| `ca789c9fe2edc52f759cb3888e9171ee44b5c12f2db21458f96176d578bc897e` | include/webp/mux_types.h |
| `992d2ffe864adb6d80cc91b7a062074d167ed96772f28d104a650148eca31794` | include/webp/types.h |
| `4fcb150246232b0e8e605752deb66ca861872f86dd692308d9f854e10ad964ee` | include/webp/sharpyuv/sharpyuv.h |
| `80c23c727edb1ce6a42b38f9458c3d75c572856c898f0cd913ba03468afb9d3a` | include/webp/sharpyuv/sharpyuv_csp.h |
| `bd5831988b34cb0a34631564a8537b40d2a76758d576d7e9561b904b237ec20b` | lib/libwebp.a |
| `1343f5ae08037b9e3608e9610e0bdf03233daec07386dd3043b24caa6e0f2f62` | lib/libsharpyuv.a |
| `d59af7bd0d1b14374cff4d0b32e7c15127ae3e5c74917f9b586dd70537696620` | lib/libwebpmux.a |
| `bacd909552de4921be1c8fd2bfc2e2a7ca15692deb0e78ce9903afc5c4f46139` | lib/libwebpdemux.a |
| `90f3637b474bb8d813b11d56bcde0164cbe6e5da8618e1450ac80a4bc7e50c03` | lib/libwebpdecoder.a |
| `4d9fb96ac6c9881e6912b13b96b0d67651c861c5d4ffef7b48c76f4b4f6aba73` | lib/libcpufeatures-webp.a |
| `85677cf683dabd9e6e92ac47d6223322648eb07523c51650b37c554f111ebac2` | lib/pkgconfig/libwebp.pc |
| `5d60e08ab582fc81dba706cfe8e32eeb1aa36deeeb213ae4865e57f9bb56b357` | lib/pkgconfig/libsharpyuv.pc |
| `f185a04d402c8edfb34a1b0e8935786a6f472a207f41310e375a394052c45dbf` | lib/pkgconfig/libwebpmux.pc |
| `a684cb6d89d82a9556360cc78e90412958d74ab179952950446d1c52d72859ca` | lib/pkgconfig/libwebpdemux.pc |
| `ff1a2df284a269cd80842e0bb11f3da45134f73f2d595ace9aacb24ec77323a0` | lib/pkgconfig/libwebpdecoder.pc |
| `f4c6bfab9f3e53f8e126ddee597d7ec7c096f7427218b3b404ed97f7497074e1` | share/WebP/cmake/WebPConfig.cmake |
| `c9be14d51d2108603dd1bd53c9e27945cea0d40bd970e5f27c6318373af3ade0` | share/WebP/cmake/WebPConfigVersion.cmake |
| `f674b32d8a4c949f65302b30dd214267e87b066935cb81e4f2b3c3d28beb5ef6` | share/WebP/cmake/WebPTargets.cmake |
| `81786edc8d8742a94cc9d55ce15d5d02fa875049daf94fef59c60dec7fa49b22` | share/WebP/cmake/WebPTargets-release.cmake |
| `5aec868f669e384a22372a4e8a1a6cd7d44c64cd451f960ca69cc170d1e13acf` | share/licenses/libwebp/COPYING |

Static-library builds are not bit-for-bit reproducible (accepted limitation,
plan §9), so re-running the workflow will produce different hashes for the
same sources. These hashes pin *these* committed bytes, not a claim of
reproducibility.
