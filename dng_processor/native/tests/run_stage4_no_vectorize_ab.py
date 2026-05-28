#!/usr/bin/env python3
"""
Stage4 NO_VECTORIZE A/B regression — Step 6.4.4 Round 2 bisection.

Two-phase build (each case does full configure+build to avoid CMake
cache pollution between flag sets):

  A) STRICT_FLOAT=ON  NO_VECTORIZE=OFF  (default: vectorize+unroll on CPU)
  B) STRICT_FLOAT=ON  NO_VECTORIZE=ON   (scalar-only: no vectorize/unroll)

Runs decode with DNG_RENDER_BIT_EXACT=0 (Halide kernel vs SDK reference)
and reports PSNR + per-channel nonZero/maxAbs.

Hypothesis: if B shows fewer nonZero pixels than A, the residual
±1 LSB is caused by SIMD codegen reordering (not FMA algebra).
"""

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

PSNR_RE = re.compile(
    r"^\[RenderHalide\] full-stage PSNR vs full-reference:\s*([0-9.]+)\s*dB$"
)
DIFF_RE = re.compile(
    r"^\[RenderHalideDiff\]\s+([RGB])\s+.*maxAbs=([0-9]+)\s+.*nonZero=([0-9]+)/([0-9]+)$"
)


@dataclass
class ChannelDiff:
    max_abs: int = 0
    non_zero: int = 0
    total: int = 0


@dataclass
class CaseResult:
    label: str
    no_vectorize: bool
    psnr: float | None
    r: ChannelDiff
    g: ChannelDiff
    b: ChannelDiff


