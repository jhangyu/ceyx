# HEIF distribution (Android arm64-v8a, NDK cross-compile) — provenance

Produced by the Python dist carrier, never by hand:

```
python3 native/scripts/build_deps.py build heif-stack \
    --platform android --arch arm64-v8a \
    --android-ndk "$ANDROID_NDK_HOME" \
    --dist native/third_party/heif-dist-android-arm64-v8a
```

Run in CI by `.github/workflows/heif_dist_android.yml` (dispatch/`workflow_call`
producer, NDK pinned with `nttld/setup-ndk@v1`), and the resulting tree is
**committed** to this repository. That placement is a user ruling
(`codec-plans-user-rulings.md`, D5): Android, like Windows, cannot be rebuilt
casually on the development machine, so the dist is built once, reviewed, and
committed with this file — not rebuilt per CI run and not fetched at build time.

## Scope ruling (D1)

**Full parity, option 1** — HEIC encode+decode and AVIF encode+decode, the HEVC
encoder (kvazaar) included. Decided by the user on 2026-08-31
(`docs/logs/2026-08-31/codec-plans-user-rulings.md`), on a stated
personal/non-commercial basis. The exposure being accepted is explicitly
HEVC **encode** inside a distributed mobile application: kvazaar's BSD-3 licence
grants no patent peace, and HEVC is covered by active pools (MPEG LA / Access
Advance / Velos). Options 2 (AV1-only) and 3 (decode-only) were declined.

Only this file may record that ruling for this dist. If the flag set below ever
stops matching it, the dist and the coverage matrix have silently disagreed and
one of them is lying.

## Components

| Component | Version | Acquisition | Pin |
|---|---|---|---|
| libheif | 1.23.2 | tarball | `8bd5d41d19dc84536d118b04774709f244df6104ef66d623dad5fa4650143405` |
| libde265 | 1.1.1 | tarball | `fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219` |
| kvazaar | 2.3.1 | **git clone**, tag `v2.3.1` | tag (see below) |
| libaom | 3.15.0 | tarball | `ea08c38ecc078bc85bb1b691020e52b06250f1a81fe7ca5b624629225081af96` |

Every value above is read from `native/deps/manifest.toml`'s **android** source
blocks by the carrier; this table is a transcription for readers, not a second
source of truth.

Three acquisition facts that are not obvious and are load-bearing:

1. **libde265 and aom are built from source here, unlike on macOS/Linux**, where
   both come from vcpkg. There is no android triplet in `native/vcpkg/` and
   vcpkg is never invoked on the android leg, so the registry route does not
   exist on this platform. libde265's tarball is byte-identical to the release
   the vcpkg overlay port pins, so the android and desktop dists are the same
   upstream code.
2. **kvazaar is a git clone, not the release tarball.** The 2.3.1 release
   tarball omits `src/threadwrapper/src/pthread.cpp`. That file is only added to
   the target when `WIN32` is true, so it would probably not have blocked this
   build — the clone is used on the plan's explicit instruction so both
   cross-built legs (Windows, Android) share one acquisition path, and because
   the tagged tree is a superset of the tarball. Stated plainly rather than
   dressed up as a build blocker it is not.
3. **libaom is 3.15.0, not the 3.12.1 the plan's prose names.** The component
   was bumped to 3.15.0 by the vcpkg migration, and the manifest schema carries
   exactly one `version` per component (it drives both the URL and the extracted
   directory name). Pinning 3.12.1 for android alone would need a per-platform
   version mechanism that does not exist and would put Android's AV1 codec on a
   different release from every other platform. **The SHA-256 above has no
   upstream-published counterpart** (the same gap the 3.12.1 pin had): it was
   obtained by downloading that exact URL on 2026-09-01 and hashing the bytes,
   and every later fetch is verified against it. Trust-on-first-use, recorded as
   such rather than presented as an upstream fact.

## Android specifics

- **ABI**: `arm64-v8a` only (D3). **API level**: `android-24`, taken from
  `native/deps/arch_map.toml [arm64-v8a]`, the same table
  `native/CMakePresets.json`'s `android-vulkan` preset is aligned with — the
  dist and its consumer must not drift apart on API level.
