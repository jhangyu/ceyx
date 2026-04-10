#!/usr/bin/env python3
import argparse
import datetime as dt
import re
import statistics
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


RENDER_TIMING_ORDER = [
    "resample",
    "extractStage3U16",
    "extractStage3",
    "buildParams",
    "halideNoMap",
    "halideMapsNoEncode",
    "halideFull",
]
COPY_SIDE_TIMING_KEYS = {"resample", "extractStage3U16", "extractStage3", "buildParams"}
KERNEL_SIDE_TIMING_KEYS = {"halideNoMap", "halideMapsNoEncode", "halideFull"}


@dataclass
class StageResult:
    time_ms: Optional[float] = None
    psnr_db: Optional[float] = None

    def as_cell(self) -> str:
        t = f"{self.time_ms:.2f} ms" if self.time_ms is not None else "N/A"
        p = f"{self.psnr_db:.2f} dB" if self.psnr_db is not None else "N/A"
        return f"{t} / {p}"


def mean_optional(values: list[Optional[float]]) -> Optional[float]:
    present = [v for v in values if v is not None]
    if not present:
        return None
    return statistics.fmean(present)


@dataclass
class CaseResult:
    case_name: str
    stage1: StageResult
    stage2: StageResult
    stage3: StageResult
    stage4: StageResult
    total_ms: Optional[float]
    render_timing_ms: dict[str, float]
    raw_output: str

    def total_cell(self) -> str:
        return f"{self.total_ms:.2f} ms" if self.total_ms is not None else "N/A"


@dataclass
class RepeatedCaseResult:
    case_name: str
    runs: list[CaseResult]

    def _stage(self, attr: str) -> StageResult:
        stages = [getattr(r, attr) for r in self.runs]
        return StageResult(
            time_ms=mean_optional([s.time_ms for s in stages]),
            psnr_db=mean_optional([s.psnr_db for s in stages]),
        )

    @property
    def stage1(self) -> StageResult:
        return self._stage("stage1")

    @property
    def stage2(self) -> StageResult:
        return self._stage("stage2")

    @property
    def stage3(self) -> StageResult:
        return self._stage("stage3")

    @property
    def stage4(self) -> StageResult:
        return self._stage("stage4")

    def total_cell(self) -> str:
        total_ms = mean_optional([r.total_ms for r in self.runs])
        return f"{total_ms:.2f} ms" if total_ms is not None else "N/A"

    def stage4_runs_cell(self) -> str:
        values = [r.stage4.time_ms for r in self.runs if r.stage4.time_ms is not None]
        if not values:
            return "N/A"
        return ", ".join(f"{v:.2f}" for v in values)

    def has_render_timing(self) -> bool:
        return any(r.render_timing_ms for r in self.runs)

    def render_timing_avg(self, key: str) -> Optional[float]:
        return mean_optional([r.render_timing_ms.get(key) for r in self.runs])

    def render_timing_avg_cell(self, key: str) -> str:
        value = self.render_timing_avg(key)
        return f"{value:.2f}" if value is not None else "N/A"

    def render_timing_runs_cell(self, key: str) -> str:
        values = [r.render_timing_ms.get(key) for r in self.runs if key in r.render_timing_ms]
        if not values:
            return "N/A"
        return ", ".join(f"{v:.2f}" for v in values)

    def render_bottleneck_hint(self) -> str:
        if not self.has_render_timing():
            return "N/A"

        copy_ms = sum(
            v for k in COPY_SIDE_TIMING_KEYS if (v := self.render_timing_avg(k)) is not None
        )
        kernel_ms = sum(
            v for k in KERNEL_SIDE_TIMING_KEYS if (v := self.render_timing_avg(k)) is not None
        )
        if copy_ms <= 0.0 and kernel_ms <= 0.0:
            return "N/A"

        stage4_ms = self.stage4.time_ms
        copy_pct = f"{(copy_ms / stage4_ms) * 100.0:.1f}%" if stage4_ms else "N/A"
        kernel_pct = f"{(kernel_ms / stage4_ms) * 100.0:.1f}%" if stage4_ms else "N/A"

        if copy_ms > kernel_ms * 1.15:
            dominant = "wrapper/copy-side"
        elif kernel_ms > copy_ms * 1.15:
            dominant = "kernel-side"
        else:
            dominant = "mixed"
        return (
            f"{dominant}; copy {copy_ms:.2f} ms ({copy_pct} of Stage4), "
            f"kernel {kernel_ms:.2f} ms ({kernel_pct} of Stage4)"
        )


