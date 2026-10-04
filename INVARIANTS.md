# INVARIANTS

User rulings that affect program behavior (transcribed from the Task 6 remediation contract).

## Rulings 2026-10-04 (memreclaim Task 6)

- R1: G5 Linux/Android limit = option (a): per-platform limit set from real measured numbers (Android device number after the F10 fix is the primary input). Numbers come back to the user before any limit is written.
- R2: Adopt the G5 precondition: step 5 must have lowered resident memory by >= 0.9 x 256 MiB, else G5 = "not observable" FAIL. Windows must be rerun after this change.
- R3: OrbStack docker is an accepted vehicle for the one-time Linux run.
- R4: Fix the ceyx_debug_idle_funnel_counters 7-vs-10 pointer mismatch in both callers (lib/perf/perf_driver.dart, tools/memgate/memgate.py).
- R5: Android F9: if the pre-registered shape sweep finds no qualifying shape, Android gets a recorded exception in the Windows form (re-proven each run).
