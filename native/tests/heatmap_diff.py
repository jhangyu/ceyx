#!/usr/bin/env python3
# ---
# file_summary: >
#   差分熱力圖工具：比對兩個 raw stage 檔案，輸出視覺化 diff PNG。
#   Contract-first：在產生 heatmap 前強制執行合約檢查，FAIL 立即 exit(2)。
#
# supported_formats:
#   - headless raw binary：
#       uint16 (pixel_size=2) → Stage1/2/3 輸出
#       uint8  (pixel_size=1) → Render 輸出
#   - interleaved (預設) 或 planar 佈局
#
# constraints:
#   - 兩檔的 width/height/planes/pixel_size/layout 必須完全一致
#   - numpy + matplotlib 必須安裝
#   - exit codes：0=OK / 1=一般錯誤 / 2=合約 FAIL
#
# usage:
#   python3 heatmap_diff.py <ref.raw> <test.raw> --output diff.png
#   python3 heatmap_diff.py --width 6048 --height 4024 --planes 3 \
#       ref.raw test.raw --output diff.png --plane -1 --colormap hot
#
# contract_checks:
#   width / height / planes / pixel_size / layout / buffer_size(×2)
#
# functions:
#   - name: "_contract / abort_if_contract_failed"
#     lines: "75-95"
#   - name: "parse_dims_from_path / infer_pixel_size"
#     lines: "98-121"
#   - name: "resolve_raw_params"
#     lines: "124-165"
#   - name: "run_contracts"
#     lines: "168-217"
#   - name: "load_raw / compute_diff_stats"
#     lines: "220-273"
#   - name: "render_heatmap"
#     lines: "276-339"
#   - name: "resolve_output_path"
#     lines: "342-356"
#   - name: "build_parser / main"
#     lines: "359-431"
# ---
"""
heatmap_diff.py — 帶合約檢查的差分熱力圖工具

用途：比對兩個 raw stage 檔案（同維度、同 pixel_size、同 layout），
      輸出 per-pixel 絕對差異的彩色熱力圖 PNG 並列印 per-channel 統計。

範例：
  python3 heatmap_diff.py lossless_stage3_6048x4024_3p_sdk.raw \\
      lossless_stage3_6048x4024_3p_halide.raw --output diff.png

  python3 heatmap_diff.py a.raw b.raw --output diff.png --plane -1
  python3 heatmap_diff.py a.raw b.raw --output out_dir/ --colormap hot
"""

import argparse
import math
import re
import sys
from pathlib import Path
from typing import Optional, Tuple


PIXEL_SIZE_TO_DTYPE = {1: "uint8", 2: "uint16"}
PSNR_MAX_VAL = {1: 255.0, 2: 65535.0}
CHANNEL_NAMES = {1: ["Gray"], 3: ["R", "G", "B"], 4: ["R", "G", "B", "A"]}
VALID_COLORMAPS = ("viridis", "hot", "magma", "inferno", "plasma")


# ---------------------------------------------------------------------------
# Contract helpers
# ---------------------------------------------------------------------------

_contract_failed = False


def _contract(label: str, passed: bool, detail: str = "") -> bool:
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
        print("\n[Contract] 合約檢查未通過，中止 heatmap 產生。")
        sys.exit(2)


# ---------------------------------------------------------------------------
# Filename parsing
# ---------------------------------------------------------------------------

_FILENAME_DIM_RE = re.compile(r"(\d+)x(\d+)_(\d+)p")


def parse_dims_from_path(path: Path) -> Optional[Tuple[int, int, int]]:
    m = _FILENAME_DIM_RE.search(path.name)
    if m:
        return int(m.group(1)), int(m.group(2)), int(m.group(3))
    return None


def infer_pixel_size(path: Path, width: int, height: int, planes: int) -> Optional[int]:
    actual = path.stat().st_size
    for bps in (1, 2):
        if actual == width * height * planes * bps:
            return bps
    return None


