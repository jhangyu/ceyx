# LibRaw + RawSpeed3 vendor provenance

Fetched by `dng_processor/native/scripts/fetch_libraw_dist.sh`. The vendored
source tree is untracked (same policy as `third_party/halide`); this file is
the tracked record. Each component's `.git` directory is stripped after
fetch (see `strip_git()` in the fetch script) so this directory can hold a
normal tracked file; the resolved revision is instead recorded in a
`.vendor-rev` sidecar file per component, which `verify_raw_provenance.py`
reads.

| Component | Source | Revision | License |
|---|---|---|---|
| LibRaw | https://github.com/LibRaw/LibRaw.git | df226ea4178ccd74245f4f13c23adddfa01411c9 | LGPL-2.1 (elected; CDDL-1.0 also offered upstream) |
| RawSpeed | https://github.com/darktable-org/rawspeed.git | c835b05aecfacb7343f7c424abd620aa12116c3f | LGPL-2.1 |
| LibRaw-cmake | https://github.com/LibRaw/LibRaw-cmake.git | eb98e4325aef2ce85d2eb031c2ff18640ca616d3 | MIT |
| pugixml | bundled by RawSpeed at the above revision | (as vendored) | MIT |
| zlib | project third_party | (existing project vendoring) | zlib |
| libjpeg-turbo | project third_party | (existing project vendoring) | IJG / BSD-3 |

## Revision pin substitutions from the original plan

> **Superseded for RawSpeed by Phase 19 W1.** The section below records the
> Phase 17 decision to fall back from `c835b05a` to `de70ef5f`. Phase 19 moved
> the pin forward to `c835b05a` after re-porting the patch set; see
> "RawSpeed3 re-pin (Phase 19 W1)" below. The Phase 17 reasoning is kept
> because it explains why the patch set needed re-porting at all.

The implementation plan (`docs/superpowers/plans/2026-08-24-multi-raw-halide-pipeline.md`,
section 14) pinned RawSpeed at `c835b05aecfacb7343f7c424abd620aa12116c3f`. At that
revision, `RawSpeed3/patches/01.CameraMeta-extensibility.patch` (from the LibRaw
distribution above) fails to apply — `git apply --check` reports the expected
`class CameraMetaData {` context is absent (the file already reads
`class CameraMetaData final {` at that revision from unrelated upstream changes,
and the surrounding context has diverged further).

