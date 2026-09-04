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

---

## Section 2 — Corrected-baseline + Bayer-only experiments (pre-registered before either number exists)

Written after reviewing experiment 1's result (ratio=3.805, INCONCLUSIVE,
commit 81c3053) with lead-opus and t4-sonnet. Two independent problems
with experiment 1's methodology were identified — NOT a "the number was
disappointing" rationale, both would need fixing regardless of which
direction they moved the ratio:

1. **Route-coverage confound.** t4-sonnet independently measured
   `STAGE3_WORKSPACE registrations` per file. Verified again here,
   individually, on the unmodified tree:
   `dng_01.dng` (bayer_conc_a) → `registrations=1`;
   `dng_02.dng` (bayer_conc_b) → `registrations=1`;
   `dng_03.dng`, `dng_04.dng`, `dng_05.dng` (batch_run_samples) →
   `registrations=0` each. Only 2 of experiment 1's 5 files reach the
   Stage-3 Bayer route into Halide Metal — the ONLY code path this
   campaign's Lock A/B changes touch. The other 3 decode via a different
   route that mostly does not contend on the process-wide `thread_lock`
   under audit. A 5-file batch that is 2-contending/3-not is expected to
   show a diluted ratio versus a fully-contending batch, independent of
   whether the eventual fix works.
2. **File-size / baseline-selection confound.** Experiment 1 compared
   `wall_ms(threads=1, corpus[0]=dng_01 alone)` against
   `wall_ms(threads=5, all 5 different files)`. The 5 files are not the
   same pixel count (`dng_01`..`dng_05` range from ~4080×3056 up to
   6000×4000, ~1.9x spread), so the ratio conflated per-file decode cost
   with concurrency overlap, and its value depended on which specific file
   happened to be picked as the N=1 sample.

### Fix, pre-registered before running either experiment

`run_parallel_bench.py` gained `--baseline-mode {matched,single}`
(default `matched`). `single` reproduces experiment 1's exact method
(kept for reference/reproducibility only — do not use for new baselines).
`matched` is the corrected default: the N=1 sample is
`threads=1` over the **same 5-file batch** used for the N=5 sample, run as
ONE process (the binary decodes them serially, one after another, in
argument order). Both arms then decode the literal same files — the only
variable is concurrency, which removes the file-size confound entirely
without needing to sum separately-measured single-file runs.

### Experiment 2A — corrected-baseline, same 5-file DNG corpus as experiment 1

- Corpus: identical to experiment 1 (`tmp/corpus/dng_01.dng` .. `dng_05.dng`,
  sha256s as recorded above — unchanged, no re-selection).
- Method: `run_parallel_bench.py --baseline-mode matched` (N=1 = serial
  threads=1 over all 5 files in one process; N=5 = threads=5 over the same
  5 files).
- Thresholds: UNCHANGED from experiment 1 — `ratio >= 4.0` ⇒ STAIRCASE,
  `< 2.5` ⇒ OVERLAPPED, else INCONCLUSIVE. Not re-derived, not tuned.

### Experiment 2B — Bayer-only (Stage-3-registering files only)

- Corpus: only the 2 files verified above to register Stage-3
  (`dng_01.dng`, `dng_02.dng`). To exercise 5-way concurrency with only 2
  distinct source files, the 5-file argument list for the N=5 sample
  repeats them: `dng_01, dng_02, dng_01, dng_02, dng_01` (5 positional
  arguments, 5 independent decode contexts/dumps — repeating a file path
  decodes it again from scratch each time, it does not share state).
  N=1 baseline (`--baseline-mode matched`) uses the identical 5-argument
  list at `threads=1`.
- Rationale: these are the only 2 corpus DNGs on the Stage-3/Metal path
  the campaign is changing; this experiment answers "does the contract's
  ~5x reproduce when every decode in the batch actually contends," isolated
  from the route-coverage confound in experiment 1.
- Thresholds: UNCHANGED — same frozen rule as above.

### Verdict-independence statement (per plan §2 constraint)

Both experiments are pre-registered here before either number exists, per
lead-opus's explicit instruction. If either lands under 4.0, that is the
finding — the fix documented here is a structural correction (route
coverage, baseline matching), not a search for a passing number, and it
would have been required even if it made the ratio worse.

---

## Correction — experiments 2A/2B verdicts are INVALID-BY-CONSTRUCTION (lead-opus, 2026-09-05)

`--baseline-mode matched` (used for 2A/2B) sets the N=1 denominator to
`threads=1` over the SAME 5-file batch in ONE process — i.e. the
**aggregate serial time for 5 decodes**, not a single-decode time. Under
that denominator, `ratio = wall5/wall1` is bounded near 1.0 by
construction (perfect overlap → ~0.2, zero overlap → ~1.0) and can never
reach the frozen STAIRCASE threshold of 4.0. Applying thresholds
calibrated for a single-decode denominator to an aggregate-serial-work
denominator produces a verdict label that was decided before the binary
ran — an assertion that cannot fail. **The `OVERLAPPED` verdict lines
recorded for 2A (ratio=0.8264) and 2B (ratio=0.8510) are retracted as
verdicts.** The raw wall-time measurements themselves are NOT retracted
and remain useful — read correctly, they show near-total serialization:

- Speedup vs. serial = `1/ratio`: 2A = 1.21x, 2B = 1.18x.
- Overlap efficiency = speedup ÷ 5 (ideal 5-way): 2A ≈ 24%, 2B ≈ 24%.
- i.e. the 5-way batch is only ~18-21% faster than doing the 5 decodes one
  after another — consistent with heavy serialization and a small amount
  of overlap, not with "the batch beats serial" as originally (mis)read.

