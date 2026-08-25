# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Session Startup

每次新對話開始，依序讀取這些文件再開始工作：

1. `rule.md` — 開發 SOP（含 Unified Task Log 推進法）
2. `memory.md` — 架構決策與避坑記錄
3. `task.md` — 任務清單，重點看頂部「🔴 現在進行中」區塊
4. `handover.md` — 上一輪的中斷點與下一步
5. `plan.md` — 高階里程碑與 Phase 狀態
6. `file_index.md` — 全專案檔案地圖（**先查這裡，不要全局搜尋**）

讀完後輸出一份簡短「現狀確認」，說明對當前斷點的理解，再等指示。

## Build Commands

### Native C++ (preferred)

```bash
# 首次 clone 後必跑一次：抓 vendored Halide v21 binary distribution
# （~540MB，未 tracked——2026-07-05 起因 GitHub 100MB 單檔限制移出 git）
dng_processor/native/scripts/fetch_halide_v21_dist.sh

# 完整 configure + build（預設 target: test_decode）
python3 dng_processor/native/scripts/build_native_watchdog.py

# 指定 target
python3 dng_processor/native/scripts/build_native_watchdog.py --target dng_decoder_native

# 跳過 configure 加速迭代
python3 dng_processor/native/scripts/build_native_watchdog.py --skip-configure --target test_decode
```

### Manual CMake

```bash
cd dng_processor/native
cmake -S . -B build
cmake --build build --target <target> -j$(nproc)
```

### Flutter

```bash
cd dng_processor
flutter run          # macOS app
flutter build macos  # release build
```

## Test Commands

```bash
# 4-stage decode + PSNR
./dng_processor/native/build/test_decode image_samples/lossless_dng_sample.dng

# 4-case regression matrix (lossless/lossy × stage1/full)
python3 dng_processor/native/tests/run_decode_matrix.py
python3 dng_processor/native/tests/run_decode_matrix.py --repeat 3

# FFI + device-handoff harness cases (production C ABI + Metal device handoff
# gate). Auto-enabled by run_decode_matrix.py whenever the default binaries
# below exist — no flag needed once both targets are built:
python3 dng_processor/native/scripts/build_native_watchdog.py --skip-configure --target dng_ffi_harness
python3 dng_processor/native/scripts/build_native_watchdog.py --skip-configure --target test_device_handoff
python3 dng_processor/native/tests/run_decode_matrix.py --repeat 3
# `dng_ffi_harness` (dng_processor/native/tests/dng_ffi_harness.cpp) drives the
# extern "C" FFI entry (dng_decode_and_process) directly, gating contract
# checks and RGB byte-exact match.
# `test_device_handoff` (dng_processor/native/tests/test_device_handoff.cpp)
# calls decode_to_rgb directly and gates the Metal device-handoff PSNR (ON vs
# OFF) for the fused Stage3→Stage4 path.
# To point at non-default binaries or opt out explicitly:
#   --ffi-harness <path> / --no-ffi-harness
#   --device-handoff-harness <path> / --no-device-handoff-harness

# PSNR comparison of two raw buffers
python3 dng_processor/native/tests/compare_psnr.py \
  --ref lossless_stage3_6048x4024_1p.raw \
  --test halide_demosaic_output.raw \
  --width 6048 --height 4024 --planes 3

# Dart FFI smoke test
cd dng_processor
dart run bin/benchmark_zero_copy.dart image_samples/lossless_dng_sample.dng
dart run bin/benchmark_preview.dart image_samples/lossless_dng_sample.dng

# Flutter widget tests
flutter test
```

## Architecture

### 4-Stage DNG Pipeline (`dng_pipeline.cpp`)

| Stage | Role | Technology |
|-------|------|------------|
| 1 | Parse DNG metadata, decompress Bayer tiles (LJPEG) | Adobe DNG SDK + libjpeg |
| 2 | OpcodeList2: linearization, black subtraction, pre-demosaic lens correction | Adobe DNG SDK |
| 3 | Demosaic Bayer→RGB + fused WarpRectilinear (OpcodeList3) | Halide AOT (Metal GPU) |
| 4 | Camera→sRGB color matrix, tone mapping, 8-bit RGBA encode | Halide AOT (Metal GPU) |

Stages 3–4 use Halide AOT kernels compiled at build time (`dng_demosaic_warp.a`, `dng_render_stage4.a`) targeting `host-metal-no_asserts-no_bounds_query`. Metal dispatch is implicit via Halide's backend.

### FFI Bridge (C++ → Dart)

```
dng_ffi_api.cpp  (extern "C" API)
  └── libdng_decoder_native.dylib
        ↓
dng_bindings.dart  (dart:ffi struct + lookupFunction)
  └── dng_decoder_service.dart  (DngDecoderService.decode)
        - Zero-copy: wraps native RGBA buffer as Dart typed list (no memcpy)
        - NativeFinalizer calls dng_free_rgba_buffer() on GC
        ↓
dng_image_widget.dart  (Flutter render)
```

`DngResult` struct must stay byte-exact between `dng_ffi_api.h` and `dng_bindings.dart`.

### Halide Generators (AOT, compiled at CMake build time)

- `DngDemosaicWarpGenerator` → `dng_demosaic_warp.a` (Stage 3 fused)
- `DngDemosaicGenerator` → `dng_demosaic_bilinear.a` (Stage 3 fallback)
- `RectilinearWarpGenerator` → `rectilinear_warp.a` (standalone warp fallback)
- `DngRenderGenerator` → `dng_render_stage4.a` (Stage 4)

## Key Gotchas (from memory.md)

- **Device handoff crop origin**: After `src_buf.crop()`, must mutate `raw_buffer()->dim.min = 0` to match generator's hard-coded `clamp(x, 0, ext-1)`. Do NOT use `set_min`/`translate` (triggers `device_deallocate`).
- **Halide device handoff**: Pass `device`-dirty `halide_buffer_t*` between AOT kernels without `copy_to_host`; Metal serial queue guarantees ordering.
- **Adobe DNG SDK `AutoPtr`**: lvalue-only; use `.Reset(.Release())` instead of direct assignment.
- **`run_decode_matrix.py`** without harness flags only exercises the fallback path; the built-in `dng_ffi_harness` / `test_device_handoff` harness cases (auto-enabled when their binaries exist, see Test Commands) or `benchmark_zero_copy.dart` are required to validate the FFI + device-handoff (fused Stage3→Stage4) path.

## Documentation Convention

All task logs go in `docs/logs/YYYY-MM-DD/Task_<name>.md`. Do not create ad-hoc files at the project root.

### Subagent / Bash 執行規範

詳見 [rule.md Appendix D](rule.md#appendix-d-測試與編譯腳本強制規範-2026-04-10-新增) 與 [rule.md Appendix C](rule.md#appendix-c-測試契約檢查規則-2026-04-09-新增)。

**關鍵摘要（同步維護於 rule.md）**：
- 禁止 inline script / heredoc / env 前綴 / git 寫操作
- 一次性腳本落檔 `dng_processor/native/scripts/tmp/`，禁止 `/tmp/`
- 既有測試入口（build_native_watchdog / run_decode_matrix / compare_psnr）優先