STAGE_HEADER_RE = re.compile(r"^--- Stage (\d+):")
TIME_RE = re.compile(r"^\s*Time:\s*([0-9]+(?:\.[0-9]+)?)\s*ms")
PSNR_RE = re.compile(r"^\s*PSNR vs baseline:\s*([0-9]+(?:\.[0-9]+)?)\s*dB")
DECODE_TOTAL_RE = re.compile(r"^\s*DECODE TOTAL:\s*([0-9]+(?:\.[0-9]+)?)\s*ms")
TOTAL_RE = re.compile(r"^\s*TOTAL:\s*([0-9]+(?:\.[0-9]+)?)\s*ms")
RENDER_HALIDE_TIMING_RE = re.compile(r"^\[RenderHalideTiming\]\s+(.*)$")
RENDER_TIMING_PAIR_RE = re.compile(r"([A-Za-z0-9_]+)=([0-9]+(?:\.[0-9]+)?)\s*ms")


def parse_render_timing(line: str) -> dict[str, float]:
    match = RENDER_HALIDE_TIMING_RE.match(line)
    if not match:
        return {}
    return {key: float(value) for key, value in RENDER_TIMING_PAIR_RE.findall(match.group(1))}


def render_timing_keys(results: list[RepeatedCaseResult]) -> list[str]:
    found = {
        key
        for result in results
        for run in result.runs
        for key in run.render_timing_ms
    }
    ordered = [key for key in RENDER_TIMING_ORDER if key in found]
    ordered.extend(sorted(found.difference(ordered)))
    return ordered