- **NDK**: r27c, pinned by the producer workflow, never the runner image's
  preinstalled copy. The NDK's recorded `Pkg.Revision` is part of the `.pins`
  stamp: two dists with identical component pins built by different NDKs are
  not interchangeable.
- **Unversioned SONAMEs.** The shipped files are `lib/libheif.so` and
  `lib/libde265.so`, *not* the desktop `libheif.so.1` / `libde265.so.0`
  spellings the plan's prose named. Android packaging only ships files whose
  name ends in `.so`, and the loader resolves `DT_NEEDED` against
  `lib/<abi>/` by exact file name — a versioned SONAME builds, links and
  installs perfectly on the build machine and fails only on a device, which is
  the one place this project has no instrument. The NDK toolchain file sets
  `CMAKE_PLATFORM_NO_VERSIONED_SONAME`; the carrier does not take that on trust
  and reads the recorded SONAME back out of each ELF.
- **PIC is mandatory** on kvazaar and aom (`CMAKE_POSITION_INDEPENDENT_CODE=ON`
  in their android overlays): they are static archives linked into a shared
  library, and without PIC the link fails with a relocation error that names
  **libheif**, sending the reader to the wrong file.
- **No `AOM_TARGET_CPU` override**, for the same reason Linux has none: the NDK
  toolchain sets `CMAKE_SYSTEM_PROCESSOR=aarch64` and aom normalises that
  spelling through its own branch, so an explicit override would not match its
  later string comparisons. Whether aom actually detected an arm64 target is a
  configure-log read (a generic C fallback would be a silent ~10× performance
  loss, not a failure) — see "Producer run" below.

## Licence and linkage

libheif and libde265 are **LGPL-3.0-or-later** and ship as **separate shared
libraries**, staged into `plugin/android/src/main/jniLibs/arm64-v8a/`. Inside an
APK that placement is what satisfies LGPL-3 §4(d)(1): the `.so` sits in
`lib/arm64-v8a/` of the archive and a user can replace it and repack. Static
linking into `libdng_decoder_native.so` is deliberately NOT done — it would
trigger the §4(d)(0) duty to publish relinkable object files with **every**
release, a permanent release-process obligation rather than a one-off.

kvazaar (BSD-3-Clause) and libaom (BSD-2-Clause **plus the separate Alliance for
Open Media Patent License 1.0**) are STATIC archives linked INTO `libheif.so`,
because `ENABLE_PLUGIN_LOADING=OFF` — a `dlopen`-ed plugin directory does not
survive APK packaging. Neither is copyleft, so this adds no source-availability
duty beyond the LGPL-3 one above.

`WITH_X265=OFF`: x265 is GPL-2.0 and would contaminate the shipped APK. Its
**absence is asserted against the built artefact's symbol table**, not merely
passed as a flag.

Licence files are vendored under `share/licenses/<component>/`, copied from each
source tree **before** the stage directory is deleted. `share/licenses/aom/`
must contain a `PATENTS*` file: the AOM Patent License 1.0 is a separate grant
on top of BSD-2, and shipping `LICENSE` alone leaves that duty unmet. The
carrier fails the build if it is missing.

## Configure flags (encode-enabled build)

Rendered from `native/deps/manifest.toml`; inspect the exact argv without
building:

```
python3 native/scripts/build_deps.py build heif-stack --platform android \
    --arch arm64-v8a --android-ndk "$ANDROID_NDK_HOME" \
    --dist native/third_party/heif-dist-android-arm64-v8a --dry-run
```

libheif's capability flags: `WITH_LIBDE265=ON`, `WITH_KVAZAAR=ON`,
`WITH_AOM_DECODER=ON`, `WITH_AOM_ENCODER=ON`, `WITH_X265=OFF`,
`WITH_X264=OFF`, `WITH_DAV1D=OFF`, `WITH_RAV1E=OFF`, `WITH_SvtEnc=OFF`,
`ENABLE_PLUGIN_LOADING=OFF`, `WITH_EXAMPLES=OFF`, `BUILD_TESTING=OFF`,
`BUILD_SHARED_LIBS=ON`, plus the NDK trio
(`CMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake`,
`ANDROID_ABI=arm64-v8a`, `ANDROID_PLATFORM=android-24`).

