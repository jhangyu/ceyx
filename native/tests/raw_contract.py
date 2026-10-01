"""Shared [Contract] helpers for compare_psnr.py and heatmap_diff.py.

Printed text is part of both tools' contract (run_decode_matrix.py dumps it
verbatim into its report), so every string here is byte-identical to the
per-script copies it replaced. A contract FAIL aborts with exit code 2.
"""
import re
import sys
from pathlib import Path
from typing import Optional, Tuple

PIXEL_SIZE_TO_DTYPE = {1: "uint8", 2: "uint16"}
PSNR_MAX_VAL = {1: 255.0, 2: 65535.0}

_contract_failed = False


def contract(label: str, passed: bool, detail: str = "") -> bool:
    """Print [Contract] <label>: PASS/FAIL and latch the failure flag."""
    global _contract_failed
    status = "PASS" if passed else "FAIL"
    message = f"[Contract] {label}: {status}"
    if detail:
        message += f"  ({detail})"
    print(message)
    if not passed:
        _contract_failed = True
    return passed


def contract_failed() -> bool:
    return _contract_failed


def abort_if_contract_failed(abort_message: str) -> None:
    if _contract_failed:
        print(f"\n{abort_message}")
        sys.exit(2)


_FILENAME_DIM_RE = re.compile(r"(\d+)x(\d+)_(\d+)p")


def parse_dims_from_path(path: Path) -> Optional[Tuple[int, int, int]]:
    """Parse (width, height, planes) from a WxH_Pp token, e.g. 6048x4024_3p."""
    match = _FILENAME_DIM_RE.search(path.name)
    if match:
        return int(match.group(1)), int(match.group(2)), int(match.group(3))
    return None


def infer_pixel_size(path: Path, width: int, height: int, planes: int) -> Optional[int]:
    """Infer bytes per sample (1 or 2) from the file size."""
    actual = path.stat().st_size
    for bytes_per_sample in (1, 2):
        if actual == width * height * planes * bytes_per_sample:
            return bytes_per_sample
    return None


def run_pair_contracts(
    path_a: Path,
    path_b: Path,
    a: Tuple[int, int, int, int, str],
    b: Tuple[int, int, int, int, str],
    label_a: str,
    label_b: str,
    separator: str,
    abort_message: str,
) -> None:
    """Seven contracts (width/height/planes/pixel_size/layout/buffer_size x2), then abort on FAIL.

    a / b = (width, height, planes, pixel_size, layout). The expected buffer
    size is taken from `a`, as both tools always did.
    """
    width_a, height_a, planes_a, pixel_size_a, layout_a = a
    width_b, height_b, planes_b, pixel_size_b, layout_b = b
    print("\n--- Stage Contract Checks ---")
    contract("width", width_a == width_b, f"{label_a}={width_a} vs {label_b}={width_b}")
    contract("height", height_a == height_b, f"{label_a}={height_a} vs {label_b}={height_b}")
    contract("planes", planes_a == planes_b, f"{label_a}={planes_a} vs {label_b}={planes_b}")
    contract("pixel_size",
             pixel_size_a == pixel_size_b,
             f"{label_a}={pixel_size_a} ({PIXEL_SIZE_TO_DTYPE.get(pixel_size_a, '?')}) "
             f"vs {label_b}={pixel_size_b} ({PIXEL_SIZE_TO_DTYPE.get(pixel_size_b, '?')})")
    contract("layout", layout_a == layout_b, f"{label_a}={layout_a} vs {label_b}={layout_b}")
    expected_bytes = width_a * height_a * planes_a * pixel_size_a
    actual_a = path_a.stat().st_size
    actual_b = path_b.stat().st_size
    dimensions = separator.join(str(v) for v in (width_a, height_a, planes_a, pixel_size_a))
    contract(f"buffer_size {label_a}", actual_a == expected_bytes,
             f"actual={actual_a} expected={expected_bytes} ({dimensions})")
    contract(f"buffer_size {label_b}", actual_b == expected_bytes,
             f"actual={actual_b} expected={expected_bytes} ({dimensions})")
    abort_if_contract_failed(abort_message)
