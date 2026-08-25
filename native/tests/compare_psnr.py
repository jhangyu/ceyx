#!/usr/bin/env python3
# ---
# file_summary: >
#   帶合約檢查的兩檔案 PSNR 比對工具（rule.md Appendix D 強制工具）。
#   在計算任何 PSNR 前強制輸出 [Contract] PASS/FAIL；任何 FAIL 立即 exit(2)，
#   不繼續計算，避免格式不符時產生假性正確結果。
#
# supported_formats:
#   - headless raw binary (現行 pipeline 主格式)：
#       uint16 → Stage1/2/3 輸出 (e.g. lossless_stage3_6048x4024_3p.raw)
#       uint8  → Render 輸出   (e.g. lossless_render_6000x4000_3p.raw)
#   - legacy rendered.rgb：8-byte header (width:u32 height:u32) + uint8 RGB
#   - JPEG 參考圖（mode=jpeg，需 Pillow）
#
# constraints:
#   - 兩檔的 width/height/planes/pixel_size/layout 必須完全一致，不做靜默 resize
#   - numpy 未安裝時退化為純 Python 整體 PSNR（無 per-channel 統計）
#   - jpeg mode 需要 Pillow（pip3 install Pillow）
#   - exit codes：0=PASS / 1=一般錯誤 / 2=合約 FAIL
#
# usage:
#   python3 compare_psnr.py <file_a.raw> <file_b.raw>
#       # 從檔名自動解析 WxH_Pp（e.g. 6048x4024_3p）
#   python3 compare_psnr.py <file_a.raw> <file_b.raw> \
#       --width 6048 --height 4024 --planes 3 --pixel-size 2
#   python3 compare_psnr.py <rendered.raw> <ref.jpg> --mode jpeg
#
# contract_checks:
#   width / height / planes / pixel_size / layout / buffer_size(×2)
#
# functions:
#   - name: "_contract"
#     description: "輸出 [Contract] PASS/FAIL，FAIL 時設定全域旗標"
#     lines: "130-142"
#   - name: "abort_if_contract_failed"
#     description: "有任何合約 FAIL 時 exit(2)"
#     lines: "143-148"
#   - name: "parse_dims_from_path"
#     description: "從檔名 WxH_Pp 模式解析 (width, height, planes)"
#     lines: "156-163"
#   - name: "infer_pixel_size"
#     description: "由實際檔案大小推算 pixel_size（1 或 2）"
#     lines: "164-172"
#   - name: "run_raw_contracts"
#     description: "執行 7 項合約（width/height/planes/pixel_size/layout/buffer_size×2）"
#     lines: "209-273"
#   - name: "compute_psnr_per_channel_numpy"
#     description: "per-channel PSNR + MAE + maxAbs 座標（需 numpy）"
#     lines: "274-346"
#   - name: "compute_psnr_fallback"
#     description: "純 Python 整體 PSNR fallback（無 numpy）"
#     lines: "347-378"
#   - name: "run_jpeg_mode"
#     description: "jpeg 比對模式（file_a raw/rgb vs file_b JPEG）"
#     lines: "379-418"
#   - name: "resolve_raw_params"
#     description: "解析單一 raw 檔的 (w,h,planes,pixel_size)：CLI > 檔名 > 推算"
#     lines: "419-461"
#   - name: "run_raw_mode"
#     description: "raw 模式主流程（解析 → 合約 → 載入 → PSNR）"
#     lines: "462-502"
#   - name: "build_parser / main"
#     description: "CLI 入口"
#     lines: "503-562"
# ---
"""
compare_psnr.py — 帶合約檢查的畫質比對工具
Rule: rule.md Appendix D（強制工具之一）

用途：直接比較兩個 raw stage 檔案，在進行 PSNR 計算前強制執行合約檢查，
      避免格式不符時產生假性正確的比對結果。

支援格式：
  1. headless raw binary（現行 pipeline 主格式）
       - uint16: Stage1/2/3 輸出  (e.g. lossless_stage3_6048x4024_3p.raw)
       - uint8:  Render 輸出      (e.g. lossless_render_6000x4000_3p.raw)
       - 尺寸可自動從檔名解析（{W}x{H}_{P}p 模式），或由 CLI 明確指定
  2. legacy rendered.rgb（8-byte header + uint8 interleaved RGB）
       - 需加 --mode legacy-rgb
  3. JPEG 參考（搭配 --mode jpeg，需 Pillow）

合約檢查（per rule.md Appendix C）：
  在 PSNR 計算前輸出 [Contract] PASS 或 [Contract] FAIL。
  任何一項 FAIL 立即中止，不繼續計算。
  檢查項目：width / height / planes / pixel_size / layout / buffer_size

使用範例：
  # 自動從檔名解析尺寸（最常用）
  python3 compare_psnr.py \\
    lossless_stage3_post_warprectilinear_sdk.raw \\
    lossless_stage3_post_warprectilinear_halide_metal.raw

  # 明確指定（當檔名不含尺寸資訊時）
  python3 compare_psnr.py file_a.raw file_b.raw \\
    --width 6048 --height 4024 --planes 3 --pixel-size 2 --layout interleaved

  # 比較 render 輸出（uint8）
  python3 compare_psnr.py \\
    lossless_render_6000x4000_3p.raw \\
    lossy_render_6000x4000_3p.raw \\
    --pixel-size 1

  # Legacy rendered.rgb vs JPEG
  python3 compare_psnr.py output.rgb reference.jpg --mode jpeg
"""

