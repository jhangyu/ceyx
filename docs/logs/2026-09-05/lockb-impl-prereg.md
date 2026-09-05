# R2-T2 (Lock B implementation) — pre-registration

Owner: r2t2-opus. Written and committed **before any measurement of mine exists**.
Authority: `Halcyon/docs/logs/2026-09-05/parallel-decode-contract.md` §AC (frozen),
plan §R2-T2, rulings (Lock B is UNCONDITIONAL; scope is not re-litigated on
measurement results).

Nothing in this file may be edited once a number exists. A run with a different
corpus, repeat count, cap or machine is a DIFFERENT experiment and must be
labelled as such. "Re-run until it looks better" is forbidden; the first valid
run of each experiment is the number that gets reported.

## 0. What is being changed

`native/src/pipeline/dng_copy_lock.cpp` (new, Apple-only): strong overrides of
`halide_copy_to_host` / `halide_copy_to_device` / `halide_buffer_copy` that keep
the upstream bodies (via the exported `*_already_locked` internals) but replace
the process-wide `device_copy_mutex` with 64 address-striped per-buffer mutexes.
`native/cmake/tests.cmake`: `test_concurrent_decode` gains
`dng_metal_context.cpp` + `dng_copy_lock.cpp` (R2-T1 FINDING 3 — that binary
previously carried NEITHER, so every prior AC5 run judged the unchanged path).

## 1. Experiment C — crop/slice reachability (settles R2-T1's declared gap)

`halide_device_crop` / `_slice` / `_release_crop` keep taking the real
`device_copy_mutex` and cannot be overridden from a ceyx TU. If they fire on our
decode paths, they lose mutual exclusion against the copies and the fork route
returns for those three only. R2-T1 has NO call counts for them.

Instrument: `lldb` breakpoints on the three symbols, auto-continue, hit counts
read from `breakpoint list`, run against `probe_concurrent_raw` (loads the dylib)
at N=1 and N=5 on the ARW corpus.

- **Positive control, declared before the run:** a fourth breakpoint on
  `halide_copy_to_host`, which R2-T1 measured at 1 call/decode. If its hit count
  is 0, the instrument is not observing the process and the verdict is
  `INSTRUMENT_INVALID` — no conclusion about crop/slice may be drawn.
- Verdict rules:
  - control > 0 AND crop = slice = release_crop = 0 in both arms ⇒
    `CROP_NOT_EXERCISED` — the change is correct on the measured decode paths and
    the limitation is recorded verbatim in the artifact.
  - control > 0 AND any of the three > 0 ⇒ `CROP_EXERCISED` — report immediately
    to lead2-opus as a finding (NOT as a descope proposal) with the counts; the
    striped design then needs the fork for those three.
- My static undefined-symbol scan of the AOT archives and the 186 ceyx objects
  returned 0 for all three, but it ALSO returns 0 for `halide_copy_to_host`,
  which provably fires. **That scan has a demonstrated blind spot and its zeros
  are not evidence.** Recorded here so the lldb result cannot later be dressed up
  as "two independent methods agreed".

## 2. Experiment A — colour identity (contract AC5)

`native/tests/run_colour_identity.py`, **full 9-file corpus in manifest order**
(a subset compares positionally against the wrong baseline rows and yields a
false red — `run_colour_identity.py:234`), serial AND concurrent5, against the
frozen baselines under `docs/logs/2026-09-05/colour-identity-baseline/`.

- The judged binary must be PROVEN to carry both markers before the run:
  `nm -m <binary> > file` then `grep -c` for `ceyx_metal_queue_pool_v1` and
  `ceyx_copy_lock_v1`, each must be ≥ 1, and the three copy entry points must
  read plain `external` (not `weak external`). The binary's `dwarfdump --uuid`
  goes in the artifact.
- Verdict rule: any `match=NO` is an **AC5 blocker** and the change is reverted.
  Per the standing operational rule, a decode that FAILS (the known LibRaw -206
  concurrent defect) is re-run with attempt counts recorded; a decode that
  SUCCEEDS with a different hash is the blocker.

## 3. Experiment B — post-change ARW bench

`native/tests/run_parallel_bench.py`, `--repeats 5`, `--baseline-mode per-file`,
default queue cap (4, env unset), corpus in R1-T2/R2-T1 order:
`arw_01 arw_02 arw_03 arw_04 arw_01`.

**Confound declared in advance:** R2-T1's 2.1487 was measured on binaries built
BEFORE `007e72e`, i.e. with `LIBRAW_NOTHREADS` still present. My tree is after it.
A raw comparison of my ratio against 2.1487 would therefore be confounded. So the
bench is an **A/B on my own tree**, both arms built from the same HEAD in the same
configuration, interleaved in one session:

- **CONTROL** = `native/build-r2t2-ctl`, configured with
  `-DCMAKE_CXX_FLAGS=-DCEYX_COPY_LOCK_DISABLED`, which compiles the new TU to
  nothing so Halide's weak definitions win (stock behaviour). Must show all three
  entry points as `weak external` and `ceyx_copy_lock_v1` count 0.
- **TREATMENT** = `native/build-r2t2`, same options without that flag. Must show
  the three as plain `external` and `ceyx_copy_lock_v1` count 1.

Predictions, written before the numbers exist:

- Control ratio lands near R2-T1's 2.1487 (I predict 1.9–2.4). If it does not,
  the `007e72e` change moved the world and the cross-round comparison is
  reported as invalid rather than quietly reinterpreted.
- Treatment ratio < control ratio. If the mutex really carries 79.00 ms of a
  89.28 ms gap, removing it should recover most of it; the ideal bound is ~1.25
  and I predict the treatment lands in 1.3–1.8. **A treatment ratio ≥ control
  ratio means the change bought nothing and the plan's §R2-T2 AC requires it to
  be reverted; that outcome will be reported, not tuned away.**
- Also reported verbatim against R2-T1's 2.1487, with the confound restated.

Failed decodes are excluded from medians by the harness and their counts are
reported per arm (the -206 defect is fixed after `007e72e`, so a non-zero count
here is itself worth reporting).

## 4. Regression gate

Every native test binary that is green today must still be green: the list is
enumerated from the build directory, each is run individually, and each artifact
carries its own `RC=$?` captured on the line after the command. No `tail`/`grep`
laundering of exit codes; no aggregate "all passed" claim without the per-binary
RC lines.
