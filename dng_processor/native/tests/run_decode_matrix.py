#!/usr/bin/env python3
# ---
# file_summary: >
#   SDK vs Halide Metal pipeline 比較矩陣工具。
#   執行 Lossless/Lossy × SDK/Halide Metal 四組 case，
#   解析 stdout 的 Stage1-4 時間與 PSNR，支援 repeat 平均，輸出 Markdown 表格。
#
# cases:
#   Lossless / SDK:      baseline mode，全 SDK Stage3+Stage4，作為 reference（存 .raw）
#   Lossless / Halide:   DNG_WARP_BIT_EXACT=0，Halide AHD demosaic + Halide Metal warp + Halide Metal render
#   Lossy    / SDK:      baseline mode，全 SDK Stage3+Stage4，作為 reference（存 .raw）
#   Lossy    / Halide:   SDK YCbCr Stage3（passthrough）+ Halide Metal render
#
# psnr_semantics:
#   SDK cases:          無 PSNR（本身即為 baseline）
#   Lossless Halide:
#     Stage1/2 PSNR:    999 dB（同 DNG parse）
#     Stage3 PSNR:      Halide AHD+Metal warp vs SDK demosaic+SDK warp（end-to-end Stage3 品質）
#     Stage4 PSNR:      最終影像 end-to-end 差（含 Stage3 差異，反映整體管線品質）
#   Lossy Halide:
#     Stage1/2/3 PSNR:  999 dB（Stage3 走同一 SDK YCbCr 路徑）
#     Stage4 PSNR:      Halide Metal render vs SDK render（Stage4 單獨品質，已排除 Stage3 差異）
#
# stage4_timing_note: >
#   Stage4 time 包含 dng_render() 建構器時間（兩條路徑共有，約 250-260ms）。
#   啟用 --timing 後，[halideFull] 欄位為 Halide GPU kernel 實際執行時間（不含建構器）。
#
# usage: |
#   python3 run_decode_matrix.py --repo-root . --repeat 2 --timing
#   python3 run_decode_matrix.py --repo-root . --repeat 3 --output docs/logs/YYYY-MM-DD/matrix.md
# ---
import argparse
import datetime as dt
import os
import re
import statistics
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclass
class StageResult:
    time_ms: Optional[float] = None
    psnr_db: Optional[float] = None


def mean_optional(values: list[Optional[float]]) -> Optional[float]:
    present = [v for v in values if v is not None]
    return statistics.fmean(present) if present else None


@dataclass
class RunResult:
    """Single run of one test case."""
    case_name: str
    stage1: StageResult
    stage2: StageResult
    stage3: StageResult
    stage4: StageResult
    total_ms: Optional[float]
    halide_full_ms: Optional[float]   # from [RenderHalideTiming] halideFull=X
    raw_output: str


@dataclass
class AggResult:
    """Averaged result across N repeats."""
    case_name: str
    runs: list[RunResult]

    def _avg(self, attr: str) -> StageResult:
        stages = [getattr(r, attr) for r in self.runs]
        return StageResult(
            time_ms=mean_optional([s.time_ms for s in stages]),
            psnr_db=mean_optional([s.psnr_db for s in stages]),
        )

    @property
    def stage1(self) -> StageResult: return self._avg("stage1")
    @property
    def stage2(self) -> StageResult: return self._avg("stage2")
    @property
    def stage3(self) -> StageResult: return self._avg("stage3")
    @property
    def stage4(self) -> StageResult: return self._avg("stage4")

    @property
    def total_ms(self) -> Optional[float]:
        return mean_optional([r.total_ms for r in self.runs])

    @property
    def halide_full_ms(self) -> Optional[float]:
        return mean_optional([r.halide_full_ms for r in self.runs])

    def stage4_runs_str(self) -> str:
        vals = [r.stage4.time_ms for r in self.runs if r.stage4.time_ms is not None]
        return ", ".join(f"{v:.1f}" for v in vals) if vals else "N/A"

    def halide_full_runs_str(self) -> str:
        vals = [r.halide_full_ms for r in self.runs if r.halide_full_ms is not None]
        return ", ".join(f"{v:.1f}" for v in vals) if vals else "N/A"


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

_STAGE_RE = re.compile(r"^--- Stage (\d+):")
_TIME_RE = re.compile(r"^\s*Time:\s*([0-9]+(?:\.[0-9]+)?)\s*ms")
_PSNR_RE = re.compile(r"^\s*PSNR vs baseline:\s*([0-9]+(?:\.[0-9]+)?)\s*dB")
_TOTAL_RE = re.compile(r"^\s*(?:DECODE )?TOTAL:\s*([0-9]+(?:\.[0-9]+)?)\s*ms")
_HALIDE_TIMING_RE = re.compile(r"^\[RenderHalideTiming\].*\bhalideFull=([0-9]+(?:\.[0-9]+)?)\s*ms")


