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

## Assertions performed on this dist (mechanical, ELF-flavoured; actually run and
## GREEN against the archives below — supersedes an earlier draft of this
## section that described a check that was never wired up)

Implemented by `native/scripts/deps/android_dist.py` (`EXPECTATIONS["libjxl"]`),
invoked automatically by `build_deps.py build libjxl --platform android` as
the post-install step, in this order:

1. **Strip.** `android_dist.strip_archives()` runs the NDK's own
   `llvm-strip --strip-debug` (not `--strip-all`: global symbols and archive
   member `.symtab` entries survive, so every check below still has the
   symbols it reads) on all seven archives BEFORE any size/symbol assertion.
   Measured this build: `libjxl.a` **208,347,970 bytes (198.7 MB) unstripped
   → 12,402,098 bytes (11.8 MB) stripped** — the exact failure mode that
   rejected the original push at 199 MB against GitHub's 100 MB per-file
   limit, confirmed reproduced and confirmed fixed by this pipeline.
2. **Size tripwire.** Every archive in `lib/` is asserted `< 95 MB`
   (`SIZE_TRIPWIRE_MB`, 5 MB headroom under GitHub's 100 MB hard limit)
   *after* stripping. `libjxl.a` at 11.8 MB clears this with wide margin.
3. **Symbol presence, captured-to-file-then-grep (never `nm | grep -q`
   piped directly — 2026-08-28 lesson: under `pipefail`, a successful match
   makes `grep` exit before `nm` finishes writing, which SIGPIPEs `nm` and
   inverts the verdict).** `llvm-nm` output for every archive is written to
   `<dist>/.stage/assertions/<archive>.nm.txt` (build scratch, not part of
   the committed tree — see `.gitignore`), then grepped:
   - `lib/libjxl.a`: `JxlEncoderProcessOutput`, `JxlDecoderProcessInput`,
     `JxlEncoderAddBox` (same `fetch_libjxl.REQUIRED_SYMBOLS` the desktop
     carrier asserts — imported, not re-picked, so the two platforms can
     never assert a different capability set for the same pinned source).
     **This is a real, executed assertion**, contrary to an earlier draft of
     this file which claimed no symbol check was possible: `JXL_STATIC_DEFINE`
     only affects a linked `.so`'s dynamic export table, not a static
     archive's own `.symtab` — archive member object files are not subject
     to shared-library visibility hiding, so `nm -g` on the `.a` finds these
     three symbols `T`-defined. Verified this build: all three present.
   - `lib/libjxl_cms.a`: `JxlGetDefaultCms` — the link-order trap made
     mechanical (undefined in `libjxl.a`, defined only here; a consumer
     linking `libjxl.a` without `libjxl_cms.a` fails at final link).
   - `lib/libjxl_threads.a`, `lib/libhwy.a`, `lib/libbrotli{common,dec,enc}.a`:
     presence-only (empty required-symbol list) — dependencies `libjxl.a`
     needs at final link, not capability surfaces of their own.
4. **ELF machine check.** `llvm-readelf -h lib/libjxl.a` asserted to contain
   `AArch64` — catches a host build silently mislabelled as Android (one
   probe is enough: every archive in this dist comes out of one toolchain
   invocation).
5. **Licence non-empty.** `share/licenses/{libjxl,highway,brotli,skcms}/`
   each asserted to contain at least one file.
6. **`.pins` stamp**, written LAST (only after 1–5 all succeed, so a
   partially-built dist never carries a stamp claiming it is current):
   `libjxl=0.12.0:git:v0.12.0 arch=arm64-v8a abi=arm64-v8a ndk=27.2.12479018`.

No alignment/DT_NEEDED check applies here (unlike `heif-dist-android`'s
shared `.so`s): these are static archives with no LOAD segments and no
dynamic section of their own — `ANDROID_MIN_PAGE_ALIGN` / 16 KB page
alignment is a property of the final linked `.so` (`libdng_decoder_native.so`),
not of a static input to that link.

The *runtime capability* claim (JXL encode/decode genuinely works, not just
"the right symbols are present") is deferred to the desktop-only
`codec_capability_probe.py`; Android's probe reading is `NOT RUNNABLE IN CI`
(D4, accepted coverage gap — foreign-arch artifact, no emulator) and is
recorded as such in the coverage matrix, not asserted here.

## Build provenance

- Producer entry point actually run: `native/scripts/build_deps.py build
  libjxl --platform android --arch arm64-v8a --android-ndk <NDK root> --dist
  native/third_party/libjxl-dist-android-arm64-v8a` — the same command
  `.github/workflows/jxl_dist_android.yml` invokes, run LOCALLY in a
  `linux/amd64` docker container (task PODMAN-SIM-ANDROID's gate
  infrastructure, `native/scripts/tmp/cisim/android/`) mirroring that
  workflow's base image, apt packages, and pinned NDK r27c, per the
  user-decreed AC8 method: build and validate locally FIRST, commit only
  after local validation passes. **This dist has NOT yet been produced by an
  actual GitHub Actions run of `jxl_dist_android.yml`** — that remains a
  follow-up (the workflow exists and this local build used its exact
  toolchain/command, but "local docker run" and "GitHub-hosted runner run"
  are not asserted identical beyond both being `ubuntu`-family `linux/amd64`
  with the same explicitly-installed package set; see cisim's own
  DIVERGENCES notes for what a local docker run does not guarantee about a
  GitHub-hosted runner).
- NDK: r27c (`27.2.12479018` per the `.pins` stamp), same revision
  `nttld/setup-ndk@v1` would pin in CI.
- Local build wall-clock: not separately measured (this was a manual
  iterative validation run, not a timed producer run); CI's
  `timeout-minutes: 45` budget for the actual workflow leg is unaffected by
  this local run.
