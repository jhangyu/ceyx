# 測試資源索引（Test Resources Index）

本檔案收錄所有 active 測試資源的位置與用途說明。

## 測試腳本

| 檔案 | 用途 | 備註 |
|------|------|------|
| `run_decode_matrix.py` | 4-case regression matrix 主入口 | 白名單 script |
| `compare_psnr.py` | PSNR 比對工具 | 被 run_decode_matrix.py 內部呼叫 |
| `scripts/build_native_watchdog.py` | Native 編譯 watchdog | 白名單 script |

## 測試執行檔（build/ 目錄）

| 檔案 | 用途 |
|------|------|
| `test_decode` | 四階段解碼測試主入口 |
| `test_color_accuracy` | 色彩準確度測試 |
| `test_cfa_phase` | CFA 相位四相（RGGB/BGGR/GRBG/GBRG）合成圖檢查；同時覆蓋 Halide AOT kernel 與 CPU 參考 demosaic。由 `run_decode_matrix.py` 自動執行 |
| `test_cfa_color` | 對真實 DNG 解碼後做像素顏色斷言（藍天必須 B ≫ R）；BGGR 樣本為外部路徑，缺檔時 matrix 印 SKIP |

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
