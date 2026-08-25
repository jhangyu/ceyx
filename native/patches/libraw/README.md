# Project-authored LibRaw patches

These diffs are rooted at the **LibRaw** tree
(`dng_processor/native/third_party/libraw/`), not at RawSpeed3. They are applied
by `scripts/fetch_libraw_dist.sh` after LibRaw's own `RawSpeed3/patches/*.patch`
set and before the `.git` directories are stripped.

Numbering continues from LibRaw's own set (01-05) so a patch number is globally
unique in this project's provenance record.

| Patch | Target file | Purpose |
|---|---|---|
| 06.fuji-tryrawspeed3.patch | `src/utils/decoder_info.cpp` | Sets `LIBRAW_DECODER_TRYRAWSPEED3` on `fuji_compressed_load_raw` and `fuji_14bit_load_raw` so RAF files are offered to RawSpeed3 first (Phase 19 W1). |

Patch 07 (`07.fuji-rotated-gate.patch`, conditional, would exempt `filters == 9`
X-Trans from the `!IO.fuji_width` clause of the RawSpeed3 gate in
`src/decoders/unpack.cpp`) was **not created**: Task 1's measurement
(`dng_processor/native/scripts/tmp/p19/t1_eligibility.txt`) showed
`fuji_width=0` for both corpus RAF samples both before and after patch 06, so
the `!IO.fuji_width` clause never blocks these files and the extra patch is
unnecessary per the decision rule in `t1_prereg.md`.

Every patch here must have its SHA-256 recorded in
`third_party/libraw/PROVENANCE.md` under "Project-authored LibRaw patches".
`scripts/verify_raw_provenance.py` fails if a patch file has no recorded digest,
or if its added lines are not present in the vendored tree.

**The vendored tree is never edited except through a patch in this directory.**