**Build order is load-bearing, not stylistic**: libde265 → kvazaar → aom →
libheif. libheif's `FindLIBDE265` does a `find_library()` against a file on
disk, and its `Findkvazaar`/`FindAOM` probes read the install prefix. All three
probes are **non-fatal**: a configure with any of these inputs missing SUCCEEDS
and the capability silently disappears. That is why the carrier checks the input
files exist on disk *before* configuring and checks the codec symbols in the
output *afterwards* — the flags we passed are not evidence of anything.

## Assertions

Run by `native/scripts/deps/heif.py` at the `assemble` stage, against the
artefacts, every one of them demonstrated red in
`native/scripts/deps/tests/test_heif.py`:

| Assertion | Instrument | Failure it catches |
|---|---|---|
| `heif_decode_image` present | `llvm-nm -D libheif.so` | decode API missing |
| `heif_context_get_encoder_for_format` present | `llvm-nm -D libheif.so` | dist silently built decode-only |
| `kvz_api_get` present | `llvm-nm libheif.so` (full table) | `WITH_KVAZAAR` did not take effect — HEIC encode dead |
| `aom_codec_av1_cx` present | `llvm-nm libheif.so` (full table) | AVIF **encode** dead |
| `aom_codec_av1_dx` present | `llvm-nm libheif.so` (full table) | AVIF **decode** dead (independent flag from the encoder) |
| `x265_encoder` **absent** | both tables | GPL-2.0 contamination |
| `libde265` in `DT_NEEDED` | `llvm-readelf -d libheif.so` | libheif built without a working HEVC decoder |
| SONAME is `libheif.so` / `libde265.so` | `llvm-readelf -d` | versioned or path-bearing name — a device-only load failure |
| AArch64 ELF64 | `llvm-readelf -h` | toolchain file silently ignored, host objects shipped |
| licences + `PATENTS*` vendored | file presence | unmet attribution duty |

Two mechanical disciplines, both from prior incidents in this repository:

- **Capture, then search.** No `nm | grep -q`: under `set -o pipefail` grep
  exits at its first match, `nm` dies of SIGPIPE, the pipeline reports 141, and
  the check fails *because* the symbol is present. The carrier reads each dump
  into a variable via an argv list (`deps/run.py` refuses any argv element
  containing a pipe), so the inversion is structurally impossible.
- **Two symbol tables.** `WITH_REDUCED_VISIBILITY=ON` keeps the merged
  kvazaar/aom symbols out of `.dynsym`; they live in `.symtab`. A dynamic-only
  dump would report a present capability as absent, and — worse — would let a
  forbidden symbol hide in the table nobody read. Presence is checked against
  the union; absence against every table.

**These are static tripwires, not capability claims.** A capability probe must
*execute* the library. The artefact is a cross-compiled AArch64 ELF, the CI
runner is x86_64, and the emulator that could have run it was removed by the
2026-08-31 compile-only ruling. Android's capability probe is therefore
**NOT RUNNABLE IN CI — an accepted coverage gap**, and no host-architecture
proxy is substituted for it: a same-source x86_64 build is a different binary
and would measure the Linux leg.

## What is committed here, and what is not

Ruled 2026-09-01 (A-T4 Finding 3, option c). The build produces more than the
consumer needs, and this tree is committed to git and consumed by path, so the
committed set is deliberately smaller than the built set:

| Committed | Excluded (`.gitignore` in this directory) |
|---|---|
| `include/` | `bin/` — aomenc (52.8 MB), aomdec (48.9 MB), kvazaar, dec265 |
| `lib/libheif.so`, `lib/libde265.so` (stripped) | `lib/*.a` — libaom.a, libkvazaar.a |
| `share/licenses/`, `share/provenance/`, `.pins` | `lib/pkgconfig/`, `lib/cmake/`, `.stage/` |