# ---------------------------------------------------------------------------
# Param resolution
# ---------------------------------------------------------------------------

def resolve_raw_params(
    path: Path,
    explicit_width: Optional[int],
    explicit_height: Optional[int],
    explicit_planes: Optional[int],
    explicit_pixel_size: Optional[int],
    label: str,
) -> Tuple[int, int, int, int]:
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
        print(f"ERROR [{label}]: 無法從 '{path.name}' 解析尺寸，請指定 --width/--height/--planes")
        sys.exit(1)

    if explicit_pixel_size is not None:
        bps = explicit_pixel_size
    else:
        bps = infer_pixel_size(path, w, h, p)
        if bps is None:
            print(f"ERROR [{label}]: 無法從檔案大小推算 pixel_size（{path.stat().st_size} bytes），"
                  f"請明確指定 --pixel-size 1 或 2")
            sys.exit(1)

    print(f"  {label}: {path.name}  {w}x{h} planes={p} pixel_size={bps} "
          f"({PIXEL_SIZE_TO_DTYPE.get(bps, '?')})")
    return w, h, p, bps


# ---------------------------------------------------------------------------
# Contract execution
# ---------------------------------------------------------------------------

def run_contracts(
    path_ref: Path,
    path_test: Path,
    w_ref: int, h_ref: int, p_ref: int, bps_ref: int, layout_ref: str,
    w_test: int, h_test: int, p_test: int, bps_test: int, layout_test: str,
) -> None:
    print("\n--- Stage Contract Checks ---")

    _contract("width",  w_ref == w_test, f"ref={w_ref} vs test={w_test}")
    _contract("height", h_ref == h_test, f"ref={h_ref} vs test={h_test}")
    _contract("planes", p_ref == p_test, f"ref={p_ref} vs test={p_test}")
    _contract("pixel_size",
              bps_ref == bps_test,
              f"ref={bps_ref} ({PIXEL_SIZE_TO_DTYPE.get(bps_ref,'?')}) "
              f"vs test={bps_test} ({PIXEL_SIZE_TO_DTYPE.get(bps_test,'?')})")
    _contract("layout", layout_ref == layout_test, f"ref={layout_ref} vs test={layout_test}")

    expected_bytes = w_ref * h_ref * p_ref * bps_ref
    actual_ref = path_ref.stat().st_size
    actual_test = path_test.stat().st_size

    _contract("buffer_size ref",
              actual_ref == expected_bytes,
              f"actual={actual_ref} expected={expected_bytes} "
              f"({w_ref}x{h_ref}x{p_ref}x{bps_ref})")
    _contract("buffer_size test",
              actual_test == expected_bytes,
              f"actual={actual_test} expected={expected_bytes} "
              f"({w_ref}x{h_ref}x{p_ref}x{bps_ref})")

    abort_if_contract_failed()


# ---------------------------------------------------------------------------
# Load + diff stats
# ---------------------------------------------------------------------------

def load_raw(path: Path, w: int, h: int, planes: int, bps: int, layout: str):
    import numpy as np
    dtype = np.uint8 if bps == 1 else np.uint16
    arr = np.frombuffer(path.read_bytes(), dtype=dtype)
    if layout == "interleaved":
        img = arr.reshape(h, w, planes)
    else:
        img = arr.reshape(planes, h, w).transpose(1, 2, 0)
    return img.astype(np.float64)


def compute_diff_stats(img_ref, img_test, bps: int, planes: int) -> dict:
    import numpy as np
    max_val = float(PSNR_MAX_VAL[bps])
    abs_diff = np.abs(img_ref - img_test)
    ch_names = CHANNEL_NAMES.get(planes, [f"C{i}" for i in range(planes)])

    stats = {"channels": [], "abs_diff": abs_diff, "ch_names": ch_names}
    for c in range(planes):
        ch = abs_diff[:, :, c]
        mse = float(np.mean(ch ** 2))
        psnr = (10.0 * math.log10(max_val ** 2 / mse)) if mse > 0.0 else math.inf
        stats["channels"].append({
            "name": ch_names[c],
            "max_abs": float(np.max(ch)),
            "mean_abs": float(np.mean(ch)),
            "non_zero": int(np.count_nonzero(ch)),
            "psnr": psnr,
            "p999": float(np.percentile(ch, 99.9)),
        })
    return stats


