# R1-T1 — Pre-registered parallel-decode bench expectations

Written and committed BEFORE any bench number exists (this file's commit
precedes the commit that adds `tmp/verify/r1-red-state.txt`). Per plan
§2 global constraint: thresholds below are frozen once a number exists;
re-running with different `--repeats`, corpus, or machine is a different
experiment and must be labelled as such, not silently substituted here.

## Spec deviation, disclosed up front (read before the corpus table)

Ruling 3 (`docs/logs/2026-09-05/parallel-decode-rulings.md`) directs a
corpus of 4 ARWs + 1 DNG. The plan's §R1-T1 Behavior section directs this
harness to drive `native/tests/test_concurrent_decode` — but that binary
calls `dng_pipeline_decode_to_rgb_sized` (the **DNG-only** entry point;
`test_concurrent_decode.cpp:14` usage line only accepts `<dng_file>...`).
Empirically verified today: all 4 staged ARWs fail through it with
`err=-2` (not a DNG parse target), while all DNG files decode with
`failures=0`. See `tmp/verify/r1-arw-entrypoint-check.txt`.

This is exactly plan §7 Open Question 5 (ARW C-entry-point ambiguity),
explicitly folded into R1-T3 and not yet resolved as of this ticket's
execution. Rather than guess which alternate binary/router is correct
for ARW, or silently drop the ARW requirement:

- The 4 ARWs are copied into `tmp/corpus/` and sha256'd below (ruling 3's
  letter is satisfied — they exist, local, hashed, ready).
- **This bench (`run_parallel_bench.py`) runs against a 5-file all-DNG
  corpus** (`dng_01..dng_05`), because that is the only corpus
  `test_concurrent_decode` can actually execute today.
- ARW inclusion in the bench is deferred to whichever ticket resolves the
  entry-point question (R1-T3) and, if needed, supplies/points at a
  RAW-capable driver (e.g. a harness over `raw_ffi_api.h`, the same API
  `probe_concurrent_raw.cpp` already drives). Flagged to lead-opus.

## Corpus (bench corpus — all-DNG, 5 files, sha256 below)

| file | sha256 | role |
|---|---|---|
| `tmp/corpus/dng_01.dng` (copy of `image_samples/bayer_conc_a.dng`) | `68aa8c85bcaf9d6d6ff877204bca93edfaa8580e017763148b76e0444eba4909` | single-decode (N=1) file |
| `tmp/corpus/dng_02.dng` (copy of `image_samples/bayer_conc_b.dng`) | `642affbe8f93d432b188e3a00f0d574100d61468898140f9342522fc84193ed0` | N=5 batch member |
| `tmp/corpus/dng_03.dng` (copy of `image_samples/batch_run_samples/2025-12-07-20-24-08.dng`) | `4047858512e35e93913fb30278cefe8032bc8c09c61a13b1088466f18effcf73` | N=5 batch member |
| `tmp/corpus/dng_04.dng` (copy of `image_samples/batch_run_samples/2025-12-07-17-16-07.dng`) | `793d15ae98a51fdba0c09d5af8d2b9ed6209b37ace5575affb03957d4fef0aea` | N=5 batch member |
| `tmp/corpus/dng_05.dng` (copy of `image_samples/batch_run_samples/2025-12-07-17-16-39.dng`) | `4a14353ff211b59138a02e80ff4d6bedce3678fc2f4829ca633f532adbb0e6ea` | N=5 batch member |

All 5 verified individually decodable (`failures=0`) via
`test_concurrent_decode <out> 1 <file>` before being registered here —
see `tmp/verify/r1-arw-entrypoint-check.txt`.

## Corpus (staged for other R1 tickets — 4 ARWs, per ruling 3, NOT used by this bench)

Copied once from `/Volumes/Raw Photos/RAW/2025/2025-11-12/` (read-only
source, never written to) into `tmp/corpus/`:

| file | sha256 | source |
|---|---|---|
| `tmp/corpus/arw_01.ARW` | `29590ffd29477cb969077056b0c749c37a1bc8195c2c42526635bbc99e590142` | `2025-11-12-08-18-20.ARW` |
| `tmp/corpus/arw_02.ARW` | `7995bb1a4ece5055c024c1d14650c382ffbdc9a51ea90ad760368c427719f97b` | `2025-11-12-08-19-27.ARW` |
| `tmp/corpus/arw_03.ARW` | `04fa707b87bb6c1de11479804fda9ecbc43d8947357073b2728a7d4f32b1ad5b` | `2025-11-12-08-33-48.ARW` |
| `tmp/corpus/arw_04.ARW` | `540a3bd9fd58b0282f73baf514723ca555661443fe6a713c184ae06117413ff8` | `2025-11-12-08-36-41.ARW` |

## Machine

- `sysctl hw.model` = `Mac15,14`
- `sysctl machdep.cpu.brand_string` = `Apple M3 Ultra`
- `sysctl hw.memsize` = `274877906944` (256 GiB)

## Binary under test

- `native/build/test_concurrent_decode`
- `dwarfdump --uuid` = `F6AEA371-8F77-3D16-B1DB-E8267208DF03 (arm64)`
- Built from source HEAD `f75be0bc96d62b4403f5108de07d965f9c210cc5` (pre-R1-T2;
  no Metal-context override present — this is the RED-state build)

## Instrument (`run_parallel_bench.py`)

`test_concurrent_decode` itself emits no per-decode wall-clock timing
(verified by reading `native/tests/test_concurrent_decode.cpp` in full —
only aggregate `SLOTS`/`CONCURRENT` summary lines, no `wall_ms`, no
per-decode completion timestamp). Two consequences, both implemented
without modifying that file (out of this ticket's ownership):

1. **`wall_ms`** (batch wall time) is measured externally in Python with
   `time.perf_counter()` bracketing the `subprocess.run()` call, mirroring
   `probe_concurrent_raw.py`'s external-timing pattern.
2. **`completions_ms`** (per-decode completion offsets, for staircase
   shape) is derived from the filesystem: `test_concurrent_decode` writes
   `decode_<index>.raw` immediately after each decode completes and before
   the next queue slot is drained (`test_concurrent_decode.cpp:279-288`,
   "DUMP ONLY ON THE FIRST PASS"). APFS `st_mtime` has sub-millisecond
   effective resolution, and dump-write cost for a ~72 MB RGB buffer is
   small relative to the ~15-70ms staircase steps described in the plan,
   so `os.stat(...).st_mtime - batch_start_time` is used as each decode's
   completion offset. This is legitimate only because these are *separate
   files*, not shared mutable state — no synchronization change to the
   binary is required to observe it externally.

Both invariants (why these are safe to run as separate OS processes vs.
one process with N threads) matter for validity: the metric under test is
Halide's **process-wide** Metal serialization, so `wall_ms`/`completions_ms`
MUST come from ONE process's internal N-thread concurrency (the binary's
own `--threads` argument), never from N separate OS processes (which would
not share the global lock and would trivially show full overlap regardless
of whether the fix has landed). `run_parallel_bench.py` always launches
exactly one `test_concurrent_decode` process per timed sample.

## Command shape

- N=1 sample: `test_concurrent_decode <out_dir> 1 dng_01.dng` (single decode).
- N=5 sample: `test_concurrent_decode <out_dir> 5 dng_01.dng dng_02.dng dng_03.dng dng_04.dng dng_05.dng`
  (5 threads, 5 distinct files, each thread gets exactly one file — a true
  5-way batch, not 5 sequential decodes on 1 thread).
- One warmup process run at each concurrency level, discarded, before the
  5 timed repeats — because Metal library compile
  (`metal_v21.cpp:667`) is paid once and would otherwise land inside a
  timed sample and distort the ratio. (Still paid once per OS process, so
  every process pays it once; the warmup process absorbs the coldest one.)

## Verdict rule (frozen, from contract §0 AC1 and plan §R1-T1)

`ratio = median(wall_ms @ N=5) / median(wall_ms @ N=1)` over 5 repeats each.

- `ratio >= 4.0` ⇒ **STAIRCASE** (the pre-change red state this ticket must
  reproduce; contract's own text: "currently ~5×").
- `ratio < 2.5` ⇒ **OVERLAPPED** (contract AC1 met).
- otherwise ⇒ **INCONCLUSIVE** — stop and report, do not tune parameters to
  chase a cleaner number.

Thresholds above are final as of this file's commit. They may not be
edited once `tmp/verify/r1-red-state.txt` (or any later bench artifact)
contains a number.