import argparse
import math
import re
import struct
import sys
from pathlib import Path
from typing import Optional, Tuple


# ---------------------------------------------------------------------------
# 常數
# ---------------------------------------------------------------------------

PIXEL_SIZE_TO_DTYPE = {1: "uint8", 2: "uint16"}
PSNR_MAX_VAL = {1: 255.0, 2: 65535.0}

# ---------------------------------------------------------------------------
# 合約檢查輸出（per rule.md Appendix C）
# ---------------------------------------------------------------------------

_contract_failed = False


def _contract(label: str, passed: bool, detail: str = "") -> bool:
    """輸出 [Contract] PASS/FAIL，並在 FAIL 時設定全域旗標。"""
    global _contract_failed
    status = "PASS" if passed else "FAIL"
    msg = f"[Contract] {label}: {status}"
    if detail:
        msg += f"  ({detail})"
    print(msg)
    if not passed:
        _contract_failed = True
    return passed


def abort_if_contract_failed() -> None:
    if _contract_failed:
        print("\n[Contract] 合約檢查未通過，中止 PSNR 計算。")
        sys.exit(2)


# ---------------------------------------------------------------------------
# 檔名自動解析
# ---------------------------------------------------------------------------

_FILENAME_DIM_RE = re.compile(r"(\d+)x(\d+)_(\d+)p")


def parse_dims_from_path(path: Path) -> Optional[Tuple[int, int, int]]:
    """從檔名解析 (width, height, planes)，例如 6048x4024_3p → (6048, 4024, 3)。"""
    m = _FILENAME_DIM_RE.search(path.name)
    if m:
        return int(m.group(1)), int(m.group(2)), int(m.group(3))
    return None


def infer_pixel_size(path: Path, width: int, height: int, planes: int) -> Optional[int]:
    """根據實際檔案大小推算 pixel_size（1 或 2）。"""
    actual = path.stat().st_size
    for bps in (1, 2):
        if actual == width * height * planes * bps:
            return bps
    return None


# ---------------------------------------------------------------------------
# 載入函式
# ---------------------------------------------------------------------------

def load_raw(path: Path, width: int, height: int, planes: int, pixel_size: int) -> bytes:
    """載入 headless raw binary。"""
    return path.read_bytes()