# ---------------------------------------------------------------------------
# Heatmap rendering
# ---------------------------------------------------------------------------

def render_heatmap(
    abs_diff,
    plane: int,
    planes: int,
    ch_names,
    bps: int,
    colormap: str,
    clamp_max: Optional[float],
    output_path: Path,
    title_extra: str,
    channel_stats: list,
) -> None:
    import numpy as np
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("ERROR: matplotlib 未安裝。請執行：pip3 install matplotlib")
        sys.exit(1)

    if plane == -1:
        data = np.max(abs_diff, axis=2)
        plane_label = "max(R,G,B)" if planes == 3 else f"max({planes}ch)"
        psnr_repr = min((s["psnr"] for s in channel_stats), default=math.inf)
    else:
        data = abs_diff[:, :, plane]
        plane_label = ch_names[plane] if plane < len(ch_names) else f"C{plane}"
        psnr_repr = channel_stats[plane]["psnr"]

    max_val = float(data.max()) if data.size > 0 else 0.0
    p999 = float(np.percentile(data, 99.9)) if data.size > 0 else 0.0

    if clamp_max is None:
        vmax = p999 if p999 > 0.0 else (max_val if max_val > 0.0 else 1.0)
    else:
        vmax = float(clamp_max)
    vmax = max(vmax, 1e-9)

    h, w = data.shape
    fig_w = max(6.0, min(16.0, w / 600.0 + 4.0))
    fig_h = max(4.0, min(12.0, h / 600.0 + 1.5))

    fig, ax = plt.subplots(figsize=(fig_w, fig_h), dpi=120)
    im = ax.imshow(data, cmap=colormap, vmin=0.0, vmax=vmax, interpolation="nearest")
    ax.set_xlabel("x")
    ax.set_ylabel("y")

    psnr_str = f"{psnr_repr:.2f}dB" if math.isfinite(psnr_repr) else "inf"
    title = (f"|ref - test|, plane={plane_label}, max={max_val:.0f}, "
             f"p99.9={p999:.1f}, PSNR={psnr_str}")
    if title_extra:
        title += f"\n{title_extra}"
    ax.set_title(title, fontsize=10)

    cbar = fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
    cbar.set_label(f"|diff|  (clamp_max={vmax:.2f}, scale=0..{int(PSNR_MAX_VAL[bps])})")

    fig.tight_layout()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=120, bbox_inches="tight")
    plt.close(fig)


# ---------------------------------------------------------------------------
# Output path resolution
# ---------------------------------------------------------------------------

