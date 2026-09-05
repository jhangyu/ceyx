# Project-authored LibRaw-cmake patches

These diffs are rooted at the **LibRaw-cmake** overlay tree
(`native/third_party/libraw-cmake/`), not at LibRaw or RawSpeed3. They are
applied by `native/scripts/deps/fetch_libraw.py`'s `apply_patches()` call for
`libraw_cmake_dest`, run after that clone and before its `.git` directory is
stripped (`apply_patches()` requires `.git` to be present).

| Patch | Target file | Purpose |
|---|---|---|
| 11.no-libraw-nothreads.patch | `CMakeLists.txt` | Drops the hardcoded `LIBRAW_NOTHREADS` compile definition, which redirected LibRaw's per-instance scratch state to process-global `static` storage and caused an intermittent Sony-ARW makernote decrypt race (`kRawErrMetadataInvalid` / -206) under concurrent decode. Task #12, 2026-09-05; see `third_party/libraw/PROVENANCE.md` under "Project-authored LibRaw-cmake patches" for the full rationale and measurements, and commit `007e72e` for the original (now-untracked) fix this patch replaces. |

Every patch here must have its SHA-256 recorded in
`third_party/libraw/PROVENANCE.md` under "Project-authored LibRaw-cmake
patches".

**The vendored tree is never edited except through a patch in this directory.**