def _run_case(cwd: Path, cmd: list[str], case_name: str, env: dict[str, str]) -> RunResult:
    merged = os.environ.copy()
    merged.update(env)
    proc = subprocess.run(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=merged,
        check=False,
    )
    output = proc.stdout
    if proc.returncode != 0:
        raise RuntimeError(f"[{case_name}] exit={proc.returncode}\n{output}")

    stages: dict[str, StageResult] = {str(i): StageResult() for i in range(1, 5)}
    cur: Optional[str] = None
    total_ms: Optional[float] = None
    halide_full_ms: Optional[float] = None

    for line in output.splitlines():
        m = _STAGE_RE.match(line)
        if m:
            cur = m.group(1)
            continue
        m = _TIME_RE.match(line)
        if m and cur in stages:
            stages[cur].time_ms = float(m.group(1))
            continue
        m = _PSNR_RE.match(line)
        if m and cur in stages:
            stages[cur].psnr_db = float(m.group(1))
            continue
        m = _TOTAL_RE.match(line)
        if m:
            total_ms = float(m.group(1))
            continue
        m = _HALIDE_TIMING_RE.match(line)
        if m:
            halide_full_ms = float(m.group(1))

    return RunResult(
        case_name=case_name,
        stage1=stages["1"],
        stage2=stages["2"],
        stage3=stages["3"],
        stage4=stages["4"],
        total_ms=total_ms,
        halide_full_ms=halide_full_ms,
        raw_output=output,
    )


# ---------------------------------------------------------------------------
# Markdown output
# ---------------------------------------------------------------------------

def _fmt_ms(v: Optional[float]) -> str:
    return f"{v:.1f}" if v is not None else "N/A"


def _fmt_db(v: Optional[float]) -> str:
    if v is None:
        return "—"
    if v >= 998.0:
        return "999.00 ✓"
    return f"{v:.2f}"


def _build_markdown(
    results: list[AggResult],
    generated_at: str,
    repeat: int,
    show_halide_timing: bool,
) -> str:
    L: list[str] = []

    L.append("# DNG Pipeline Matrix — SDK vs Halide Metal")
    L.append("")
    L.append(f"_Generated: {generated_at} | Repeat: {repeat} (arithmetic mean)_")
    L.append("")
    L.append("> **PSNR semantics**")
    L.append("> - SDK cases: no PSNR (they are the baseline reference)")
    L.append("> - Lossless Halide: Stage3 PSNR = Halide AHD+Metal vs SDK demosaic+SDK warp; "
             "Stage4 PSNR = end-to-end final image diff (includes Stage3 differences)")
    L.append("> - Lossy Halide: Stage3 PSNR = 999 dB (same SDK YCbCr path); "
             "Stage4 PSNR = Stage4-isolated rendering quality")
    L.append(">")
    L.append("> **Stage4 timing note**: total Stage4 now closely tracks `halideFull` GPU kernel time "
             "(prior 250ms+ first-touch page-fault cost on `out_rgb` was eliminated by caller "
             "pre-allocating + replacing `assign(N,0)` with `resize(N)` in `runHalideFullOrSdkFallback`). "
             "SDK Stage4 = `dng_render::Render()` total. `dng_render()` constructor itself is ~0.03 ms.")
    L.append("")

    # --- Timing table ---
    L.append("## Timing (ms, mean of N runs)")
    L.append("")
    L.append("| Case | Stage1 | Stage2 | Stage3 | Stage4 (total) | DECODE TOTAL |")
    L.append("|---|---|---|---|---|---|")
    for r in results:
        L.append(
            f"| {r.case_name} "
            f"| {_fmt_ms(r.stage1.time_ms)} "
            f"| {_fmt_ms(r.stage2.time_ms)} "
            f"| {_fmt_ms(r.stage3.time_ms)} "
            f"| {_fmt_ms(r.stage4.time_ms)} "
            f"| {_fmt_ms(r.total_ms)} |"
        )
    L.append("")

    # --- PSNR table ---
    L.append("## PSNR vs SDK Baseline (dB)")
    L.append("")
    L.append("| Case | Stage1 | Stage2 | Stage3 | Stage4 |")
    L.append("|---|---|---|---|---|")
    for r in results:
        L.append(
            f"| {r.case_name} "
            f"| {_fmt_db(r.stage1.psnr_db)} "
            f"| {_fmt_db(r.stage2.psnr_db)} "
            f"| {_fmt_db(r.stage3.psnr_db)} "
            f"| {_fmt_db(r.stage4.psnr_db)} |"
        )
    L.append("")

    # --- Halide timing breakdown ---
    if show_halide_timing:
        has_any = any(r.halide_full_ms is not None for r in results)
        if has_any:
            L.append("## Stage4 Halide GPU Kernel Time (ms)")
            L.append("")
            L.append("_`halideFull` = actual GPU kernel time, excluding `dng_render()` constructor._")
            L.append("")
            L.append("| Case | halideFull avg (ms) | runs (ms) | Stage4 total (ms) |")
            L.append("|---|---|---|---|")
            for r in results:
                L.append(
                    f"| {r.case_name} "
                    f"| {_fmt_ms(r.halide_full_ms)} "
                    f"| {r.halide_full_runs_str()} "
                    f"| {_fmt_ms(r.stage4.time_ms)} |"
                )
            L.append("")

    # --- Raw run data ---
    L.append("## Stage4 Raw Runs (ms)")
    L.append("")
    L.append("| Case | Stage4 total runs (ms) |")
    L.append("|---|---|")
    for r in results:
        L.append(f"| {r.case_name} | {r.stage4_runs_str()} |")
    L.append("")

    return "\n".join(L)