def load_legacy_rgb(path: Path) -> Tuple[int, int, bytes]:
    """載入舊版 rendered.rgb（8-byte header）。"""
    data = path.read_bytes()
    if len(data) < 8:
        print(f"ERROR: {path.name} 太小，無法讀取 8-byte header")
        sys.exit(1)
    w = struct.unpack_from("<I", data, 0)[0]
    h = struct.unpack_from("<I", data, 4)[0]
    return w, h, data[8:]


def load_jpeg(path: Path) -> Tuple[int, int, bytes]:
    """載入 JPEG（需要 Pillow）。"""
    try:
        from PIL import Image
    except ImportError:
        print("ERROR: Pillow 未安裝。請執行：pip3 install Pillow")
        sys.exit(1)
    img = Image.open(path).convert("RGB")
    w, h = img.size
    return w, h, img.tobytes()


# ---------------------------------------------------------------------------
# 合約檢查：headless raw 模式
# ---------------------------------------------------------------------------

def run_raw_contracts(
    path_a: Path,
    path_b: Path,
    width_a: int,
    height_a: int,
    planes_a: int,
    pixel_size_a: int,
    layout_a: str,
    width_b: int,
    height_b: int,
    planes_b: int,
    pixel_size_b: int,
    layout_b: str,
) -> None:
    """執行所有合約檢查，任何 FAIL 立即標記；最後統一中止。"""
    print("\n--- Stage Contract Checks ---")

    # 1. width
    _contract("width",
              width_a == width_b,
              f"file_a={width_a} vs file_b={width_b}")

    # 2. height
    _contract("height",
              height_a == height_b,
              f"file_a={height_a} vs file_b={height_b}")

    # 3. planes
    _contract("planes",
              planes_a == planes_b,
              f"file_a={planes_a} vs file_b={planes_b}")

    # 4. pixel_size
    _contract("pixel_size",
              pixel_size_a == pixel_size_b,
              f"file_a={pixel_size_a} ({PIXEL_SIZE_TO_DTYPE.get(pixel_size_a,'?')}) "
              f"vs file_b={pixel_size_b} ({PIXEL_SIZE_TO_DTYPE.get(pixel_size_b,'?')})")

    # 5. layout
    _contract("layout",
              layout_a == layout_b,
              f"file_a={layout_a} vs file_b={layout_b}")

    # 6. buffer_size（以 file_a 的 w/h/p/bps 為期望值）
    expected_bytes = width_a * height_a * planes_a * pixel_size_a
    actual_a = path_a.stat().st_size
    actual_b = path_b.stat().st_size

    _contract("buffer_size file_a",
              actual_a == expected_bytes,
              f"actual={actual_a} expected={expected_bytes} "
              f"({width_a}×{height_a}×{planes_a}×{pixel_size_a})")

    _contract("buffer_size file_b",
              actual_b == expected_bytes,
              f"actual={actual_b} expected={expected_bytes} "
              f"({width_a}×{height_a}×{planes_a}×{pixel_size_a})")

    abort_if_contract_failed()


# ---------------------------------------------------------------------------
# PSNR 計算
# ---------------------------------------------------------------------------

