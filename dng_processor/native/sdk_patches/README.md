# SDK Patches

Patches applied to `third_party/dng_sdk/source/` (the LIVE compiled vendor tree).

These diffs are maintained for traceability and to aid future SDK upgrades.
For the full patch index (including timing instrumentation patches), see
`docs/reference/sdk_patch_index.md`.

## Bridge-Related Patches

### 1. dng_opcode_list.cpp — Halide MapPolynomial hook (Stage 2)

**File**: `dng_opcode_list_cpp.patch`

Inserts two Halide GPU dispatch calls in `dng_opcode_list::Apply()`:
- `halide_try_dispatch_opcode2_batch()` — batched 3-plane MapPolynomial GPU dispatch
- `halide_try_dispatch_opcode2()` — single-op fallback

Both hook into the Stage2 OpcodeList2 Halide bridge (`dng_opcodelist2_halide.h`).
Also adds `#include "dng_opcodelist2_halide.h"` at the top.

Introduced in Phase 10 Sprint C3/C4.

### 2. dng_lens_correction.h — WarpParams accessor

**File**: `dng_lens_correction_h.patch`

Adds a public `WarpParams()` const accessor to `dng_opcode_WarpRectilinear`,
returning a const reference to the protected `fWarpParams` member. Required by
`dng_warp_halide.cpp` to extract warp polynomial coefficients for the Halide
Stage3 fused demosaic+warp kernel without friend access.

Discovered during W6 H-3 bridge source move (previously resolved implicitly
via the `dng_sdk_custom/source/` include path overlay).

## Applying

From the `dng_processor/native/` directory:

    git apply sdk_patches/dng_lens_correction_h.patch
    git apply sdk_patches/dng_opcode_list_cpp.patch

## Maintenance

- When upgrading the Adobe DNG SDK, replay these patches and re-run the
  decode matrix (`run_decode_matrix.py --repeat 3`) before trusting results.
- Update `docs/reference/sdk_patch_index.md` when adding or modifying patches.
