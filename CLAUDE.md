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

### 4-Stage DNG Pipeline (`dng_pipeline_v2.cpp`)

| Stage | Role | Technology |
|-------|------|------------|
| 1 | Parse DNG metadata, decompress Bayer tiles (LJPEG) | Adobe DNG SDK + libjpeg-turbo |
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
        - NativeFinalizer calls dng_free_halide_buffer() on GC
        ↓
dng_image_widget.dart  (Flutter render)
```

`DngResult` struct must stay byte-exact between `dng_ffi_api.h` and `dng_bindings.dart`.

### Halide Generators (AOT, compiled at CMake build time)

- `DngDemosaicWarpGenerator` → `dng_demosaic_warp.a` (Stage 3 fused)
- `DngDemosaicGenerator` → `dng_demosaic_bilinear.a` (Stage 3 fallback)
- `DngWarpGenerator` → `rectilinear_warp.a` (standalone warp fallback)
- `DngRenderGenerator` → `dng_render_stage4.a` (Stage 4)

## Key Gotchas (from memory.md)

- **Device handoff crop origin**: After `src_buf.crop()`, must mutate `raw_buffer()->dim.min = 0` to match generator's hard-coded `clamp(x, 0, ext-1)`. Do NOT use `set_min`/`translate` (triggers `device_deallocate`).
- **Halide device handoff**: Pass `device`-dirty `halide_buffer_t*` between AOT kernels without `copy_to_host`; Metal serial queue guarantees ordering.
- **Adobe DNG SDK `AutoPtr`**: lvalue-only; use `.Reset(.Release())` instead of direct assignment.
- **`run_decode_matrix.py`** only exercises the fallback path; device handoff validation requires FFI entry (`benchmark_zero_copy.dart`).

## Documentation Convention

All task logs go in `docs/logs/YYYY-MM-DD/Task_<name>.md`. Do not create ad-hoc files at the project root.

## Subagent / Bash 執行規範（強制）

主 agent 與 subagent 在跑測試、診斷、驗證腳本時必須遵守，避免每次都觸發使用者手動 approve。

1. **禁止 inline script**：不得使用 `python3 <<EOF`、`python3 -c "..."`、`bash -c "..."`、`zsh -c "..."` 這類 heredoc / 內嵌字串執行。
2. **禁止 env 前綴**：不得把環境變數直接前綴在 command（`FOO=1 ./test`），首 token 一變化白名單就匹配不到。
   - 正確做法：在腳本內 `os.environ.setdefault(...)`，或寫成 wrapper script 後再執行。
3. **一次性腳本必須落檔**：診斷或驗證腳本一律寫到 `dng_processor/native/scripts/tmp/<name>.py`（gitignored 區），再用 `python3 dng_processor/native/scripts/tmp/<name>.py` 執行。不要 inline。禁止使用 `/tmp/` —— 所有操作都在專案資料夾內進行。
4. **優先使用既有測試入口**（已在白名單）：
   - `python3 dng_processor/native/scripts/build_native_watchdog.py [...]`
   - `python3 dng_processor/native/tests/run_decode_matrix.py [...]`
   - `python3 dng_processor/native/tests/compare_psnr.py [...]`
   - `./dng_processor/native/build/test_decode [...]`
   - `./dng_processor/native/build/test_warp_rectilinear_halide [...]`
5. **主 agent 分派 subagent 時**，必須在 prompt 內覆述上述規範，subagent 不得繞過。
6. **Git 危險操作禁令**：subagent 一律禁止執行 `git stash`、`git reset`、`git checkout`（除非是讀檔，如 `git checkout <sha> -- <file>`）、`git clean`、`git rm`、`git restore` 等可能破壞 worktree 或 reflog 的命令。允許：`git status`、`git diff`、`git log`、`git show`、`git cat-file`、`git fsck` 等純讀取操作。任何寫操作必須回主 agent 拍板。歷史教訓：2026-05-18 D-B agent 用 stash + drop 把 Sprint C tracked 修改全部丟失，靠任務日誌手動重建（見 memory.md Gotcha #59）。

