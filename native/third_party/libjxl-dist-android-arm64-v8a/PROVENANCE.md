# libjxl distribution (Android arm64-v8a, NDK cross-compile) — provenance

Committed dist (D5 override, Windows convention): built once by the
`jxl_dist_android.yml` producer workflow and committed here with this
`PROVENANCE.md`, because Android cannot be rebuilt casually on any machine
in this project. This is **not** a per-CI-run build and **not**
fetch-at-build — see
`docs/logs/2026-08-31/plan-android-codec-full-green.md` §"D5".

Rendered by `native/scripts/build_deps.py build libjxl --platform android
--arch arm64-v8a --android-ndk <NDK root>` (`native/deps/manifest.toml`
`[component.libjxl.cmake.android]` overlay). Nothing here is hand-written
except this file — the lib/include/share tree below is the verbatim output
of that command.

## Pin mechanism (identical to the desktop `libjxl-dist`, only the toolchain differs)

Upstream libjxl has never published a release asset that is a source tarball
including the `third_party/highway` and `third_party/brotli` git submodules
the static build requires (checked via the GitHub Releases API across every
tag from v0.6 through v0.12.0, 2026-08-30, recorded in
`native/third_party/libjxl-dist/PROVENANCE.md`). The pin is therefore `git
clone` at a tagged commit plus selective `git submodule update --init` for
only the submodules the static core library links (brotli, highway,
skcms), with the full submodule SHA set recorded below.

- Tag: `v0.12.0`
- Commit: `a7a9c787341cf703dede03c2009fa460cae5e5df`
- Arch: `arm64-v8a` (Android ABI; `arch_map.toml [arm64-v8a]`,
  `ANDROID_PLATFORM=android-24`, NDK r27c)

### Submodule pins (identical across every platform — the pin is the tag, not the toolchain)

```
 028fb5a23661f123017c060daa546b55cf4bde29 third_party/brotli (028fb5a)
 457c891775a7397bdb0376bb1031e6e027af1c48 third_party/highway (457c891)
 96d9171c94b937a1b5f0293de7309ac16311b722 third_party/skcms (96d9171)
```

## Configure flags (`[component.libjxl.cmake.base]` + `[component.libjxl.cmake.android]`)

```
-DCMAKE_BUILD_TYPE=Release
-DCMAKE_INSTALL_PREFIX=native/third_party/libjxl-dist-android-arm64-v8a
-DCMAKE_POSITION_INDEPENDENT_CODE=ON
-DBUILD_SHARED_LIBS=OFF
-DBUILD_TESTING=OFF
-DJPEGXL_ENABLE_TOOLS=OFF
-DJPEGXL_ENABLE_BENCHMARK=OFF
-DJPEGXL_ENABLE_EXAMPLES=OFF
-DJPEGXL_ENABLE_FUZZERS=OFF
-DJPEGXL_ENABLE_DOXYGEN=OFF
-DJPEGXL_ENABLE_MANPAGES=OFF
-DJPEGXL_ENABLE_SJPEG=OFF
-DJPEGXL_ENABLE_OPENEXR=OFF
-DJPEGXL_ENABLE_SKCMS=ON
-DJPEGXL_ENABLE_JNI=OFF
-DJPEGXL_FORCE_SYSTEM_BROTLI=OFF
-DJPEGXL_FORCE_SYSTEM_HWY=OFF
-DCMAKE_TOOLCHAIN_FILE=<NDK root>/build/cmake/android.toolchain.cmake
-DANDROID_ABI=arm64-v8a
-DANDROID_PLATFORM=android-24
```

Highway targets AArch64 NEON via the NDK toolchain's own architecture
detection; no `HWY_` override is passed (plan Task 3 constraint: passing an
x86 `HWY_` flag would be silently wrong on this ABI).

## Licence and linkage

libjxl, brotli and skcms are BSD-3-Clause; highway (Google) is Apache-2.0.
All four are linked **statically** into `libdng_decoder_native.so` — same
static-linkage rationale as the desktop `libjxl-dist` (BSD-3/Apache-2.0
carry no relink duty, unlike the LGPL-3 heif-dist, which stays a separate
shared `.so` in `jniLibs/`). `-DJPEGXL_ENABLE_SKCMS=ON` is required, not a
style choice: `libjxl.a` leaves `JxlGetDefaultCms` undefined and only
`libjxl_cms.a` (built because of this flag) defines it — a consumer linking
`libjxl.a` without `libjxl_cms.a` fails at final link time, not at compile
time. Licence files for all four are vendored under
`share/licenses/{libjxl,highway,brotli,skcms}/`.

## Static libraries

`libjxl.a`, `libjxl_cms.a`, `libjxl_threads.a`, `libhwy.a`,
`libbrotlicommon.a`, `libbrotlidec.a`, `libbrotlienc.a` — release-built with
`CMAKE_BUILD_TYPE=Release`, cross-compiled for `arm64-v8a`/`android-24` with
NDK r27c (`nttld/setup-ndk@v1` pin), matching
`native/CMakePresets.json`'s `android-vulkan` preset ABI/platform exactly
(no drift between the dist and the consumer, `arch_map.toml [arm64-v8a]`).

## Assertions performed on this dist (mechanical, ELF-flavoured — see plan Task 3 constraints)

- All seven archives listed above present in `lib/`.
- **No symbol-presence assertion for the JXL public API.** `JXL_STATIC_DEFINE`
  makes `JXL_EXPORT` expand to nothing, so `JxlEncoderInitBasicInfo` can never
  appear in an export table regardless of correctness
  (`docs/logs/2026-08-31/r5-jxl-diagnosis.md`). The one archive-level check
  permitted here is against the raw archive symbol table (archive members are
  not hidden-visibility-affected the way a linked `.so`'s dynamic table is):
  `llvm-nm lib/libjxl_cms.a > cms.txt; grep JxlGetDefaultCms cms.txt` — the
  link-order trap made mechanical. The actual *capability* claim (JXL
  encode/decode genuinely works) is deferred to the desktop-only
  `codec_capability_probe.py` (Task 9); Android's probe reading is
  `NOT RUNNABLE IN CI` (D4, accepted coverage gap — foreign-arch artifact, no
  emulator) and is recorded as such in the coverage matrix, not asserted here.
- `share/licenses/{libjxl,highway,brotli,skcms}/` all non-empty.
- No `nm | grep -q` piping under `set -o pipefail` — every assertion captures
  tool output to a file first, then greps the file (lessons 2026-08-28).
- Exit codes are self-captured in the artifact via an immediately following
  `rc=$?`, never `${PIPESTATUS[0]}`, never a harness-reported code.

## Build provenance (filled in by the producer workflow run)

- Producer workflow: `.github/workflows/jxl_dist_android.yml` (owner: CI-T6).
- NDK: r27c, `nttld/setup-ndk@v1`.
- Run ID / measured wall-clock time: **TBD — filled in when the producer
  workflow first runs green** (D5: "Tasks 2/3/4 still record measured build
  times, now as the producer workflows' `timeout-minutes` evidence").
