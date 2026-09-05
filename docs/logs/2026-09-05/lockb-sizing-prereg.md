# R2-T1 — Lock-B residual sizing: PRE-REGISTRATION

Written and committed **before any number of this ticket exists**. Owner: r2t1-opus.
Baseline HEAD: `9b46c56` (ceyx). Machine: this Apple-silicon host (recorded in the artifact).

Scope note, inherited and binding: the go/no-go framing of plan §R2-T1 is **SUPERSEDED**
(`Halcyon/docs/logs/2026-09-05/parallel-decode-rulings.md`, USER DIRECTIVE 2026-09-05).
Lock-B work happens regardless. This ticket produces a **design weight**, not a decision to
do or skip. No outcome below may be used to propose descoping.

## 0. Question

Post-Lock-A, the ARW 5-way ratio is 2.00–2.25 (R1-T2 AC6: 2.0030 / 2.1693 / 2.2500 / 2.1950;
pre-change control 3.2174 / 2.7229 / 2.8042). The 4-queues-for-5-lanes ideal is ~1.25.
How is the residual gap split between:

* **(B) `device_copy_mutex`** — `device_interface_v21.cpp:28`, taken by `halide_copy_to_host`
  (:142), `halide_copy_to_device` (:210), `halide_buffer_copy` (:619), `halide_device_crop`
  (:645), `halide_device_slice` (:676), `halide_device_release_crop` (:715);
* **(Q) deliberate queue sharing** — cap 4 with 5 lanes (`dng_metal_context.cpp:101-119`,
  `DNG_METAL_QUEUE_CAP` clamps to [1,8]);
* **(O) everything else** — CPU-side LibRaw open/unpack, memory bandwidth, allocator,
  Amdahl-serial fractions.

## 1. Fixed measurement objects

* Corpus: `tmp/corpus/arw_01.ARW … arw_04.ARW` (sha256 recorded in the artifact) plus
  `dng_01..dng_05.dng` for the DNG cross-check. ARW is the user's workload and the primary
  object; DNG is reported but does not drive the recommendation.
* Drivers: `native/tests/run_parallel_bench.py --driver probe_concurrent_raw` (ARW) and
  `--driver test_concurrent_decode` (DNG), `--repeats 5`, medians, warmup discarded — the
  R1-T1 harness, unmodified.
* Definitions (all medians over 5 repeats, one process, `--threads=N`):
  * `T1` = wall_ms at N=1 (single decode), `T5` = wall_ms of the N=5 batch,
    `ratio = T5 / T1`.
  * `gap_ms = T5 − 1.25 × T1` — the residual over the 4-for-5 ideal, in ms. This is the
    quantity being attributed. (If `T5 < 1.25 × T1`, `gap_ms ≤ 0` ⇒ verdict
    `NO_RESIDUAL`, see §5.)
  * `lockb_ms` = median over decodes of (total `device_copy_mutex` **wait** time per decode
    at N=5) − (same at N=1). Measured by the probe of §2.
  * `queue_ms = T5(cap=4) − T5(cap=8)`, same session, interleaved.
  * `omp_ms = T5(cap=8, OMP_NUM_THREADS default) − T5(cap=8, OMP_NUM_THREADS=1)`,
    the CPU-oversubscription component of the (O) term (X6). Reported as
    `omp_share = omp_ms / gap_ms`; it is part of (O), never of (B) or (Q).
  * `lockb_share = lockb_ms / gap_ms`, `queue_share = queue_ms / gap_ms`,
    `other_share = 1 − lockb_share − queue_share` (may be negative; reported as-is, not
    clamped).

## 2. Instrument

A private, temporary diagnostic translation unit `dng_copy_lock_probe.cpp` that provides
**strong** overrides of the six weak `device_copy_mutex` entry points, each of which:
timestamps, takes the *real* `Halide::Runtime::Internal::device_copy_mutex`, timestamps
again, calls the corresponding upstream `*_already_locked` internal, timestamps, unlocks.
Feasibility is established by symbol evidence, not assumption: `nm -m` on
`native/build-r1t2/libdng_decoder_native.dylib` shows
`__ZN6Halide7Runtime8Internal17device_copy_mutexE`,
`__ZN6Halide7Runtime8Internal27copy_to_host_already_lockedEPvP15halide_buffer_t`,
`_copy_to_device_already_locked`, `_halide_buffer_copy_already_locked` all present as
`weak external` (`tmp/r2t1/nm_all.txt`). The R1-T2 strong-override-of-weak pattern is the
precedent.

Recording is gated on `DNG_COPY_LOCK_PROBE=1`; with the variable unset the overrides do
exactly lock → `*_already_locked` → unlock, i.e. upstream semantics. Per-thread counters are
dumped at exit to `$DNG_COPY_LOCK_PROBE_OUT` as
`copylock|thread=<id>|fn=<name>|calls=<n>|wait_us=<f>|work_us=<f>`.

**Hygiene, non-negotiable:**
* The probe source lives under `tmp/` (gitignored) and is compiled **only** into the private
  build dir `native/build-r2t1`. `native/cmake/ffi.cmake` is edited temporarily; a `cp`
  backup is taken first and the file is restored by `cp` afterwards, with
  `git status --porcelain` shown clean at both ends inside the artifact.
