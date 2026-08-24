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
| RawSpeed | https://github.com/darktable-org/rawspeed.git | de70ef5fbc62cde91009c8cff7a206272abe631e | LGPL-2.1 |
| LibRaw-cmake | https://github.com/LibRaw/LibRaw-cmake.git | eb98e4325aef2ce85d2eb031c2ff18640ca616d3 | MIT |
| pugixml | bundled by RawSpeed at the above revision | (as vendored) | MIT |
| zlib | project third_party | (existing project vendoring) | zlib |
| libjpeg-turbo | project third_party | (existing project vendoring) | IJG / BSD-3 |

## Revision pin substitutions from the original plan

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

Hashes below are for the RawSpeed3 patch set applied against RawSpeed commit
`de70ef5fbc62cde91009c8cff7a206272abe631e`. Patch 01 is applied in reverse
(see "Revision pin substitutions" above); 02-05 apply forward.

| Patch | SHA-256 |
|---|---|
| 01.CameraMeta-extensibility.patch | fcbebe0d0e03ee3849fab63d5f115697fc5bcda124cdf56efe1f0de00826db0a |
| 02.Makernotes-processing.patch | 865a63f2d42adfcb0728a556ca63c4822326d445f802e614e48e382aecf5cc7f |
| 03.remove-limits-and-logging.patch | d7924108af23bcbd3a95cebdd29465b00172695f9a4bbf4a6de8f756af5394c4 |
| 04.clang-cl-compatibility.patch | 44646383fc16c83b7c068452968b2e73c391201a0a01053a2b924cc4250ba7f1 |
| 05.no-phase-one-correction.patch | 048db1b2ed7735bfbb136a4f42ab726566ef0f31c5706899d72535cb7fe41313 |

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

No other local modification. Any future local edit must be listed here with
a diff and a rationale.

## Build options

RawSpeed3 is built with `WITH_OPENMP=OFF`, `RAWSPEED_ENABLE_WERROR=OFF`, and
tools/tests/benchmarks/fuzzers disabled. LibRaw is the only decoder facade
linked into the app; no standalone RawSpeed library is exposed to any app or
Halide target (spec section 6.6). LibRaw's exported CMake library target
name (via the LibRaw-cmake overlay) is `raw` (aliased `libraw::libraw`).
