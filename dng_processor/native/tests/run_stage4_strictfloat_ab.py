#!/usr/bin/env python3
"""
Stage4 strict-float A/B regression runner.

Builds `test_decode` twice via build_native_watchdog.py:
1) default (DNG_RENDER_STAGE4_STRICT_FLOAT=OFF)
2) strict  (DNG_RENDER_STAGE4_STRICT_FLOAT=ON)

Then runs a fixed decode command and reports:
- full-stage PSNR vs reference
- per-channel nonZero count / maxAbs from [RenderHalideDiff]
"""

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


PSNR_RE = re.compile(r"^\[RenderHalide\] full-stage PSNR vs full-reference:\s*([0-9.]+)\s*dB$")
DIFF_RE = re.compile(
    r"^\[RenderHalideDiff\]\s+([RGB])\s+.*maxAbs=([0-9]+)\s+.*nonZero=([0-9]+)/([0-9]+)$"
)


@dataclass
class ChannelDiff:
    max_abs: int = 0
    non_zero: int = 0
    total: int = 0


@dataclass
class ABResult:
    label: str
    strict_float: bool
    psnr: float | None
    r: ChannelDiff
    g: ChannelDiff
    b: ChannelDiff


def run_cmd(cmd: list[str], cwd: Path, env: dict[str, str] | None = None) -> str:
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
        print(proc.stdout)
        raise RuntimeError(f"command failed ({proc.returncode}): {' '.join(cmd)}")
    return proc.stdout


def build_case(repo_root: Path, native_dir: Path, build_dir: Path, strict_float: bool, idle_timeout: int) -> None:
    mode = "ON" if strict_float else "OFF"
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
        f"--cmake-arg=-DDNG_RENDER_STAGE4_STRICT_FLOAT={mode}",
    ]
    run_cmd(cmd, repo_root)


def decode_case(
    repo_root: Path,
    test_decode: Path,
    dng_file: Path,
    warp_mode: str,
    render_mode: str,
) -> ABResult:
    # NOTE (Phase 11 Round 2 follow-up): the historical env block that
    # configured "Stage4-isolated" SDK bit-exact mode here injected
    # DNG_WARP_BIT_EXACT / DNG_RENDER_BIT_EXACT / DNG_RENDER_HALIDE_DEBUG /
    # DNG_RENDER_LSB_RESEARCH / DNG_RENDER_HALIDE_TIMING. All five envs were
    # retired in commit 49d8111 (env-switch sweep) and the corresponding
    # `[RenderHalide]` / `[RenderHalideDiff]` log emitters were also removed
    # from the native source. As a result this A/B harness no longer captures
    # meaningful per-channel diff data and PSNR_RE / DIFF_RE will match
    # nothing. The script is preserved as a build-time A/B scaffold for the
    # DNG_RENDER_STAGE4_STRICT_FLOAT CMake flag only; redesigning the
    # measurement layer is tracked as a separate follow-up.
    env = os.environ.copy()
    cmd = [
        str(test_decode),
        str(dng_file),
        "test",
        warp_mode,
        render_mode,
    ]
    out = run_cmd(cmd, repo_root, env)

    psnr = None
    ch = {"R": ChannelDiff(), "G": ChannelDiff(), "B": ChannelDiff()}
    for line in out.splitlines():
        m_psnr = PSNR_RE.match(line.strip())
        if m_psnr:
            psnr = float(m_psnr.group(1))
            continue
        m_diff = DIFF_RE.match(line.strip())
        if m_diff:
            c = m_diff.group(1)
            ch[c] = ChannelDiff(
                max_abs=int(m_diff.group(2)),
                non_zero=int(m_diff.group(3)),
                total=int(m_diff.group(4)),
            )
    return ABResult(
        label="",
        strict_float=False,
        psnr=psnr,
        r=ch["R"],
        g=ch["G"],
        b=ch["B"],
    )


def render_table(results: list[ABResult]) -> str:
    lines = [
        "| Case | strict_float | PSNR(dB) | R nonZero | G nonZero | B nonZero | maxAbs(R/G/B) |",
        "|------|--------------|----------|-----------|-----------|-----------|---------------|",
    ]
    for r in results:
        lines.append(
            f"| {r.label} | {'ON' if r.strict_float else 'OFF'} | "
            f"{(f'{r.psnr:.4f}' if r.psnr is not None else 'N/A')} | "
            f"{r.r.non_zero}/{r.r.total} | {r.g.non_zero}/{r.g.total} | {r.b.non_zero}/{r.b.total} | "
            f"{r.r.max_abs}/{r.g.max_abs}/{r.b.max_abs} |"
        )
    if len(results) == 2:
        a, b = results[0], results[1]
        lines.append("")
        lines.append("Delta (strict - default):")
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
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Stage4 strict-float A/B regression.")
    parser.add_argument("--repo-root", default=".", help="Repository root path")
    parser.add_argument(
        "--native-dir",
        default="dng_processor/native",
        help="Native root path (relative to repo-root or absolute)",
    )
    parser.add_argument(
        "--build-dir",
        default="dng_processor/native/build",
        help="Build dir path (relative to repo-root or absolute)",
    )
    parser.add_argument(
        "--test-decode",
        default="dng_processor/native/build/test_decode",
        help="Path to test_decode binary (relative to repo-root or absolute)",
    )
    parser.add_argument(
        "--dng",
        default="image_samples/lossless_dng_sample.dng",
        help="Path to DNG sample (relative to repo-root or absolute)",
    )
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

    results: list[ABResult] = []
    cases = [("default", False), ("strict_float", True)]
    for label, strict in cases:
        print(f"[INFO] Build case: {label} (strict_float={'ON' if strict else 'OFF'})")
        build_case(repo_root, native_dir, build_dir, strict, args.idle_timeout_sec)
        print(f"[INFO] Decode case: {label}")
        r = decode_case(repo_root, test_decode, dng_file, args.warp_mode, args.render_mode)
        r.label = label
        r.strict_float = strict
        results.append(r)

    markdown = render_table(results)
    print(markdown)
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
