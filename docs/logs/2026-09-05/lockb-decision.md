# R2-T1 — Lock-B design memo

Owner: r2t1-opus, 2026-09-05. Evidence: `tmp/verify/r2t1-lockb-sizing.txt` and
`tmp/verify/r2t1-instrument-validation.txt`. Pre-registration: `lockb-sizing-prereg.md`
(commits `37939c1`, `809553d`, both before any number existed).

**Scope framing, restated so nobody reads this memo as a gate:** Lock B is
UNCONDITIONAL under the standing user directive. This memo sizes the residual to
decide *how much machinery* Lock B justifies. Nothing here proposes doing less.

## Recommendation

`lockb|residual_ratio=2.1487|attributable_ms=79.00|recommendation=SCOPE_FULL`

`device_copy_mutex` accounts for **88.5 %** of the post-Lock-A residual on the
ARW workload: 79.0 ms of the 89.3 ms by which a 5-way batch exceeds the
4-queues-for-5-lanes ideal is time threads spend *blocked on that one mutex*.

## The finding that should change how R2-T2 is scoped

The campaign assumed the residual was mostly the deliberate 4-for-5 queue
sharing. **It is not.** Sweeping `DNG_METAL_QUEUE_CAP` across 1, 2, 4, 5, 8 moves
the ratio only 2.2191 → 1.9790. A *single shared queue for five lanes* is barely
worse than four. The Metal queue dimension is worth ~17 ms of an ~89 ms gap
(`queue_share` 0.099); OpenMP oversubscription is worth nothing measurable
(`omp_share` ≈ 0, and its sign is negative); everything else is 1.6 %.

Two consequences worth stating plainly:
* Raising `kMaxDecodeSlots` or the queue cap would buy very little. That remains
  out of scope and this measurement supports leaving it there — the cap is not
  what is binding.
* The mechanism is not subtle. Each ARW decode spends ~30 ms inside the lock
  (18 held calls per decode: 17 `halide_copy_to_device`, 1 `halide_copy_to_host`,
  0 `halide_buffer_copy`). Five decodes therefore contain ~149 ms of strictly
  mutually-exclusive work against a ~211 ms batch wall. That *is* the staircase
  that survived Lock A.

## What R2-T2 should build, and what it no longer needs to

**The `halide_runtime_fork/` copy of `device_interface.cpp` and its
`llvm-objcopy --weaken` link discipline are NOT required for the measured path.**
This is demonstrated, not argued: a ceyx translation unit providing strong
definitions of `halide_copy_to_host`, `halide_copy_to_device` and
`halide_buffer_copy` — which take the real
`Halide::Runtime::Internal::device_copy_mutex` and call the upstream
`*_already_locked` internals — links, runs, and decodes **bit-identically** to
the frozen 9-row baseline, serially and 5-way concurrent
(`colour|verdict=IDENTICAL`, `AC_C_RC=0`). `nm -m` shows `weak external` →
`external` for all three, with the marker absent in the control build. That
instrument is Lock B's mechanism with the locking *policy* left unchanged; R2-T2
only has to change the policy.

Boundary, and I will not overstate it: `halide_device_crop`, `halide_device_slice`
and `halide_device_release_crop` hold the same mutex and are **not** reachable
this way, because their bodies need the opaque `halide_device_interface_t::impl`.
I did not override them and therefore have no call counts for them. If R2-T2
finds they are exercised, the fork returns *for those three only*. On the
evidence I have, the copy path is where the 79 ms lives.

Suggested policy, in increasing order of what the evidence actually demands:
1. Replace the process-wide mutex with per-`halide_buffer_t` locking (or an
   address-striped lock) in the three strong overrides. Every decode owns its own
   buffers, so distinct decodes stop excluding each other.
2. R2-T2's real verification burden is *not* the linking — that is settled — but
   whether the upstream `*_already_locked` bodies are safe when entered
   concurrently on distinct buffers. That is the question to design the tests
   around.

## Corrections to the inherited brief, from source

* `halide_device_malloc` and `halide_device_free` do **not** take
  `device_copy_mutex` (Halide v21 `device_interface.cpp:251`, `:274` — no
  `ScopedMutexLock`). The holders are `:142`, `:210`, `:619`, `:645`, `:676`,
  `:715`. Lock B's surface is narrower than the ticket assumed.
* The residual is **not** dominated by the structural 4-for-5 ceiling, as the
  round-2 briefs supposed.

## Risk the user is being asked to carry

Reduced, not eliminated, versus what the plan anticipated. The plan's stated risk
was reimplementing `copy_to_host_already_locked` — a Halide internal with no
stability guarantee — plus fork maintenance. What the evidence now supports is
*calling* those internals rather than reimplementing them, and no vendored fork.
The residual risk is the one that cannot be removed: ceyx would depend on three
Halide-internal symbols (`copy_to_host_already_locked`,
`copy_to_device_already_locked`, `halide_buffer_copy_already_locked`) continuing
to exist with these signatures across Halide upgrades. A Halide bump could break
the link — loudly, at build time, not silently at runtime, which is the failure
mode you want. Pinning is already the practice (`third_party/halide/VERSION` =
21.0.0).

## What this memo does not cover

DNG. The only pixel-dumping driver, `test_concurrent_decode`, does not contain
R1-T2's queue pool (statically linked, never loads the dylib; marker count 0;
zero `queuepool|ev=bind` lines at runtime against 4 for the ARW driver). Every
DNG ratio and every colour-identity run taken so far was driven by it. That is an
input to tickets #10 and #11, recorded in
`tmp/verify/r2t1-instrument-validation.txt` §3, and accepted by lead2-opus.

All measurements were taken on binaries built before `007e72e`, i.e. with
`LIBRAW_NOTHREADS` still present, deliberately, so that `T5` is comparable to the
R1-T2 baseline the attribution is defined against. R2-T3 owes a post-fix
re-baseline.
