# 測試資源索引（Test Resources Index）

測試清單不再手寫維護（舊的手寫表已過期）：測試執行檔以 `native/cmake/tests.cmake` 的 `add_executable` 為準；測試入口為 `native/tests/run_decode_matrix.py` 與 `native/tests/run_raw_matrix.py`。

## Halide AOT 產出（halide_generated/ 目錄）

### Active AOT

| 檔案 | 用途 |
|------|------|
| `dng_demosaic_warp.a/h` | Stage3 fused（fused demosaic + warp） |
| `dng_render_stage4.a/h` | Stage4（Camera→sRGB + tone mapping） |
| `rectilinear_warp.a/h` | Standalone warp baseline |

### Legacy / Reference AOT

| 檔案 | 用途 |
|------|------|
| `dng_demosaic_bilinear.a/h` | Stage3 fallback（production correctness path） |
| `dng_pipeline.a/h` | Phase 5 legacy pipeline |