This correction does not change the corpus, the frozen thresholds, or the
raw artifacts (`tmp/verify/r1-exp2a-body.txt`, `r1-exp2b-body.txt`,
commit 0a760de) — it changes only which reading of those numbers is valid.

## Experiment 3 — per-file baseline (AC1-correct methodology), pre-registered before any number exists

Fixes the flaw above properly: the N=1 denominator must be an actual
single-decode time. `--baseline-mode per-file` (new default) decodes
each of the 5 batch files INDIVIDUALLY — `threads=1`, one file per
process invocation, one shared warmup (first file, discarded) — and pools
all `(file x repeat)` wall_ms samples (5 files x `--repeats` each) into
one list; the **median** of that pooled list (not mean — stated
explicitly per lead-opus's instruction) is the single-decode denominator.
`ratio = median(wall_ms @ N=5 batch) / median(pooled per-file single-decode wall_ms)`.

This also answers reading (b) from the earlier report (whether the serial
arm was itself warmed/pipelined by adjacent decodes in the same process):
per-file mode runs every single-decode sample in its own fresh process,
so there is no cross-decode cache warming in the denominator at all.

- **Experiment 3A** — same 5-file mixed-route DNG corpus as experiments 1
  and 2A (`dng_01.dng` .. `dng_05.dng`, unchanged sha256s).
- **Experiment 3B** — Bayer-only, same corpus as 2B
  (`dng_01, dng_02, dng_01, dng_02, dng_01` — the only 2 Stage-3-registering
  files, repeated to fill the 5-way batch; N=1 per-file samples are drawn
  from the 2 distinct underlying files, each decoded fresh per sample).
- Thresholds: UNCHANGED, frozen — `ratio >= 4.0` ⇒ STAIRCASE, `< 2.5` ⇒
  OVERLAPPED, otherwise INCONCLUSIVE. Not re-derived from this experiment.

---

## Experiment 4 — ARW, production entry point (`raw_decode_and_process`), pre-registered before any number exists

Unblocked per lead-opus 2026-09-05: `native/tests/probe_concurrent_raw`
already exists, unmodified, and calls `raw_decode_and_process` directly
(`probe_concurrent_raw.cpp:48`) — the same production ARW entry point
t3-opus's audit traced Dart-side (`raw_route.dart` →
`dng_decoder_service.dart` → `dng_bindings.dart:222`). No dual-route
change to `test_concurrent_decode.cpp` was needed after all; that plan was
cancelled.

### Driver provenance (material difference from experiments 1-3)

Experiments 1-3 drive `native/build/test_concurrent_decode` (DNG-only,
`dng_pipeline_decode_to_rgb_sized`). Experiment 4 drives a DIFFERENT
binary, `native/build/probe_concurrent_raw` (RAW/ARW-only,
`raw_decode_and_process`, "ALREADY LOCK-FREE" per its own file header —
no ceyx DNG single-flight mutex on this path, only whatever Halide/Metal
serialization the campaign targets). `run_parallel_bench.py` gained
`--driver {test_concurrent_decode,probe_concurrent_raw}` to support both
binaries' different CLI shapes and output formats without touching either
binary's source.

- `dwarfdump --uuid native/build/probe_concurrent_raw` =
  `F730B58F-7B30-362B-A05C-B053BA1CA123 (arm64)`
- `native/tests/probe_concurrent_raw.cpp` sha256 =
  `a75c9b44e0e5c1b612fcdd14ec82ecfe68fc4517714219d00db9a788ab89fb76`
  (unmodified from before this campaign — verified by reading the file;
  no edits made to it in this ticket)
- Built from source HEAD `a62b16c9fa7b3081c81881c399e1e4ba50260c51`

### Known instrument limitation, accepted rather than engineered around

`probe_concurrent_raw` prints only its own aggregate
`PROBE threads=N files=M wall_ms=W` line — no per-decode dump mechanism,
so **`completions_ms` is always `[]` for this driver.** The ratio
(what AC1 is stated in) is unaffected; the staircase SHAPE is not
observable from this driver. Per lead-opus's instruction, not building a
new driver to recover it now.

### Corpus

The 4 ARWs already staged and hashed in the corpus table above
(`tmp/corpus/arw_01.ARW` .. `arw_04.ARW`). Only 4 distinct ARW files exist
on this machine (ruling 3's allotment); to fill the 5-way batch,
`arw_01.ARW` is repeated once: batch argument list =
`arw_01, arw_02, arw_03, arw_04, arw_01`. All 4 individually verified
decodable via `probe_concurrent_raw` today (`RC=0` each,
`tmp/verify/r1-arw-probe-check.txt`).

### Method

`run_parallel_bench.py --driver probe_concurrent_raw --baseline-mode per-file`
— identical construction to experiment 3: N=5 arm is one process,
`threads=5`, the 5-argument batch above; N=1 arm decodes each of the 4
distinct ARWs individually (`threads=1`, one file per process), pools all
`(file x repeat)` wall_ms samples, and takes the median as the
single-decode denominator — commensurable with experiments 3A/3B by
construction (same denominator-building method), which is the point of
running ARW at all.

### Thresholds

UNCHANGED, frozen — `ratio >= 4.0` ⇒ STAIRCASE, `< 2.5` ⇒ OVERLAPPED,
otherwise INCONCLUSIVE. Not re-derived for this experiment.