def run_case(cwd: Path, cmd: list[str], case_name: str) -> CaseResult:
    proc = subprocess.run(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    output = proc.stdout
    if proc.returncode != 0:
        raise RuntimeError(f"[{case_name}] failed (exit={proc.returncode})\n{output}")

    stage_map = {
        "1": StageResult(),
        "2": StageResult(),
        "3": StageResult(),
        "4": StageResult(),
    }
    current_stage = None
    total_ms = None
    render_timing_ms: dict[str, float] = {}

    for line in output.splitlines():
        parsed_render_timing = parse_render_timing(line)
        if parsed_render_timing:
            render_timing_ms.update(parsed_render_timing)
            continue
        m = STAGE_HEADER_RE.match(line)
        if m:
            current_stage = m.group(1)
            continue
        m = TIME_RE.match(line)
        if m and current_stage in stage_map:
            stage_map[current_stage].time_ms = float(m.group(1))
            continue
        m = PSNR_RE.match(line)
        if m and current_stage in stage_map:
            stage_map[current_stage].psnr_db = float(m.group(1))
            continue
        m = DECODE_TOTAL_RE.match(line)
        if m:
            total_ms = float(m.group(1))
            continue
        m = TOTAL_RE.match(line)
        if m:
            total_ms = float(m.group(1))

    return CaseResult(
        case_name=case_name,
        stage1=stage_map["1"],
        stage2=stage_map["2"],
        stage3=stage_map["3"],
        stage4=stage_map["4"],
        total_ms=total_ms,
        render_timing_ms=render_timing_ms,
        raw_output=output,
    )


def build_markdown(results: list[RepeatedCaseResult], generated_at: str, repeat: int) -> str:
    lines = []
    lines.append("# Decode 4-Case Performance/PSNR Matrix")
    lines.append("")
    lines.append(f"_Generated at: {generated_at}_")
    lines.append(f"_Repeat count: {repeat}; table values are arithmetic means._")
    lines.append("")
    lines.append(
        "| Case | Stage1 performance/PSNR | Stage2 performance/PSNR | "
        "Stage3 performance/PSNR | Stage4(Render) performance/PSNR | Total |"
    )
    lines.append("|---|---|---|---|---|---|")
    for r in results:
        lines.append(
            f"| {r.case_name} | {r.stage1.as_cell()} | {r.stage2.as_cell()} | "
            f"{r.stage3.as_cell()} | {r.stage4.as_cell()} | {r.total_cell()} |"
        )
    lines.append("")
    lines.append("## Stage4 Raw Runs")
    lines.append("")
    lines.append("| Case | Stage4 runs (ms) |")
    lines.append("|---|---|")
    for r in results:
        lines.append(f"| {r.case_name} | {r.stage4_runs_cell()} |")
    lines.append("")

    timing_keys = render_timing_keys(results)
    if timing_keys:
        lines.append("## Stage4 Halide Timing Breakdown")
        lines.append("")
        lines.append(
            "| Case | " + " | ".join(f"{key} avg (ms)" for key in timing_keys) + " | Bottleneck hint |"
        )
        lines.append("|---" + "|---" * (len(timing_keys) + 1) + "|")
        for r in results:
            cells = [r.render_timing_avg_cell(key) for key in timing_keys]
            lines.append(f"| {r.case_name} | " + " | ".join(cells) + f" | {r.render_bottleneck_hint()} |")
        lines.append("")

        lines.append("## Stage4 Halide Timing Raw Runs")
        lines.append("")
        lines.append("| Case | " + " | ".join(timing_keys) + " |")
        lines.append("|---" + "|---" * len(timing_keys) + "|")
        for r in results:
            cells = [r.render_timing_runs_cell(key) for key in timing_keys]
            lines.append(f"| {r.case_name} | " + " | ".join(cells) + " |")
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run 4 fixed test_decode cases and output Markdown table")
    parser.add_argument(
        "--repo-root",
        default=".",
        help="Repository root path (default: current directory)",
    )
    parser.add_argument(
        "--output",
        default="",
        help="Optional markdown output file path",
    )
    parser.add_argument(
        "--test-decode",
        default="dng_processor/native/build/test_decode",
        help="Path to test_decode executable (relative to repo root by default)",
    )
    parser.add_argument(
        "--lossless",
        default="image_samples/lossless_dng_sample.dng",
        help="Path to lossless DNG sample",
    )
    parser.add_argument(
        "--lossy",
        default="image_samples/lossy_dng_sample.dng",
        help="Path to lossy DNG sample",
    )
    parser.add_argument(
        "--render-mode-custom",
        default="sdk",
        choices=["sdk", "halide-metal", "auto"],
        help="Render mode for custom test cases (default: sdk)",
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=1,
        help="Number of times to run each case and average the parsed values (default: 1)",
    )
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be >= 1")

    repo_root = Path(args.repo_root).resolve()
    test_decode = str((repo_root / args.test_decode).resolve()) if not Path(args.test_decode).is_absolute() else args.test_decode
    lossless = str((repo_root / args.lossless).resolve()) if not Path(args.lossless).is_absolute() else args.lossless
    lossy = str((repo_root / args.lossy).resolve()) if not Path(args.lossy).is_absolute() else args.lossy

    cases = [
        ("Lossless Baseline (SDK)", [test_decode, lossless, "baseline"]),
        ("Lossless Custom (Halide + halide-metal warp)", [test_decode, lossless, "test", "halide-metal", args.render_mode_custom]),
        ("Lossy Baseline (SDK)", [test_decode, lossy, "baseline"]),
        ("Lossy Custom (test)", [test_decode, lossy, "test", "auto", args.render_mode_custom]),
    ]

    results = []
    for case_name, cmd in cases:
        runs = []
        for i in range(args.repeat):
            print(f"[RUN] {case_name} ({i + 1}/{args.repeat})")
            runs.append(run_case(repo_root, cmd, case_name))
        results.append(RepeatedCaseResult(case_name=case_name, runs=runs))

    generated_at = dt.datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")
    markdown = build_markdown(results, generated_at, args.repeat)

    if args.output:
        out_path = Path(args.output)
        if not out_path.is_absolute():
            out_path = (repo_root / out_path).resolve()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(markdown, encoding="utf-8")
        print(f"[OK] Markdown written to: {out_path}")
    else:
        print(markdown)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