# ---------------------------------------------------------------------------
# Case definitions
# ---------------------------------------------------------------------------

def _build_cases(
    test_decode: str,
    lossless: str,
    lossy: str,
    extra_env: dict[str, str],
    enable_timing: bool,
) -> list[tuple[str, list[str], dict[str, str]]]:
    """Returns [(case_name, cmd, env), ...]."""

    timing_env = {"DNG_RENDER_HALIDE_TIMING": "1"} if enable_timing else {}

    sdk_env = dict(extra_env)
    halide_env_lossless = {**extra_env, **timing_env, "DNG_WARP_BIT_EXACT": "0"}
    halide_env_lossy = {**extra_env, **timing_env}

    return [
        (
            "Lossless / SDK",
            [test_decode, lossless, "baseline"],
            sdk_env,
        ),
        (
            "Lossless / Halide Metal",
            [test_decode, lossless, "test", "halide-metal", "halide-metal"],
            halide_env_lossless,
        ),
        (
            "Lossy / SDK",
            [test_decode, lossy, "baseline"],
            sdk_env,
        ),
        (
            "Lossy / Halide Metal",
            [test_decode, lossy, "test", "auto", "halide-metal"],
            halide_env_lossy,
        ),
    ]


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Run SDK vs Halide Metal pipeline matrix and output Markdown table."
    )
    ap.add_argument("--repo-root", default=".", help="Repository root (default: .)")
    ap.add_argument(
        "--test-decode",
        default="dng_processor/native/build/test_decode",
        help="Path to test_decode binary (relative to repo-root)",
    )
    ap.add_argument(
        "--lossless",
        default="image_samples/lossless_dng_sample.dng",
        help="Lossless DNG sample path (relative to repo-root)",
    )
    ap.add_argument(
        "--lossy",
        default="image_samples/lossy_dng_sample.dng",
        help="Lossy DNG sample path (relative to repo-root)",
    )
    ap.add_argument("--repeat", type=int, default=1, help="Runs per case (default: 1)")
    ap.add_argument(
        "--timing",
        action="store_true",
        default=False,
        help="Enable DNG_RENDER_HALIDE_TIMING=1 and show halideFull breakdown table",
    )
    ap.add_argument("--output", default="", help="Optional Markdown output file path")
    ap.add_argument(
        "--env",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="Extra env var override, repeatable (applies to all cases)",
    )
    args = ap.parse_args()

    if args.repeat < 1:
        ap.error("--repeat must be >= 1")

    root = Path(args.repo_root).resolve()
    bin_path = (root / args.test_decode).resolve()
    if not bin_path.exists():
        ap.error(f"test_decode binary not found: {bin_path}")

    lossless = str((root / args.lossless).resolve())
    lossy = str((root / args.lossy).resolve())
    for p in (lossless, lossy):
        if not Path(p).exists():
            ap.error(f"DNG sample not found: {p}")

    extra_env: dict[str, str] = {}
    for item in args.env:
        if "=" not in item:
            ap.error(f"--env must be KEY=VALUE, got: {item!r}")
        k, v = item.split("=", 1)
        k = k.strip()
        if not k:
            ap.error(f"--env key empty: {item!r}")
        extra_env[k] = v

    cases = _build_cases(
        str(bin_path), lossless, lossy, extra_env, enable_timing=args.timing
    )

    agg_results: list[AggResult] = []
    for case_name, cmd, env in cases:
        runs: list[RunResult] = []
        for i in range(args.repeat):
            label = f"[RUN {i+1}/{args.repeat}] {case_name}"
            print(label)
            try:
                run = _run_case(root, cmd, case_name, env)
                runs.append(run)
            except RuntimeError as exc:
                print(f"  ERROR: {exc}")
                raise SystemExit(1)
        agg_results.append(AggResult(case_name=case_name, runs=runs))

    generated_at = dt.datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")
    md = _build_markdown(
        agg_results,
        generated_at=generated_at,
        repeat=args.repeat,
        show_halide_timing=args.timing,
    )

    if args.output:
        out = Path(args.output)
        if not out.is_absolute():
            out = (root / out).resolve()
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(md, encoding="utf-8")
        print(f"[OK] written: {out}")
    else:
        print(md)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