LibRaw's own `RawSpeed3/README.md` (vendored, see above) documents the
patch set was authored against RawSpeed commit
`de70ef5fbc62cde91009c8cff7a206272abe631e`. At that revision:
- `01.CameraMeta-extensibility.patch` does not apply forward (same "already
  final" mismatch), but *reverse*-applies cleanly (`git apply --check
  --reverse` exits 0). The patch's purpose (per its name and the LibRaw
  README: "allows derived classes from CameraMeta") is to make
  `CameraMetaData` non-final so `rawspeed3_capi.cpp`'s
  `CameraMetaDataFromMem : public CameraMetaData` can compile; at this
  revision the class already reads `final` (the patch's post-state), so the
  fetch script applies it **in reverse** to reach the actually-required
  pre-state (no `final`). This is implemented in `fetch_libraw_dist.sh`
  (the "already applied" branch actually runs `git apply --reverse`, not a
  no-op skip — an earlier version of this script had that bug, fixed before
  first commit).
- `02.Makernotes-processing.patch`, `03.remove-limits-and-logging.patch`,
  `04.clang-cl-compatibility.patch`, `05.no-phase-one-correction.patch` all
  apply forward cleanly with zero fuzz.

`RAWSPEED_REV` was therefore pinned to `de70ef5fbc62cde91009c8cff7a206272abe631e`
instead of the plan's original value. This follows the plan's own documented
contingency ("If any patch does not apply with zero fuzz... pin a RawSpeed
revision at which the patch set applies cleanly and record the reason in
PROVENANCE.md"). Approved by team-lead 2026-08-24.

## Missing LibRaw CMake build

LibRaw at the pinned revision (`df226ea4...`) ships no `CMakeLists.txt` of
its own. `README.cmake` in the vendored tree states: "Due to inability to
support (user contributed) Cmake scripts, the cmake builds are not
officially supported by LibRaw team since October 23, 2014. The scripts are
moved to separate github repository github.com:LibRaw/LibRaw-cmake.git".

This project therefore vendors `LibRaw/LibRaw-cmake` (community-maintained
overlay) as a third pinned dependency, pointed at our vendored LibRaw source
tree via the `LIBRAW_PATH` CMake variable (no files are copied into the
LibRaw tree). Approved by team-lead 2026-08-24 (option a of three
alternatives: vendor the overlay vs. hand-write our own CMakeLists.txt vs.
wrap LibRaw's autotools build — autotools was rejected because it has no
Windows/Android path, required by spec section 6.6's five-platform release
gate; hand-writing was rejected because LibRaw-cmake already correctly
maintains the exact source-file enumeration).

Header layout note: the correct header path exported by this pin is
`third_party/libraw/libraw/libraw.h` (no `include/` subdirectory).

## Applied patches

Hashes below are for the RawSpeed3 patch set as **re-ported by Phase 19 W1**
against RawSpeed commit `c835b05aecfacb7343f7c424abd620aa12116c3f`. All four
surviving patches apply **forward** at this pin (no reverse case remains, so
`verify_raw_provenance.py`'s `REVERSE_APPLIED_PATCHES` is empty).
`04.clang-cl-compatibility.patch` is **dropped (upstream)** and is therefore
absent from the table. See "RawSpeed3 re-pin (Phase 19 W1)" below.

The re-ported patch files are project-owned and live in
`dng_processor/native/patches/rawspeed3/`; `fetch_libraw_dist.sh` overlays them
onto `RawSpeed3/patches/` after cloning LibRaw, replacing LibRaw's originals
wholesale. They cannot be stored only in `RawSpeed3/patches/` because that
directory sits inside the LibRaw source tree and is wiped and restored to
LibRaw's originals by every LibRaw re-clone.

| Patch | SHA-256 |
|---|---|
| 01.CameraMeta-extensibility.patch | 2138a5221522011662097a21acc7b8abc792e1a2ad3f5772cb1c1dcb262070b1 |
| 02.Makernotes-processing.patch | ce6245bbc12a8493add42444c51a4217bd177ff203c40f18cd610ec539d717fc |
| 03.remove-limits-and-logging.patch | dea6e247b55b52ef97112fcc791a581b9f568c7d76a87f03f98216034c53e14b |
| 05.no-phase-one-correction.patch | 7d972f23760337b48b36ba8d7b913fee1c0821bc9254b530690955be1a3ea24a |

## Project-authored LibRaw patches

Rooted at this LibRaw tree, applied by `scripts/fetch_libraw_dist.sh` after
LibRaw's own RawSpeed3 patch set. Rationale and target files:
`dng_processor/native/patches/libraw/README.md`.

Phase 19 W1 motivation: LibRaw gates its RawSpeed3 branch on
`LIBRAW_DECODER_TRYRAWSPEED3` (`src/decoders/unpack.cpp:124`) and the two Fuji
branches (`src/utils/decoder_info.cpp:63-69`) set no flags, so no RAF was ever
offered to RawSpeed3. Patch 06 sets the flag on those two decoders only; every
other decoder keeps LibRaw's upstream flag choices.

Measured gate inputs on the corpus RAF files (before/after patch 06, recorded
in `dng_processor/native/scripts/tmp/p19/t1_eligibility.txt`):
- fuji_xt3.raf: `fuji_width=0 filters=9 decoder=fuji_compressed_load_raw()` —
  before: `backend=libraw_native`; after: see POST-PATCH section of the
  eligibility file.
- fuji_xt5.raf: `fuji_width=0 filters=9 decoder=fuji_compressed_load_raw()` —
  before: `backend=libraw_native`; after: see POST-PATCH section of the
  eligibility file.

Both samples measured `fuji_width == 0`, so the `(!IO.fuji_width)` clause in
`src/decoders/unpack.cpp:117` never blocks these files at the current
RawSpeed3 pin `de70ef5f`; patch 07 (conditional, would relax that clause for
`filters == 9` X-Trans) was **not created**.

Patch 07 (Phase 19 Task 2) adapts LibRaw's RawSpeed3 C-API shim to the
re-pinned RawSpeed3 API; see "RawSpeed3 re-pin (Phase 19 W1)" below. Note this
is a different patch 07 from the one contemplated (and not created) above: the
`(!IO.fuji_width)` clause still never blocks these files, so no fuji-rotated
gate patch exists.

| Patch | SHA-256 |
|---|---|
| 06.fuji-tryrawspeed3.patch | 4da0ea93cbbb46aef4d52830612eb67212ee549e9f4aedacc572b99d6afa041f |
| 07.rawspeed3-capi-repin.patch | 5fdd081ed321538f3d4b2d14ab77c428bda0a02e114c4f37037b208988049a0a |

## RawSpeed3 re-pin (Phase 19 W1)

The Phase 17 pin `de70ef5fbc62cde91009c8cff7a206272abe631e` (2021-09-10)
predates the Fujifilm X-T5, so its `cameras.xml` cannot claim that body and
RawSpeed3 declined the file regardless of LibRaw's dispatch flags. Phase 19
re-pins RawSpeed3 to upstream develop at
`c835b05aecfacb7343f7c424abd620aa12116c3f` (2026-07-28), which carries
`RafDecoder` + `FujiDecompressor` and compressed X-T3/X-T5 entries
(`RawSpeed3/rawspeed/data/cameras.xml:16511` plain and `:16530` `mode="compressed"`).

LibRaw declares its five patches commit-specific (`RawSpeed3/README.md`). All
five failed to apply at the new pin (`git apply --check` in both directions;
captured in `scripts/tmp/p19/t2_patch_status_pre.txt`). Re-port outcome:

| Patch | Direction at de70ef5f | Direction at c835b05a | Adjustment |
|---|---|---|---|
| 01.CameraMeta-extensibility.patch | reverse | forward | Re-ported to a new blocker. Upstream **adopted the patch's intent**: `metadata/CameraMetaData.h:46` now reads `// NOTE: *NOT* `final`, could be derived from by downstream.` and line 47 is `class CameraMetaData {`, so the `final` half is obsolete. But upstream moved `addCamera()` into a `private:` section (`CameraMetaData.h:76-77`), which blocks `rawspeed3_capi.cpp`'s `CameraMetaDataFromMem : public CameraMetaData` exactly as `final` used to (build errors at `rawspeed3_capi.cpp:241` and `:248`). The re-ported patch widens that `private:` to `protected:` — same purpose (subclass extensibility), new blocker. |
| 02.Makernotes-processing.patch | forward | forward | Re-ported: intent unchanged (skip `LSI1` makernotes; bail to an empty IFD when the declared IFD size exceeds the entry's byte count). Context drift plus one API change — `ByteStream::hasPrefix()` now takes a `std::string_view` (`io/ByteStream.h:145`), so the `("LSI1\0", 5)` / `("AOC\0", 4)` literals are wrapped in `std::string_view(...)`. The `UINT32_MAX`-offset "virtual/empty IFD" contract the patch depends on is still honoured (`tiff/TiffIFD.cpp:114-117`). |
| 03.remove-limits-and-logging.patch | forward | forward | Re-ported: same 15 files / 19 sites as at `de70ef5f`, context drift only. Two API renames absorbed: the log priority enum is now scoped (`DEBUG_PRIO::INFO` / `DEBUG_PRIO::EXTRA`), and several decompressor guards now spell the zero-area test `!mRaw->dim.hasPositiveArea()`. Scope deliberately **not** widened: upstream has added further max-dimension sites since 2021 (45 `Unexpected image dimensions found` sites tree-wide vs 19 here); a re-port preserves the original patch's scope rather than extending it. |
| 04.clang-cl-compatibility.patch | forward | **dropped (upstream)** | Both hunks are fixed upstream. Hunk 1 worked around `typename = std::enable_if_t<std::is_pod<T>::value>` as a defaulted template parameter; upstream deleted that parameter entirely — the template now reads `template <typename T, typename ActualAllocator = std::allocator<T>>` (`adt/DefaultInitAllocatorAdaptor.h:28-29`, also moved from `common/`), and `std::is_pod` no longer occurs anywhere in the tree (`grep -rn 'is_pod' src/librawspeed/` → no matches). Hunk 2 worked around comparing a pointer with a container iterator (`&wavelet == channel.wavelets.begin()`, ill-formed on MSVC's class-type iterators); upstream now writes `&wavelet == &*channel.wavelets.begin()` (`decompressors/VC5Decompressor.cpp:420-421`), a pointer-to-pointer comparison that is valid on every implementation. |
| 05.no-phase-one-correction.patch | forward | forward | Re-ported: context drift only. The double-correction hazard is unchanged — `IiqDecoder::CorrectPhaseOneC` still exists (`decoders/IiqDecoder.cpp:282`) and is still called unconditionally after decompression (`:271-272`); the patch compiles that call out so LibRaw's own PhaseOne correction is not applied twice. |

C-API shim adaptation: one build error required a change to the vendored
`rawspeed3_capi.cpp`, delivered as project patch
`patches/libraw/07.rawspeed3-capi-repin.patch` (the vendored file is not edited
in place):
- `error: no member named 'getDataUncropped' in 'rawspeed::RawImageData'`
  (`rawspeed3_capi.cpp:181`). Upstream removed `getDataUncropped(x, y)` in the
  Array2DRef refactor; the equivalent uncropped byte view is
  `getByteDataAsUncroppedArray2DRef()` (`common/RawImage.h:135-136`, dispatching
  at `RawImage.h:330-339`), so the call becomes
  `&(r->getByteDataAsUncroppedArray2DRef()(0, 0))`, the same address as before.
- The two `error: 'addCamera' is a private member` errors
  (`rawspeed3_capi.cpp:241`, `:248`) are resolved by the re-ported patch 01
  above, not by a shim change.
- No `CMakeLists.txt` change was needed: the `rawspeed` and
  `rawspeed_get_number_of_processor_cores` targets, the `pugixml` target and
  its `SOURCE_DIR` property, and `rsxml2c.sh` all still exist at the new pin.

`rawspeed3_c_api/cameras.cpp` regenerates at configure time from the new pin's
`data/cameras.xml` (`CMakeLists.txt` `execute_process` calling `rsxml2c.sh`), so
a configure-inclusive build is required after any re-pin. Verified: the
regenerated `build/rawspeed3-cameras/cameras.cpp` contains 8 `X-T5` and 16
`X-T3` occurrences.

Measured backend on the corpus RAF files after the re-pin:
`dng_processor/native/scripts/tmp/p19/t2_eligibility.txt`. `fuji_xt3.raf` reads
`backend=rawspeed3`; `fuji_xt5.raf` still reads `backend=libraw_native` even
though the body is now in `cameras.xml` and the regenerated camera table. This
is a recorded finding for Phase 19 Task 3, not a re-pin failure — the re-pin's
success criteria are the build and the gates. Leading (but **unverified**)
hypothesis: LibRaw throws `"Size mismatch"` when RawSpeed's reported dimensions
differ from its own (`src/decoders/unpack.cpp:170-171`) and silently falls back
to the native decoder; discriminating that from
`LIBRAW_WARN_RAWSPEED3_NOTLISTED` needs the `process_warnings` bits, which
`libraw_smoke` does not yet print.

## Local modifications

**RawSpeed3 C-API glue is not present in the LibRaw-cmake overlay** (that
overlay only supports the legacy RawSpeed v1 codec path via
`ENABLE_RAWSPEED`, not the bundled RawSpeed3). Rather than patch the
overlay, `dng_processor/native/CMakeLists.txt` extends the overlay's `raw`
target in place, after `add_subdirectory`, with:
- `RawSpeed3/rawspeed3_c_api/rawspeed3_capi.cpp` and a generated
  `cameras.cpp` (produced at configure time from
  `RawSpeed3/rawspeed3_c_api/rsxml2c.sh` +
  `RawSpeed3/rawspeed/data/cameras.xml`, per LibRaw's own documented build
  step in `RawSpeed3/README.md`).
- Preprocessor defines `USE_RAWSPEED3` and `USE_RAWSPEED_BITS` (per
  `RawSpeed3/README.md`, "Building LibRaw with RawSpeed-v3 support").
- A link dependency on the `rawspeed` target built from RawSpeed3's own
  (real) CMake build (`RawSpeed3/rawspeed/CMakeLists.txt`), configured with
  `WITH_OPENMP=OFF`, `RAWSPEED_ENABLE_WERROR=OFF`, and
  `BUILD_TOOLS`/`BUILD_TESTING`/`BUILD_BENCHMARKING`/`BUILD_FUZZERS` all OFF
  (spec section 6.6), plus the `rawspeed_get_number_of_processor_cores`
  helper target RawSpeed3's `common/` subdirectory defines separately.
  `rawspeed` is never linked into any Halide/AOT target or exposed as a
  standalone target to the app.

**pugixml include-path shim (CMake-only, no vendored source edited)**:
`RawSpeed3/rawspeed3_c_api/rawspeed3_capi.cpp` includes
`<../pugixml/pugixml.hpp>`, which assumes pugixml is vendored by hand as a
sibling of `RawSpeed3/rawspeed3_c_api/` (i.e. at `RawSpeed3/pugixml/`). This
project instead lets RawSpeed3's own build fetch a hash-pinned pugixml
tarball (see "pugixml" above) whose source dir is elsewhere under the CMake
build directory. `dng_processor/native/CMakeLists.txt` generates a small
forwarding header at build-configure time
(`${CMAKE_CURRENT_BINARY_DIR}/rawspeed3-pugixml-shim/pugixml/pugixml.hpp`,
containing a single `#include` of the real pugixml header's absolute path)
and adds its parent directory to the `raw` target's include path, so the
angle-bracket relative include resolves without editing any vendored file.

**Build-order dependency (CMake-only)**: the LibRaw/RawSpeed3/LibRaw-cmake
`add_subdirectory()` calls are placed in `dng_processor/native/CMakeLists.txt`
*after* `find_package(Halide REQUIRED COMPONENTS Halide)`, not before.
RawSpeed3's own CMakeLists.txt mutates global CACHE state as a side effect
of its own configure (e.g. via its zlib/pugixml discovery), which was
observed to break the Halide AOT generator executables' link against the
system zlib bundled inside Halide's compression support (undefined
`_uncompress`) when configured beforehand. Deferring the vendored
subdirectories until after Halide is fully configured avoids that ordering
hazard without patching RawSpeed3 itself.

**`.vendor-rev` sidecar files (CMake/script-only, no vendored source
edited)**: each vendored component's `.git` directory is removed after
fetch (`strip_git()` in `fetch_libraw_dist.sh`), and the resolved revision
is written to `<component>/.vendor-rev` instead. This is required because a
nested `.git` directory makes the parent project's git treat the entire
vendored tree as an opaque untracked boundary (like a submodule gitlink),
which would make it impossible to track this PROVENANCE.md file inside
`third_party/libraw/`. `verify_raw_provenance.py` reads `.vendor-rev` in
preference to `git rev-parse HEAD`.

**`CMAKE_POLICY_VERSION_MINIMUM` policy floor (CMake-only, R2 F6)**: pugixml
1.9's own `CMakeLists.txt` predates CMake's minimum-supported-version floor
(introduced 3.5+), so its `cmake_minimum_required()` call fails under modern
CMake unless `CMAKE_POLICY_VERSION_MINIMUM` is set. `dng_processor/native/CMakeLists.txt`
sets `CMAKE_POLICY_VERSION_MINIMUM=3.5` (as a normal, non-cache variable — see
R2 F4 fix below) for the duration of the RawSpeed3/LibRaw generic-RAW block.
This is a project-wide-visible CMake variable while set (it is not
target-scoped), but only relaxes the *sub-build's* own version-floor check;
it does not change this project's own `cmake_minimum_required(VERSION 3.14)`
or any policy this project itself relies on.

**pugixml is downloaded at configure time, not vendored (R2 F6)**: contrary
to what the "No other local modification" line below previously implied,
pugixml is **not** part of this project's vendored/offline source tree.
RawSpeed3's own `cmake/Modules/Pugixml.cmake.in` uses CMake's
`ExternalProject`/`FetchContent` machinery (gated by
`ALLOW_DOWNLOADING_PUGIXML=ON`, set in `dng_processor/native/CMakeLists.txt`)
to fetch and hash-verify:
- URL: `https://github.com/zeux/pugixml/releases/download/v1.9/pugixml-1.9.tar.gz`
- Pinned hash: `SHA512=853a9d985aae537391c6524d5413ef4de237d99d96cc58ea7fe7152f786df1e408cdacd2e4387697e23c3e67cdc1d42b29de554501309eae16d86edd0e24785f`

This means a from-scratch configure of `DNG_ENABLE_GENERIC_RAW=ON` requires
network access to `github.com` the first time (subsequent configures reuse
the already-fetched/extracted source in the build directory). This is a
self-containment / offline-CI gap against spec section 11.3 (identical
source revisions across five platforms implicitly assumes no network
dependency at configure time); vendoring pugixml 1.9 at the pinned hash
above as a fourth pinned dependency, instead of relying on the download, is
tracked as a follow-up.

R2 F4 fix note: the CACHE-forced `set(... CACHE ... FORCE)` variables that
used to configure RawSpeed3/LibRaw-cmake (`WITH_OPENMP`,
`RAWSPEED_ENABLE_WERROR`, `BUILD_TOOLS`, `BUILD_TESTING`,
`BUILD_BENCHMARKING`, `BUILD_FUZZERS`, `USE_XMLLINT`, `USE_BUNDLED_PUGIXML`,
`ALLOW_DOWNLOADING_PUGIXML`, `CMAKE_POLICY_VERSION_MINIMUM`, `LIBRAW_PATH`,
`ENABLE_RAWSPEED`, `ENABLE_OPENMP`, `ENABLE_EXAMPLES`, `LIBRAW_INSTALL`) were
persisted into `CMakeCache.txt` and therefore applied on every subsequent
configure *before* `find_package(Halide)` — reproducing the exact
Halide/zlib `_uncompress` link corruption the deferred-`add_subdirectory`
placement above was meant to avoid, on the second and later configures over
the same build directory. These are now plain (non-cache) `set()` calls,
directory-scoped and inherited by `add_subdirectory()`, relying on CMP0077
(NEW by default since this project's `cmake_minimum_required(VERSION 3.14)`
>= 3.13) so the subprojects' own `option()`/cache `set()` calls honor them
without leaving cache residue.

No other local modification. Any future local edit must be listed here with
a diff and a rationale.

## Build options

RawSpeed3 is built with `WITH_OPENMP=OFF`, `RAWSPEED_ENABLE_WERROR=OFF`, and
tools/tests/benchmarks/fuzzers disabled. LibRaw is the only decoder facade
linked into the app; no standalone RawSpeed library is exposed to any app or
Halide target (spec section 6.6). LibRaw's exported CMake library target
name (via the LibRaw-cmake overlay) is `raw` (aliased `libraw::libraw`).

### Foveon X3F support (Phase 19 W2)

`ENABLE_X3FTOOLS=ON` is set by `dng_processor/native/CMakeLists.txt` (plain
non-cache `set()`, inside the `DNG_ENABLE_GENERIC_RAW` block), which makes the
LibRaw-cmake overlay define `USE_X3FTOOLS` on the `raw` target.

Verified at LibRaw pin `df226ea4178ccd74245f4f13c23adddfa01411c9` BEFORE
enabling (inventory: `scripts/tmp/p19/t4_x3f_inventory.txt`): the decoder is
already part of this LibRaw distribution as `src/x3f/x3f_parse_process.cpp`,
`src/x3f/x3f_utils_patched.cpp` and `internal/x3f_tools.h`. **No import was
required and no file was copied into the vendored tree.** The overlay's
`file(GLOB_RECURSE ... src/*.cpp)` already compiled these sources; before this
change their bodies were entirely inside `#ifdef USE_X3FTOOLS`.

Upstream origin of that code: Kalpanika x3f-tools, redistributed by LibRaw
(see `LICENSE.LGPL` / `COPYRIGHT` in this tree). Recorded in
`docs/THIRD_PARTY_LICENSES.md`.