* `native/build/` is never touched. Configure passes `-DENABLE_X3FTOOLS=ON` (virgin-configure
  trap).
* Every artifact self-captures `RC=$?` on the line after the command. No `nm | grep -q`.

## 3. Experiments (fixed list; no others may be substituted after numbers exist)

| id | what | code needed |
|---|---|---|
| X1 | ARW `ratio` on the **existing** `build-r1t2` binaries at `DNG_METAL_QUEUE_CAP` ∈ {1, 2, 4, 5, 8}, interleaved, 5 repeats each | none (env only) |
| X2 | Probe totals at N=1 and N=5 (cap=4), ARW; call counts per entry point | instrumented build |
| X3 | **Instrument positive control**: `DNG_COPY_LOCK_PROBE_SPIN_US=2000` (extra hold inside the lock). Measured N=5 `wait_us` must rise by ≥ 5× versus X2's N=5 `wait_us`, and N=1 `wait_us` must stay near zero | instrumented build |
| X4 | **Inertness**: `run_colour_identity.py` on the instrumented build with the probe **off**, ARW+DNG, serial and concurrent5 | instrumented build |
| X5 | DNG cross-check of X1 at cap ∈ {4, 8} | none |
| X6 | **(O)-term probe, env-only**: ARW `ratio` at `OMP_NUM_THREADS` ∈ {1, default} × cap ∈ {4, 8}. Rationale registered in advance: `build-r1t2` has `ENABLE_OPENMP:BOOL=ON`, so each LibRaw unpack may itself fan out across all cores; five concurrent decodes would then oversubscribe the CPU, which would inflate the residual with a term that is neither the mutex nor queue sharing | none |

Attempt accounting: a **FAILED** ARW decode is the known LIBRAW_NOTHREADS race (~30 %/run,
`tmp/verify/race-206-root-cause.md`) — re-run and record the attempt count in the artifact;
it is not a blocker. A **SUCCEEDED-but-hash-different** decode is an AC5 blocker: stop and
report.

## 4. Instrument-validity gates (checked before any attribution is believed)

* X3 must show the registered rise. If it does not, the wait meter is not live ⇒ report
  `INSTRUMENT_INVALID`, publish no attribution.
* X4 must be `colour|verdict=IDENTICAL`. If not, the probe is not inert ⇒ discard the
  instrumented numbers.
* X2 must show `calls > 0` for at least one entry point on the ARW path. If ARW never takes
  `device_copy_mutex` at all, `lockb_share := 0` by direct evidence and that is the finding.

## 5. Verdict rules — fixed NOW, may not be edited once a number exists

One `lockb|residual_ratio=<f>|attributable_ms=<f>|recommendation=<...>` line is emitted.
`recommendation` is a **design weight** and takes exactly one of:

| condition (on ARW) | recommendation | meaning |
|---|---|---|
| `lockb_share ≥ 0.50` | `SCOPE_FULL` | The mutex is the dominant residual: de-globalize properly — per-`halide_buffer_t`/per-device-interface locking across all six entry points, upstream copy of `device_interface.cpp` into `halide_runtime_fork/` pinned to Halide v21.0.0 if and only if §6 shows the strong-override route cannot reach an internal it needs. |
| `0.20 ≤ lockb_share < 0.50` | `SCOPE_TARGETED` | Shard the mutex (N-way stripe keyed by buffer address) via strong overrides of only the entry points carrying the measured wait; no upstream fork. |
| `lockb_share < 0.20` | `SCOPE_MINIMAL` | Ship the smallest correct de-globalization (striped lock, strong overrides only, no fork, no vendored upstream file) and report the dominant residual term as the campaign's real finding. |
| `gap_ms ≤ 0` | `NO_RESIDUAL` | Read as `SCOPE_MINIMAL` for design purposes; report that the 4-for-5 ideal is already met. |
| instrument gates of §4 fail | `INSTRUMENT_INVALID` | No attribution published; report the failure trace. |

Ties/borderline (within ±0.02 of a boundary) resolve to the **heavier** scope — the user
directive forbids measurement-driven shrinkage, so ambiguity must not be spent on cutting.

## 6. Mechanism question decided by symbol evidence, not preference

The plan assumed Lock B requires `halide_runtime_fork/` plus `llvm-objcopy --weaken`. That
assumption is testable: if the `*_already_locked` internals are linkable from a ceyx
translation unit (they are, per §2's `nm` output) and a strong override of the six entry
points links and runs, then **the fork route is unnecessary** and R2-T2 should take the
strong-override route. The instrumented build of X2 is itself the proof of that claim: it is
the Lock-B mechanism with the locking policy left unchanged. Whether it links and produces
`colour|verdict=IDENTICAL` (X4) is the evidence. This finding is reported regardless of the
share numbers.

## 7. Forbidden

* Re-running any experiment with different repeats/corpus/cap set until a number falls on a
  preferred side of a threshold. A changed parameter is a *different experiment* and must be
  labelled and reported as such, alongside the original.
* Editing §1 definitions, §3 experiment list, or §5 thresholds after the first number exists.
* Starting any Lock-B implementation in this ticket.
* Touching production code in the end state (temporary edits are `cp`-restored and proven
  restored).