Reasons, not preferences: the command-line tools never run inside an APK; the
static archives are build INPUTS already merged into `libheif.so`, so keeping
them would commit the same code twice; and `libaom.a` measured **99,554,482
bytes** on CI run 33460559016 — about 450 KB below GitHub's 100 MB per-file
hard limit. That is luck, not headroom: the same wall rejected a sibling
android dist push outright. Everything excluded is reproducible by re-running
the producer workflow against the pins recorded above.

**Stripping.** An NDK cross-build embeds full debug info by default
(`CMAKE_BUILD_TYPE=Release` does not strip it): the first green run shipped
`libheif.so` at 90.5 MB "with debug_info, not stripped". The carrier now runs
`llvm-strip --strip-debug` over the artefacts **before** the assertions, so the
checks read the bytes that actually ship rather than a richer intermediate.
`--strip-debug`, never `--strip-all`: it removes debug sections only, leaving
`.symtab`, `.dynsym`, the SONAME and `DT_NEEDED` intact — every assertion below
still has its evidence, and a full strip would silently weaken the capability
checks to a `.dynsym`-only test without any of them going red.

## 16 KB page alignment

Android 15+ devices may use 16 KB pages, and a library whose LOAD segments are
aligned to 4 KB does not load there. Nothing in this repository handled page
alignment before this dist, and the handoff note that claimed a particular NDK
r27c default was never verified — so the carrier **measured** it with
`llvm-readelf -l` on every produced `.so`, writing the result to
`share/provenance/android_so_alignment.txt`, committed beside the binaries it
describes.

**The measurement falsified the handoff note.** CI run 33460559016 produced
both libraries with `p_align 0x1000` (4096) on *every* LOAD segment:

```
libheif.so  LOAD alignments: 0x1000 0x1000 0x1000
libde265.so LOAD alignments: 0x1000 0x1000 0x1000
```

Those binaries would not have loaded on a 16 KB-page device, and this project
runs nothing on a device or emulator that would ever have noticed. Both android
overlays therefore carry `CMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384`,
and the measurement has been promoted from a record to an **assertion**
(`ANDROID_MIN_PAGE_ALIGN` in `deps/heif.py`): the build now fails if any LOAD
segment is below 16384, so the flag cannot silently stop taking effect. Passing
a linker flag and the artefact carrying its effect are different facts. The
report file is still written before the assertion fires, so a failing run
leaves its numbers behind to read.

This is plan F5's ordering honoured exactly: measure first, gate on the
measured value second — never assert a remembered default.

## Producer run

Filled from the run that produced the committed binaries. Every value below was
either printed by the run or re-measured by hand on the downloaded artefact —
never copied from the step's green conclusion.

- **Workflow run**: <https://github.com/jhangyu/ceyx/actions/runs/33464077144>,
  head SHA `ecdc7262be814d60453257c935e7e6442169fc02`.
- **`HEIF_DIST_ANDROID_RC`: 0**, echoed by the step itself on the line
  immediately after the command (never a harness-reported status).
- **Wall clock**: build step 02:51:55Z → 02:54:49Z (2 min 54 s); whole job
  3 min 44 s. Well inside the workflow's `timeout-minutes: 90`.
- **Three earlier runs failed**, and the failures are part of this provenance
  because each one was a real defect this dist would otherwise have shipped:
  33454839588 (kvazaar's x86 assembler flag on aarch64, misreported by CMake as
  "Could NOT find Threads"), 33457406073 (the carrier's output-candidate list
  had no unversioned android spelling), 33460559016 (green, but produced
  4 KB-aligned libraries — see the alignment section).

**aom really targeted arm64**, not a generic C fallback: its configure ran
`arm_feature_flag_neon_available`, `arm_crc32`, `neon_dotprod` and `neon_i8mm`
probes, and the produced `libaom.a` carries 8171 NEON symbols. This is the check
the manifest's "no `AOM_TARGET_CPU` override" note defers to.

**Strip, measured before → after:**

| Artefact | Unstripped | Stripped |
|---|---|---|
| `libheif.so` | 90,472,744 | 10,480,104 |
| `libde265.so` | 5,019,008 | 1,711,936 |
| `libaom.a` (not committed) | 99,554,482 | 10,468,842 |
| `libkvazaar.a` (not committed) | 3,891,174 | 813,262 |