def run_cmd(cmd: list[str], cwd: Path, env: dict | None = None) -> str:
    proc = subprocess.run(
        cmd,
        cwd=str(cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        print(proc.stdout, file=sys.stderr)
        raise RuntimeError(f"command failed ({proc.returncode}): {' '.join(cmd)}")
    return proc.stdout


def build_case(
    repo_root: Path,
    native_dir: Path,
    build_dir: Path,
    strict_float: bool,
    no_vectorize: bool,
    idle_timeout: int,
) -> None:
    """Full configure + build via watchdog (always reconfigure to pick up flag changes)."""
    sf = "ON" if strict_float else "OFF"
    nv = "ON" if no_vectorize else "OFF"
    cmd = [
        "python3",
        str(repo_root / "dng_processor/native/scripts/build_native_watchdog.py"),
        "--native-dir",
        str(native_dir),
        "--build-dir",
        str(build_dir),
        "--target",
        "test_decode",
        "--idle-timeout-sec",
        str(idle_timeout),
        "--cmake-arg=-DDNG_RENDER_STAGE4_STRICT_FLOAT=" + sf,
        "--cmake-arg=-DDNG_RENDER_STAGE4_NO_VECTORIZE=" + nv,
    ]
    print(f"[BUILD] strict_float={sf} no_vectorize={nv}")
    run_cmd(cmd, repo_root)


def decode_case(
    test_decode: Path,
    dng_file: Path,
    warp_mode: str,
    render_mode: str,
) -> tuple[float | None, dict]:
    # NOTE (Phase 11 Round 2 follow-up): the historical env block here
    # injected DNG_WARP_BIT_EXACT / DNG_RENDER_BIT_EXACT /
    # DNG_RENDER_HALIDE_DEBUG / DNG_RENDER_LSB_RESEARCH /
    # DNG_RENDER_HALIDE_TIMING. All five were retired in commit 49d8111
    # (env-switch sweep) and the `[RenderHalide]` / `[RenderHalideDiff]`
    # log emitters were also removed from the native source. PSNR_RE and
    # DIFF_RE will therefore match nothing. The script is preserved as a
    # build-time A/B scaffold for the DNG_RENDER_STAGE4_NO_VECTORIZE
    # CMake flag only; redesigning the measurement layer is a deferred
    # follow-up.
    env = os.environ.copy()
    cmd = [
        str(test_decode),
        str(dng_file),
        "test",
        warp_mode,
        render_mode,
    ]
    proc = subprocess.run(
        cmd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    out = proc.stdout

    psnr = None
    ch = {"R": ChannelDiff(), "G": ChannelDiff(), "B": ChannelDiff()}
    for line in out.splitlines():
        m = PSNR_RE.match(line.strip())
        if m:
            psnr = float(m.group(1))
            continue
        m = DIFF_RE.match(line.strip())
        if m:
            c = m.group(1)
            ch[c] = ChannelDiff(
                max_abs=int(m.group(2)),
                non_zero=int(m.group(3)),
                total=int(m.group(4)),
            )
    return psnr, ch


def render_table(results: list[CaseResult]) -> str:
    lines = [
        "| Case | no_vectorize | PSNR(dB) | R nonZero | G nonZero | B nonZero | maxAbs(R/G/B) |",
        "|------|--------------|----------|-----------|-----------|-----------|---------------|",
    ]
    for r in results:
        lines.append(
            f"| {r.label} | {'ON' if r.no_vectorize else 'OFF'} | "
            f"{(f'{r.psnr:.4f}' if r.psnr is not None else 'N/A')} | "
            f"{r.r.non_zero} | {r.g.non_zero} | {r.b.non_zero} | "
            f"{r.r.max_abs}/{r.g.max_abs}/{r.b.max_abs} |"
        )
    if len(results) == 2:
        a, b = results[0], results[1]
        lines.append("")
        lines.append("Delta (no_vectorize ON - OFF):")
        lines.append(
            f"- PSNR: {(b.psnr - a.psnr):+.4f} dB"
            if a.psnr is not None and b.psnr is not None
            else "- PSNR: N/A"
        )
        lines.append(
            f"- nonZero: R {b.r.non_zero - a.r.non_zero:+d}, "
            f"G {b.g.non_zero - a.g.non_zero:+d}, "
            f"B {b.b.non_zero - a.b.non_zero:+d}"
        )
        lines.append("")
        nz_delta = (b.r.non_zero + b.g.non_zero + b.b.non_zero) - (
            a.r.non_zero + a.g.non_zero + a.b.non_zero
        )
        if nz_delta < 0:
            lines.append("**Interpretation: fewer nonZero pixels → SIMD codegen IS contributing**")
        elif nz_delta > 0:
            lines.append("**Interpretation: more nonZero → disabling vectorize alone does NOT fix**")
        else:
            lines.append("**Interpretation: no change → residual NOT from SIMD codegen reorder**")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Stage4 NO_VECTORIZE A/B regression.")
    parser.add_argument("--repo-root", default=".", help="Repository root")
    parser.add_argument("--native-dir", default="dng_processor/native")
    parser.add_argument("--build-dir", default="dng_processor/native/build")
    parser.add_argument("--test-decode", default="dng_processor/native/build/test_decode")
    parser.add_argument("--dng", default="image_samples/lossless_dng_sample.dng")
    parser.add_argument("--warp-mode", default="halide-metal")
    parser.add_argument("--render-mode", default="halide-metal")
    parser.add_argument("--idle-timeout-sec", type=int, default=60)
    parser.add_argument("--output", help="Optional markdown output path")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    native_dir = Path(args.native_dir)
    if not native_dir.is_absolute():
        native_dir = (repo_root / native_dir).resolve()
    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = (repo_root / build_dir).resolve()
    test_decode = Path(args.test_decode)
    if not test_decode.is_absolute():
        test_decode = (repo_root / test_decode).resolve()
    dng_file = Path(args.dng)
    if not dng_file.is_absolute():
        dng_file = (repo_root / dng_file).resolve()

    if not dng_file.exists():
        print(f"[ERROR] DNG file not found: {dng_file}", file=sys.stderr)
        return 2

    results: list[CaseResult] = []
    cases = [("vectorize", False), ("no_vectorize", True)]

    for label, nv in cases:
        print(f"\n=== Case {label} (no_vectorize={'ON' if nv else 'OFF'}) ===")
        build_case(repo_root, native_dir, build_dir,
                   strict_float=True, no_vectorize=nv,
                   idle_timeout=args.idle_timeout_sec)
        psnr, ch = decode_case(
            test_decode, dng_file, args.warp_mode, args.render_mode
        )
        r = CaseResult(label=label, no_vectorize=nv, psnr=psnr,
                       r=ch["R"], g=ch["G"], b=ch["B"])
        results.append(r)
        print(
            f"  -> PSNR={psnr:.4f}dB | nonZero R={r.r.non_zero} G={r.g.non_zero} B={r.b.non_zero}"
            if psnr is not None
            else "  -> PSNR=N/A"
        )

    markdown = render_table(results)
    print("\n" + markdown)
    if args.output:
        out_path = Path(args.output)
        if not out_path.is_absolute():
            out_path = (repo_root / out_path).resolve()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(markdown, encoding="utf-8")
        print(f"[INFO] Wrote: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