def compute_psnr_per_channel_numpy(
    data_a: bytes,
    data_b: bytes,
    width: int,
    height: int,
    planes: int,
    pixel_size: int,
    layout: str,
    threshold: float = 36.0,
) -> None:
    """用 numpy 計算整體 + per-channel PSNR/MAE/maxAbs，輸出詳細統計。"""
    import numpy as np

    dtype = np.uint8 if pixel_size == 1 else np.uint16
    max_val = float(PSNR_MAX_VAL[pixel_size])

    arr_a = np.frombuffer(data_a, dtype=dtype)
    arr_b = np.frombuffer(data_b, dtype=dtype)

    if layout == "interleaved":
        # shape: (height, width, planes)
        img_a = arr_a.reshape(height, width, planes).astype(np.float64)
        img_b = arr_b.reshape(height, width, planes).astype(np.float64)
    else:
        # planar: shape (planes, height, width) → transpose to (height, width, planes)
        img_a = arr_a.reshape(planes, height, width).transpose(1, 2, 0).astype(np.float64)
        img_b = arr_b.reshape(planes, height, width).transpose(1, 2, 0).astype(np.float64)

    diff = img_a - img_b
    abs_diff = np.abs(diff)

    print("\n--- PSNR / Channel Statistics ---")
    print(f"{'Channel':<10} {'PSNR (dB)':<14} {'MAE':<12} {'maxAbs':<10} {'maxAbs Location (x,y)'}")
    print("-" * 70)

    channel_names = {1: ["Gray"], 3: ["R", "G", "B"], 4: ["R", "G", "B", "A"]}
    ch_names = channel_names.get(planes, [f"C{i}" for i in range(planes)])

    overall_mse_sum = 0.0
    for c in range(planes):
        ch_diff = diff[:, :, c]
        ch_abs = abs_diff[:, :, c]

        mse = np.mean(ch_diff ** 2)
        overall_mse_sum += mse
        psnr = (10.0 * math.log10(max_val ** 2 / mse)) if mse > 0.0 else math.inf
        mae = float(np.mean(ch_abs))
        max_abs = float(np.max(ch_abs))

        # 找最大差異座標 (x, y)
        flat_idx = int(np.argmax(ch_abs))
        max_y, max_x = divmod(flat_idx, width)

        psnr_str = f"{psnr:.2f}" if math.isfinite(psnr) else "999.00"
        name = ch_names[c] if c < len(ch_names) else f"C{c}"
        print(f"{name:<10} {psnr_str:<14} {mae:<12.3f} {max_abs:<10.0f} ({max_x},{max_y})")

    # Overall PSNR（跨所有 channel 的平均 MSE）
    overall_mse = overall_mse_sum / planes
    overall_psnr = (10.0 * math.log10(max_val ** 2 / overall_mse)) if overall_mse > 0.0 else math.inf
    overall_psnr_str = f"{overall_psnr:.2f}" if math.isfinite(overall_psnr) else "999.00"
    print("-" * 70)
    print(f"{'Overall':<10} {overall_psnr_str} dB")

    # Pass/Fail 判斷（threshold 由 --min-psnr 傳入，預設 36.0dB 保向後相容）
    print()
    passed = overall_psnr >= threshold or not math.isfinite(overall_psnr)
    status = "PASS" if passed else "FAIL"
    marker = "✅" if passed else "❌"
    print(f"  {marker} [{status}] PSNR {overall_psnr_str} dB "
          f"{'≥' if passed else '<'} {threshold} dB threshold")


def compute_psnr_fallback(
    data_a: bytes,
    data_b: bytes,
    pixel_size: int,
    threshold: float = 36.0,
) -> None:
    """純 Python fallback（無 numpy）。僅計算整體 PSNR，無 per-channel 統計。"""
    max_val = float(PSNR_MAX_VAL[pixel_size])

    if pixel_size == 1:
        samples_a = list(data_a)
        samples_b = list(data_b)
    else:
        import array as arr
        samples_a = list(arr.array("H", data_a))
        samples_b = list(arr.array("H", data_b))

    n = len(samples_a)
    mse = sum((a - b) ** 2 for a, b in zip(samples_a, samples_b)) / n
    psnr = (10.0 * math.log10(max_val ** 2 / mse)) if mse > 0.0 else math.inf
    psnr_str = f"{psnr:.2f}" if math.isfinite(psnr) else "999.00"

    print(f"\n  PSNR: {psnr_str} dB  (numpy 未安裝，僅輸出整體 PSNR)")
    passed = psnr >= threshold or not math.isfinite(psnr)
    status = "PASS" if passed else "FAIL"
    print(f"  [{'✅' if passed else '❌'}] [{status}] {psnr_str} dB vs {threshold} dB threshold")


