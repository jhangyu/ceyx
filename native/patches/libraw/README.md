# Project-authored LibRaw patches

These diffs are rooted at the **LibRaw** tree
(`native/third_party/libraw/`), not at RawSpeed3. They are applied
by `scripts/fetch_libraw_dist.sh` after LibRaw's own `RawSpeed3/patches/*.patch`
set and before the `.git` directories are stripped.

Numbering continues from LibRaw's own set (01-05) so a patch number is globally
unique in this project's provenance record.

| Patch | Target file | Purpose |
|---|---|---|
| 06.fuji-tryrawspeed3.patch | `src/utils/decoder_info.cpp` | Sets `LIBRAW_DECODER_TRYRAWSPEED3` on `fuji_compressed_load_raw` and `fuji_14bit_load_raw` so RAF files are offered to RawSpeed3 first (Phase 19 W1). |
| 08.x3f-parallel-lut.patch | `src/x3f/x3f_utils_patched.cpp` | Foveon X3F TRUE-engine acceleration: 3-plane std::thread parallel decode + 256-entry packed branchless Huffman LUT fast path driven by a register-resident 64-bit bit window, with verbatim tree-walk fallback (RAW decode accel round 2026-08-27; round-2 R4 window/branchless rework 2026-08-28 — a 16-bit two-symbol pair LUT was measured and rejected, floors 162/169 ms vs 146/146 ms, see `native/scripts/tmp/foveon_impl_artifacts/`). |
| 09.fuji-stdthread-parallel.patch | `src/decoders/fuji_compressed.cpp` | Fuji compressed RAF (lossless + lossy) block-parallel decode via bounded std::thread pool, plus a real mutex for the datastream sites upstream leaves unguarded or under `omp critical` (RAW decode accel round, 2026-08-27). Round 2 (2026-08-28, contract R1 form A): the std::thread pool is now the single fuji path on every platform — the OpenMP branch is removed from this file (its per-byte `omp critical` thread-id lookup cost −11…−15% wall, measured) — and the pool gains a thread-count cap (default 16, `CEYX_FUJI_DECODE_THREADS` override). |
| 10.fuji-qtable-cache.patch | `src/decoders/fuji_compressed.cpp` | Round-2 contract R2: per-strip memoisation of `init_main_qtable`/`setup_qlut` output keyed by `q_base` (pure function of q_base for a fixed file; 9.9% measured profile share). `init_main_grads` still runs on every q_base transition. Applies on top of patch 09. |
| 12.normalize-model-orig-race.patch | `src/metadata/normalize_model.cpp` | R4 item 2 (2026-09-05): fixes a shared-static data race at `GetNormalizedModel()` line 406 affecting every non-Sony brand (Fujifilm/Kodak/Leaf/Konica-Minolta/Nikon/Olympus/Panasonic/PhaseOne/Samsung-Pentax/Samsung) — `static const char *orig;` was one process-global pointer racing across concurrent decodes of different camera models. Changed to a per-call automatic local (`const char *orig = "";`); every alias table's `'@'`-prefixed-first-element convention (mechanically checked, `native/scripts/check_alias_table_convention.py`) guarantees it is always written before read within one call. Full rationale and red→green evidence: `native/third_party/libraw/PROVENANCE.md` and `docs/logs/2026-09-05/race2/`. |

Patch 07 (`07.fuji-rotated-gate.patch`, conditional, would exempt `filters == 9`
X-Trans from the `!IO.fuji_width` clause of the RawSpeed3 gate in
`src/decoders/unpack.cpp`) was **not created**: Task 1's measurement
(`native/scripts/tmp/p19/t1_eligibility.txt`) showed
`fuji_width=0` for both corpus RAF samples both before and after patch 06, so
the `!IO.fuji_width` clause never blocks these files and the extra patch is
unnecessary per the decision rule in `t1_prereg.md`.

Every patch here must have its SHA-256 recorded in
`third_party/libraw/PROVENANCE.md` under "Project-authored LibRaw patches".
`scripts/verify_raw_provenance.py` fails if a patch file has no recorded digest,
or if its added lines are not present in the vendored tree.

**The vendored tree is never edited except through a patch in this directory.**
