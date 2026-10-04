# INVARIANTS

User rulings that affect program behavior (transcribed from the Task 6 remediation contract).

## Rulings 2026-10-04 (memreclaim Task 6)

- R1: G5 Linux/Android limit = option (a): per-platform limit set from real measured numbers (Android device number after the F10 fix is the primary input). Numbers come back to the user before any limit is written.
- R2: Adopt the G5 precondition: step 5 must have lowered resident memory by >= 0.9 x 256 MiB, else G5 = "not observable" FAIL. Windows must be rerun after this change.
- R3: OrbStack docker is an accepted vehicle for the one-time Linux run.
- R4: Fix the ceyx_debug_idle_funnel_counters 7-vs-10 pointer mismatch in both callers (lib/perf/perf_driver.dart, tools/memgate/memgate.py).
- R5: Android F9: if the pre-registered shape sweep finds no qualifying shape, Android gets a recorded exception in the Windows form (re-proven each run).

## Rulings 2026-10-04 (later, memreclaim Task 6)

- G5 limit: ONE unified limit of 6.0x for every platform where step 5 runs (supersedes Windows 5.0); macOS stays N/A (step 5 unavailable). The resident-drop precondition (R2) stays.
- Android F9: recorded exception, same form as Windows. The allocator self-returns at free(); F9 asserts "not observable" and re-proves the basis every run. Basis artifact: docs/logs/2026-10-04/memreclaim-android-adb-fix.txt (sweep: freed-retained 3-12 MiB across 16/64 KiB shapes).
- Halide error handler: the approved design (docs/logs/2026-10-04/halide-error-handler-recommendation.md) proved unsafe on Vulkan: generated pipelines ignore failed GPU copies and segfault after a returning handler (evidence: docs/logs/2026-10-04/vulkan-segv-diagnosis.md). User ruled 2026-10-04: revert and park. Remediation options (ceyx pre-upload with rc checks / forked-runtime null guard + failure flag) are recorded in the diagnosis file for a future campaign.
- G5 criterion = absolute re-access <= 600 ms (user ruling 2026-10-04, supersedes 180 ms of the same day); with this value all three step-5 platforms' measured numbers (Linux ~164 ms, Windows 273 ms, Android 422 ms) are within the gate.