# ---------------------------------------------------------------------------
# Legacy JPEG mode
# ---------------------------------------------------------------------------

def run_jpeg_mode(path_a: Path, path_b: Path, threshold: float = 36.0) -> None:
    """Legacy mode：比較 rendered.rgb 或 headless uint8 raw vs JPEG 參考圖。"""
    print(f"\n[Mode] jpeg  file_a={path_a.name}  file_b={path_b.name}")

    # 判斷 file_a 格式：8-byte header 或 headless
    if path_a.suffix.lower() == ".rgb":
        wa, ha, raw_a = load_legacy_rgb(path_a)
        print(f"  file_a (legacy-rgb): {wa}x{ha}")
    else:
        # 嘗試從檔名解析尺寸
        dims = parse_dims_from_path(path_a)
        if dims is None:
            print(f"ERROR: 無法從 '{path_a.name}' 解析尺寸，請改用 --mode raw 並指定 --width/--height/--planes")
            sys.exit(1)
        wa, ha, planes_a = dims
        raw_a = path_a.read_bytes()
        print(f"  file_a (raw): {wa}x{ha} planes={planes_a}")

    wb, hb, raw_b = load_jpeg(path_b)
    print(f"  file_b (jpeg): {wb}x{hb}")

    # 合約：維度必須完全一致，不做靜默 resize
    print("\n--- Stage Contract Checks ---")
    _contract("width",  wa == wb, f"file_a={wa} vs file_b (jpeg)={wb}")
    _contract("height", ha == hb, f"file_a={ha} vs file_b (jpeg)={hb}")
    _contract("pixel_size", True, "file_a=uint8 file_b=uint8 (jpeg decoded as uint8)")
    _contract("buffer_size file_a", len(raw_a) == wa * ha * 3, f"actual={len(raw_a)} expected={wa*ha*3}")
    _contract("buffer_size file_b", len(raw_b) == wb * hb * 3, f"actual={len(raw_b)} expected={wb*hb*3}")
    abort_if_contract_failed()

    try:
        compute_psnr_per_channel_numpy(raw_a, raw_b, wa, ha, 3, 1, "interleaved", threshold=threshold)
    except ImportError:
        compute_psnr_fallback(raw_a, raw_b, 1, threshold=threshold)


# ---------------------------------------------------------------------------
# Raw mode 主流程
# ---------------------------------------------------------------------------

def resolve_raw_params(
    path: Path,
    explicit_width: Optional[int],
    explicit_height: Optional[int],
    explicit_planes: Optional[int],
    explicit_pixel_size: Optional[int],
    label: str,
) -> Tuple[int, int, int, int]:
    """
    解析單一 raw 檔案的 (width, height, planes, pixel_size)。
    優先級：CLI 明確指定 > 檔名自動解析 > 從檔案大小推算 pixel_size。
    """
    # width/height/planes：CLI 優先，否則從檔名解析
    auto = parse_dims_from_path(path)
    if explicit_width is not None and explicit_height is not None and explicit_planes is not None:
        w, h, p = explicit_width, explicit_height, explicit_planes
    elif auto is not None:
        w, h, p = auto
        if explicit_width is not None:
            w = explicit_width
        if explicit_height is not None:
            h = explicit_height
        if explicit_planes is not None:
            p = explicit_planes
    else:
        print(f"ERROR [{label}]: 無法從 '{path.name}' 解析尺寸，請明確指定 --width / --height / --planes")
        sys.exit(1)

    # pixel_size：CLI 優先，否則從檔案大小推算
    if explicit_pixel_size is not None:
        bps = explicit_pixel_size
    else:
        bps = infer_pixel_size(path, w, h, p)
        if bps is None:
            print(f"ERROR [{label}]: 無法從檔案大小推算 pixel_size（{path.stat().st_size} bytes）。"
                  f"請明確指定 --pixel-size 1 或 --pixel-size 2")
            sys.exit(1)

    print(f"  {label}: {path.name}  {w}x{h} planes={p} pixel_size={bps} "
          f"({PIXEL_SIZE_TO_DTYPE.get(bps,'?')})")
    return w, h, p, bps