def resolve_output_path(raw_output: str, ref_path: Path, test_path: Path, plane: int, ch_names) -> Path:
    p = Path(raw_output)
    is_dir_hint = raw_output.endswith("/") or raw_output.endswith("\\") or (p.exists() and p.is_dir())
    if is_dir_hint:
        if plane == -1:
            plane_tag = "all"
        else:
            name = ch_names[plane] if plane < len(ch_names) else f"C{plane}"
            plane_tag = f"{plane}_{name}"
        out_name = f"{ref_path.stem}_vs_{test_path.stem}_plane{plane_tag}.png"
        return p / out_name
    if not p.suffix:
        p = p.with_suffix(".png")
    return p


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="heatmap_diff.py — 帶合約檢查的差分熱力圖工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("ref",  help="參考 raw 檔（baseline）")
    p.add_argument("test", help="待比對 raw 檔")
    p.add_argument("--output", required=True, help="輸出 PNG 路徑（檔案或目錄）")

    p.add_argument("--width",  type=int, help="影像寬度（pixels）")
    p.add_argument("--height", type=int, help="影像高度（pixels）")
    p.add_argument("--planes", type=int, help="色彩平面數（1=Gray, 3=RGB, 4=RGBA）")
    p.add_argument("--pixel-size", type=int, choices=[1, 2],
                   help="每樣本位元組數（1=uint8, 2=uint16）；省略時從檔案大小推算")
    p.add_argument("--layout", choices=["interleaved", "planar"], default="interleaved",
                   help="記憶體佈局（預設：interleaved）")

    p.add_argument("--plane", type=int, default=0,
                   help="要視覺化的 channel：0/1/2 或 -1=所有 channel 取 max（預設 0）")
    p.add_argument("--colormap", choices=list(VALID_COLORMAPS), default="viridis",
                   help="色彩映射（預設：viridis）")
    p.add_argument("--clamp-max", type=float, default=None,
                   help="色彩映射上限；省略時使用 99.9th percentile（避免 outlier 撐爆色域）")
    return p


def main() -> int:
    args = build_parser().parse_args()

    ref_path = Path(args.ref)
    test_path = Path(args.test)
    for path in (ref_path, test_path):
        if not path.exists():
            print(f"ERROR: 找不到檔案：{path}")
            return 1

    try:
        import numpy  # noqa: F401
    except ImportError:
        print("ERROR: numpy 未安裝。請執行：pip3 install numpy")
        return 1

    print("=" * 65)
    print("  heatmap_diff.py — Stage Contract + Diff Heatmap")
    print("=" * 65)
    print("[Files]")

    w_r, h_r, p_r, bps_r = resolve_raw_params(
        ref_path, args.width, args.height, args.planes, args.pixel_size, "ref"
    )
    w_t, h_t, p_t, bps_t = resolve_raw_params(
        test_path, args.width, args.height, args.planes, args.pixel_size, "test"
    )

    run_contracts(
        ref_path, test_path,
        w_r, h_r, p_r, bps_r, args.layout,
        w_t, h_t, p_t, bps_t, args.layout,
    )

    if args.plane != -1 and (args.plane < 0 or args.plane >= p_r):
        print(f"ERROR: --plane={args.plane} 超出有效範圍 [0, {p_r - 1}] 或 -1")
        return 1

    img_ref  = load_raw(ref_path,  w_r, h_r, p_r, bps_r, args.layout)
    img_test = load_raw(test_path, w_t, h_t, p_t, bps_t, args.layout)

    stats = compute_diff_stats(img_ref, img_test, bps_r, p_r)

    print("\n--- Channel Diff Statistics ---")
    print(f"[HeatmapDiff] ref={ref_path.name} test={test_path.name}")
    for ch in stats["channels"]:
        psnr_str = f"{ch['psnr']:.2f}dB" if math.isfinite(ch["psnr"]) else "999.00dB"
        print(f"[HeatmapDiff] Channel {ch['name']:<4}: "
              f"maxAbs={ch['max_abs']:<7.0f} "
              f"meanAbs={ch['mean_abs']:<8.3f} "
              f"nonZero={ch['non_zero']:<10d} "
              f"p99.9={ch['p999']:<7.2f} "
              f"PSNR={psnr_str}")

    output_path = resolve_output_path(args.output, ref_path, test_path, args.plane, stats["ch_names"])
    title_extra = f"{ref_path.name}  vs  {test_path.name}"

    render_heatmap(
        stats["abs_diff"], args.plane, p_r, stats["ch_names"], bps_r,
        args.colormap, args.clamp_max, output_path, title_extra, stats["channels"],
    )
    print(f"[HeatmapDiff] Output: {output_path}")
    print()
    return 0 if not _contract_failed else 2


if __name__ == "__main__":
    sys.exit(main())