The whole committed tree is now ~12.9 MB.

**LOAD `p_align` (the 16 KB gate), full text in
`share/provenance/android_so_alignment.txt`:**

```
libheif.so  LOAD alignments: 0x4000 0x4000 0x4000
libde265.so LOAD alignments: 0x4000 0x4000 0x4000
```

0x4000 = 16384 on every segment of both libraries, up from 0x1000 before the
linker flag. Asserted, not merely recorded.

**`DT_NEEDED` of `libheif.so`** — `libde265.so`, `libm.so`, `libdl.so`,
`libc.so`. Every entry is either shipped in this dist or supplied by the Android
platform; nothing else is required at load time. Note there is no
`libc++_shared.so`: both libraries link libc++ statically, so this dist does not
impose an STL dependency on its consumer.

**SONAMEs** — `libheif.so`, `libde265.so`. Unversioned and path-free, as APK
packaging requires.

**Assertion transcript, as printed by the run:**

```
ASSERT ok      libde265.so SONAME is libde265.so
STRIP          libheif.so: 90472744 -> 10480104 bytes
STRIP          libde265.so: 5019008 -> 1711936 bytes
STRIP          libkvazaar.a: 3891174 -> 813262 bytes
STRIP          libaom.a: 99554482 -> 10468842 bytes
ASSERT ok      present in libheif: heif_decode_image
ASSERT ok      present in libheif: heif_context_get_encoder_for_format
ASSERT ok      present in libheif: kvz_api_get
ASSERT ok      present in libheif: aom_codec_av1_cx
ASSERT ok      present in libheif: aom_codec_av1_dx
ASSERT absent  correctly not in libheif: x265_encoder
ASSERT ok      libheif records a libde265 runtime dependency
ASSERT ok      libheif.so is a 64-bit AArch64 ELF (arm64-v8a)
ASSERT ok      libde265.so is a 64-bit AArch64 ELF (arm64-v8a)
ASSERT ok      libheif.so SONAME is libheif.so
MEASURE        libheif.so LOAD p_align: 0x4000 0x4000 0x4000
MEASURE        libde265.so LOAD p_align: 0x4000 0x4000 0x4000
ASSERT ok      libheif.so LOAD segments are >= 16384-byte aligned
ASSERT ok      libde265.so LOAD segments are >= 16384-byte aligned
ASSERT ok      libheif licences vendored: ['COPYING']
ASSERT ok      libde265 licences vendored: ['COPYING']
ASSERT ok      kvazaar licences vendored: ['LICENSE', 'LICENSE.EXT.greatest']
ASSERT ok      aom licences vendored: ['LICENSE', 'PATENTS']
ASSERT ok      aom patent grant vendored: ['PATENTS']
```

**Independent re-verification of the shipped bytes** (done on the downloaded
artefact with a host `objdump`, *after* stripping, so it measures what is
committed here rather than what the build claimed): `heif_decode_image`,
`heif_context_get_encoder_for_format`, `kvz_api_get`, `aom_codec_av1_cx` and
`aom_codec_av1_dx` all present in both `.dynsym` and `.symtab`; `x265_encoder`
and `x264_encoder` return **zero** matches in either table.

**`.pins` stamp** (names every component, the ABI and the NDK revision, so a
dist missing an encoder can never match):

```
libheif=1.23.2:8bd5d41d19dc84536d118b04774709f244df6104ef66d623dad5fa4650143405 libde265=1.1.1:fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219 kvazaar=2.3.1:git:v2.3.1 aom=3.15.0:ea08c38ecc078bc85bb1b691020e52b06250f1a81fe7ca5b624629225081af96 arch=arm64-v8a abi=arm64-v8a ndk=27.2.12479018
```

**What this evidence does NOT establish.** Nothing here executes the library.
Every check above is a static tripwire on a foreign-architecture ELF; the
capability probe is NOT RUNNABLE IN CI on Android and no host-architecture proxy
was substituted. HEIC/AVIF encode and decode are evidenced by symbol presence,
libheif's own backend selection, and this provenance — not by a decode.