def run_raw_mode(args: argparse.Namespace, threshold: float = 36.0) -> None:
    path_a = Path(args.file_a)
    path_b = Path(args.file_b)

    print(f"[Mode] raw")
    print(f"[Files]")

    w_a, h_a, p_a, bps_a = resolve_raw_params(
        path_a, args.width, args.height, args.planes, args.pixel_size, "file_a"
    )
    w_b, h_b, p_b, bps_b = resolve_raw_params(
        path_b, args.width, args.height, args.planes, args.pixel_size, "file_b"
    )

    layout_a = args.layout
    layout_b = args.layout

    # 合約檢查
    run_raw_contracts(
        path_a, path_b,
        w_a, h_a, p_a, bps_a, layout_a,
        w_b, h_b, p_b, bps_b, layout_b,
    )

    # 載入資料（合約已通過）
    data_a = load_raw(path_a, w_a, h_a, p_a, bps_a)
    data_b = load_raw(path_b, w_b, h_b, p_b, bps_b)

    # PSNR 計算
    try:
        import numpy  # noqa: F401
        compute_psnr_per_channel_numpy(data_a, data_b, w_a, h_a, p_a, bps_a, layout_a, threshold=threshold)
    except ImportError:
        print("  (numpy 未安裝，使用純 Python fallback，無 per-channel 統計)")
        compute_psnr_fallback(data_a, data_b, bps_a, threshold=threshold)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="compare_psnr.py — 帶合約檢查的畫質比對工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("file_a", help="第一個輸入檔案（基準）")
    p.add_argument("file_b", help="第二個輸入檔案（待比對）")

    mode_group = p.add_argument_group("模式")
    mode_group.add_argument(
        "--mode",
        choices=["raw", "jpeg"],
        default="raw",
        help="比對模式：raw（預設）或 jpeg（file_b 為 JPEG 參考圖）",
    )

    raw_group = p.add_argument_group("raw 模式參數（當檔名無法自動解析時使用）")
    raw_group.add_argument("--width",      type=int, help="影像寬度（pixels）")
    raw_group.add_argument("--height",     type=int, help="影像高度（pixels）")
    raw_group.add_argument("--planes",     type=int, help="色彩平面數（1=灰階, 3=RGB）")
    raw_group.add_argument(
        "--pixel-size", type=int, choices=[1, 2],
        help="每個樣本的位元組數（1=uint8, 2=uint16）；省略時從檔案大小自動推算",
    )
    raw_group.add_argument(
        "--layout",
        choices=["interleaved", "planar"],
        default="interleaved",
        help="記憶體佈局（預設：interleaved）",
    )
    p.add_argument(
        "--min-psnr",
        type=float,
        default=36.0,
        metavar="DB",
        help="最低可接受 PSNR（dB）。預設 36.0（Phase 2 遺留值，向後相容）。"
             "run_decode_matrix.py 對 Stage3 傳 100、Stage4 傳 75、999路徑傳 999。",
    )
    return p


def main() -> int:
    args = build_parser().parse_args()

    path_a = Path(args.file_a)
    path_b = Path(args.file_b)

    for p in (path_a, path_b):
        if not p.exists():
            print(f"ERROR: 找不到檔案：{p}")
            return 1

    threshold = float(args.min_psnr)

    print("=" * 65)
    print("  compare_psnr.py — Stage Contract + PSNR/Channel Statistics")
    print("=" * 65)

    if args.mode == "jpeg":
        run_jpeg_mode(path_a, path_b, threshold=threshold)
    else:
        run_raw_mode(args, threshold=threshold)

    print()
    return 0 if not _contract_failed else 2


if __name__ == "__main__":
    sys.exit(main())
