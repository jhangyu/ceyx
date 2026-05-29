#!/usr/bin/env python3
# ---
# file_summary: >
#   SDK vs Halide Metal pipeline 比較矩陣工具。
#   執行 Lossless/Lossy × SDK/Halide Metal 四組 case，
#   解析 stdout 的 Stage1-4 時間與 PSNR，支援 repeat 平均，輸出 Markdown 表格。
#
# cases:
#   Lossless / SDK:      baseline mode，全 SDK Stage3+Stage4，作為 reference（存 .raw）
#   Lossless / Halide:   Halide bilinear/fused Stage3 + Halide Metal render
#   Lossy    / SDK:      baseline mode，全 SDK Stage3+Stage4，作為 reference（存 .raw）
#   Lossy    / Halide:   SDK YCbCr Stage3（passthrough）+ Halide Metal render
#
# psnr_semantics:
#   SDK cases:          無 PSNR（本身即為 baseline）
#   Lossless Halide:
#     Stage1/2 PSNR:    999 dB（同 DNG parse）
#     Stage3 PSNR:      Halide bilinear/fused Stage3 vs SDK demosaic+SDK warp（end-to-end Stage3 品質；基準 ≈ 102.72 dB）
#     Stage4 PSNR:      最終影像 end-to-end 差（含 Stage3 差異；基準 ≈ 79.85 dB）
#   Lossy Halide:
#     Stage1/2/3 PSNR:  999 dB（Stage3 走同一 SDK YCbCr 路徑）
#     Stage4 PSNR:      Halide Metal render vs SDK render（Stage4 單獨品質，已排除 Stage3 差異；基準 999 dB）
#
# stage4_timing_note: >
#   Stage4 time 包含 dng_render() 建構器時間（兩條路徑共有，約 250-260ms）。
#   啟用 --timing 後，僅注入仍被認可的 DiagnosticConfig timing envs
#   (DNG_STAGE1_TIMING, DNG_MAP_POLY_TIMING, DNG_STAGE2_SDK_TIMING)。
#   歷史上的 [halideFull] / [DemosaicHalideTiming] / [DemosaicWarpHalideTiming]
#   / [RenderHalideTiming] 等診斷輸出與其對應 envs (DNG_RENDER_HALIDE_TIMING、
#   DNG_DEMOSAIC_HALIDE_TIMING、DNG_DEMOSAIC_WARP_HALIDE_TIMING、
#   DNG_WARP_HALIDE_TIMING、DNG_WARP_BIT_EXACT) 已於 49d8111 sweep 全數退役；
#   本腳本 2026-05-28 cleanup 後已移除對應 parsers / fields / 表格欄位。
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
from dataclasses import dataclass, field
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
    raw_output: str
    stage3_probe: dict[str, float] = field(default_factory=dict)
    stage2_probe: dict[str, float] = field(default_factory=dict)
    opcode2_probe: dict[str, float] = field(default_factory=dict)


@dataclass
class FfiRunResult:
    sample_name: str
    ok: bool
    width: Optional[int]
    height: Optional[int]
    rgb_bytes: Optional[int]
    decode_ms: Optional[float]
    process_ms: Optional[float]
    wall_ms: Optional[float]
    error_code: Optional[int]
    stage2_do_build_ms: Optional[float]
    stage2_opcode2_ms: Optional[float]
    stage2_total_ms: Optional[float]
    opcode2_probe: dict[str, float] = field(default_factory=dict)


@dataclass
class FfiAggResult:
    sample_name: str
    runs: list[FfiRunResult]

    @property
    def decode_ms(self) -> Optional[float]:
        return mean_optional([r.decode_ms for r in self.runs])

    @property
    def process_ms(self) -> Optional[float]:
        return mean_optional([r.process_ms for r in self.runs])

    @property
    def wall_ms(self) -> Optional[float]:
        return mean_optional([r.wall_ms for r in self.runs])

    @property
    def stage2_do_build_ms(self) -> Optional[float]:
        return mean_optional([r.stage2_do_build_ms for r in self.runs])

    @property
    def stage2_opcode2_ms(self) -> Optional[float]:
        return mean_optional([r.stage2_opcode2_ms for r in self.runs])

    @property
    def stage2_total_ms(self) -> Optional[float]:
        return mean_optional([r.stage2_total_ms for r in self.runs])

    @property
    def rgb_bytes(self) -> Optional[int]:
        vals = [r.rgb_bytes for r in self.runs if r.rgb_bytes is not None]
        return vals[-1] if vals else None

    def opcode2_probe_avg(self, key: str) -> Optional[float]:
        return mean_optional([r.opcode2_probe.get(key) for r in self.runs])


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

    def stage4_runs_str(self) -> str:
        vals = [r.stage4.time_ms for r in self.runs if r.stage4.time_ms is not None]
        return ", ".join(f"{v:.1f}" for v in vals) if vals else "N/A"

    def stage3_probe_avg(self, key: str) -> Optional[float]:
        return mean_optional([r.stage3_probe.get(key) for r in self.runs])

    def stage3_probe_runs_str(self, key: str) -> str:
        vals = [r.stage3_probe.get(key) for r in self.runs if key in r.stage3_probe]
        return ", ".join(f"{v:.1f}" for v in vals) if vals else "N/A"

    def stage2_probe_avg(self, key: str) -> Optional[float]:
        return mean_optional([r.stage2_probe.get(key) for r in self.runs])

    def opcode2_probe_avg(self, key: str) -> Optional[float]:
        return mean_optional([r.opcode2_probe.get(key) for r in self.runs])

    def stage_validation_extract_ms(self, stage: int) -> Optional[float]:
        if stage != 3:
            return None
        return mean_optional([
            (r.stage3_probe.get("extractStage3", 0.0) +
             r.stage3_probe.get("sdkExtract", 0.0))
            for r in self.runs
            if r.stage3_probe
        ])

    def stage_production_ms(self, stage: int) -> Optional[float]:
        stage_result = getattr(self, f"stage{stage}")
        total = stage_result.time_ms
        if total is None:
            return None
        validation = self.stage_validation_extract_ms(stage)
        if validation is None:
            return total
        return max(0.0, total - validation)

    @property
    def production_total_ms(self) -> Optional[float]:
        vals = [self.stage_production_ms(i) for i in range(1, 5)]
        return sum(vals) if all(v is not None for v in vals) else None

    @property
    def validation_extract_total_ms(self) -> Optional[float]:
        vals = [self.stage_validation_extract_ms(i) for i in range(1, 5)]
        present = [v for v in vals if v is not None]
        return sum(present) if present else None


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

_STAGE_RE = re.compile(r"^--- Stage (\d+):")
_TIME_RE = re.compile(r"^\s*Time:\s*([0-9]+(?:\.[0-9]+)?)\s*ms")
_PSNR_RE = re.compile(r"^\s*PSNR vs baseline:\s*([0-9]+(?:\.[0-9]+)?)\s*dB")
_TOTAL_RE = re.compile(r"^\s*(?:DECODE )?TOTAL:\s*([0-9]+(?:\.[0-9]+)?)\s*ms")
# Stale parsers removed (2026-05-28 cleanup): the source emitters
# `[RenderHalideTiming]`, `[DemosaicHalideTiming]`, `[DemosaicWarpHalideTiming]`
# were dropped together with the DNG_RENDER_HALIDE_TIMING /
# DNG_DEMOSAIC_HALIDE_TIMING / DNG_DEMOSAIC_WARP_HALIDE_TIMING envs in
# commit 49d8111. Only currently-emitted log lines retain matchers below.
_STAGE3_PROBE_RE = re.compile(r"^\[Stage3Probe\]\s*(.*)$")
_STAGE2_PROBE_RE = re.compile(r"^\[Stage2SdkTiming\]\s*(.*)$")
_OPCODE2_TIMING_RE = re.compile(r"^\[OpcodeList2Timing\]\s*(.*)$")
_FFI_RUN_RE = re.compile(
    r"^\[FFI run \d+\]\s+ok=(\d+)\s+w=(\d+)\s+h=(\d+)\s+rgb_bytes=(\d+)\s+"
    r"decode_ms=([0-9]+(?:\.[0-9]+)?)\s+process_ms=([0-9]+(?:\.[0-9]+)?)\s+"
    r"wall_ms=([0-9]+(?:\.[0-9]+)?)\s+err=(-?\d+)"
)
_KV_FLOAT_RE = re.compile(r"\b([A-Za-z0-9_]+)=(-?[0-9]+(?:\.[0-9]+)?)")


def _accumulate_opcode2_timing(dst: dict[str, float], payload: str) -> None:
    values = {k: float(v) for k, v in _KV_FLOAT_RE.findall(payload)}
    for key in ("prewarm", "gather", "kernel", "copy_to_host", "scatter", "t"):
        if key in values:
            dst[key] = dst.get(key, 0.0) + values[key]


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
    stage3_probe: dict[str, float] = {}
    stage2_probe: dict[str, float] = {}
    opcode2_probe: dict[str, float] = {}

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
        m = _STAGE3_PROBE_RE.match(line)
        if m:
            stage3_probe = {k: float(v) for k, v in _KV_FLOAT_RE.findall(m.group(1))}
            continue
        m = _STAGE2_PROBE_RE.match(line)
        if m:
            stage2_probe = {k: float(v) for k, v in _KV_FLOAT_RE.findall(m.group(1))}
            continue
        m = _OPCODE2_TIMING_RE.match(line)
        if m:
            _accumulate_opcode2_timing(opcode2_probe, m.group(1))

    return RunResult(
        case_name=case_name,
        stage1=stages["1"],
        stage2=stages["2"],
        stage3=stages["3"],
        stage4=stages["4"],
        total_ms=total_ms,
        raw_output=output,
        stage3_probe=stage3_probe,
        stage2_probe=stage2_probe,
        opcode2_probe=opcode2_probe,
    )


def _run_ffi_case(cwd: Path, harness: str, sample_name: str, dng_path: str,
                  env: dict[str, str]) -> FfiRunResult:
    merged = os.environ.copy()
    merged.update(env)
    proc = subprocess.run(
        [harness, dng_path, "1"],
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=merged,
        check=False,
    )
    output = proc.stdout
    stage2_probe: dict[str, float] = {}
    opcode2_probe: dict[str, float] = {}
    ffi_match: Optional[re.Match[str]] = None
    for line in output.splitlines():
        m = _FFI_RUN_RE.match(line)
        if m:
            ffi_match = m
        m = _STAGE2_PROBE_RE.match(line)
        if m:
            stage2_probe = {k: float(v) for k, v in _KV_FLOAT_RE.findall(m.group(1))}
        m = _OPCODE2_TIMING_RE.match(line)
        if m:
            _accumulate_opcode2_timing(opcode2_probe, m.group(1))
    if proc.returncode != 0 or not ffi_match:
        raise RuntimeError(f"[FFI {sample_name}] exit={proc.returncode}\n{output}")
    return FfiRunResult(
        sample_name=sample_name,
        ok=ffi_match.group(1) == "1",
        width=int(ffi_match.group(2)),
        height=int(ffi_match.group(3)),
        rgb_bytes=int(ffi_match.group(4)),
        decode_ms=float(ffi_match.group(5)),
        process_ms=float(ffi_match.group(6)),
        wall_ms=float(ffi_match.group(7)),
        error_code=int(ffi_match.group(8)),
        stage2_do_build_ms=stage2_probe.get("doBuildStage2"),
        stage2_opcode2_ms=stage2_probe.get("opcode2"),
        stage2_total_ms=stage2_probe.get("total"),
        opcode2_probe=opcode2_probe,
    )


# ---------------------------------------------------------------------------
# Markdown output
# ---------------------------------------------------------------------------

def _fmt_ms(v: Optional[float]) -> str:
    return f"{v:.1f}" if v is not None else "N/A"


def _fmt_pct(part: Optional[float], total: Optional[float]) -> str:
    if part is None or total is None or total == 0:
        return "N/A"
    return f"{(part / total) * 100.0:.1f}%"


def _fmt_db(v: Optional[float]) -> str:
    if v is None:
        return "—"
    if v >= 998.0:
        return "999.00 ✓"
    return f"{v:.2f}"


def _build_markdown(
    results: list[AggResult],
    ffi_results: list[FfiAggResult],
    generated_at: str,
    repeat: int,
    show_halide_timing: bool,
    artifact_dir: Path,
) -> str:
    L: list[str] = []

    L.append("# DNG Pipeline Matrix — SDK vs Halide Metal")
    L.append("")
    L.append(f"_Generated: {generated_at} | Repeat: {repeat} (arithmetic mean)_")
    L.append(f"_Intermediate artifacts: `{artifact_dir}`_")
    L.append("")
    L.append("> **PSNR semantics**")
    L.append("> - SDK cases: no PSNR (they are the baseline reference)")
    L.append("> - Lossless Halide: Stage3 PSNR = Halide bilinear/fused Stage3 vs SDK demosaic+SDK warp; "
             "Stage4 PSNR = end-to-end final image diff (includes Stage3 differences)")
    L.append("> - Lossy Halide: Stage3 PSNR = 999 dB (same SDK YCbCr path); "
             "Stage4 PSNR = Stage4-isolated rendering quality")
    L.append(">")
    L.append("> **Stage4 timing note**: total Stage4 = `dng_render::Render()` (SDK path) or the "
             "Halide AOT Stage4 kernel + SDK fallback wrapper (Halide path). `dng_render()` "
             "constructor itself is ~0.03 ms. The historical `halideFull` GPU-kernel-only breakdown "
             "(emitted via `[RenderHalideTiming]` under `DNG_RENDER_HALIDE_TIMING=1`) was retired in "
             "commit 49d8111; the corresponding `out_rgb_assign` first-touch cost was also eliminated "
             "by the caller pre-allocating + `resize(N)` change in `runHalideFullOrSdkFallback`.")
    L.append(">")
    L.append("> **Production vs validation timing**: `production` is the stage work that belongs to the "
             "decode/render pipeline. `validation extract` is extra test-harness work used to materialize "
             "raw buffers for contract checks / PSNR. Today only Stage3 exposes this split; Stage1/2/4 "
             "validation extraction is not separately timed by `test_decode` and is shown as `N/A`.")
    if ffi_results:
        L.append(">")
        L.append("> **FFI timing note**: FFI timings are collected from `dng_pipeline_v2_decode_to_rgb()` "
                 "with a separate harness. Output-buffer ownership costs are now amortised by the "
                 "RGB output pool; FFI columns therefore reflect end-to-end decode + process + wall time.")
    L.append("")

    # --- Timing table ---
    L.append("## Timing (ms, mean of N runs)")
    L.append("")
    L.append("| Case | Stage1 production | Stage1 validation extract | Stage2 production | Stage2 validation extract | Stage3 production | Stage3 validation extract | Stage4 production | Stage4 validation extract | Production total | Legacy total |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for r in results:
        L.append(
            f"| {r.case_name} "
            f"| {_fmt_ms(r.stage_production_ms(1))} "
            f"| {_fmt_ms(r.stage_validation_extract_ms(1))} "
            f"| {_fmt_ms(r.stage_production_ms(2))} "
            f"| {_fmt_ms(r.stage_validation_extract_ms(2))} "
            f"| {_fmt_ms(r.stage_production_ms(3))} "
            f"| {_fmt_ms(r.stage_validation_extract_ms(3))} "
            f"| {_fmt_ms(r.stage_production_ms(4))} "
            f"| {_fmt_ms(r.stage_validation_extract_ms(4))} "
            f"| {_fmt_ms(r.production_total_ms)} "
            f"| {_fmt_ms(r.total_ms)} |"
        )
    L.append("")

    if ffi_results:
        L.append("## FFI Production Timing (ms, independent from Stage4)")
        L.append("")
        L.append("| Sample | decode_ms | process_ms | wall_ms | Stage2 total | Stage2 doBuildStage2 | Stage2 opcode2 | rgb_bytes |")
        L.append("|---|---|---|---|---|---|---|---|")
        for r in ffi_results:
            rgb_bytes = str(r.rgb_bytes) if r.rgb_bytes is not None else "N/A"
            L.append(
                f"| {r.sample_name} "
                f"| {_fmt_ms(r.decode_ms)} "
                f"| {_fmt_ms(r.process_ms)} "
                f"| {_fmt_ms(r.wall_ms)} "
                f"| {_fmt_ms(r.stage2_total_ms)} "
                f"| {_fmt_ms(r.stage2_do_build_ms)} "
                f"| {_fmt_ms(r.stage2_opcode2_ms)} "
                f"| {rgb_bytes} |"
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

    # --- Diagnostic timing breakdown (Stage2 SDK / OpcodeList2 / Stage3 probe) ---
    # Historical Stage3 Demosaic / DemosaicWarp AOT and Stage4 halideFull tables
    # were removed alongside the [DemosaicHalideTiming] / [DemosaicWarpHalideTiming]
    # / [RenderHalideTiming] source emitters in commit 49d8111.
    if show_halide_timing:
        has_stage2_probe = any(r.stage2_probe_avg("total") is not None for r in results)
        has_stage3_probe = any(r.stage3_probe_avg("total") is not None for r in results)
        if has_stage2_probe:
            L.append("## Stage2 SDK Orchestration Breakdown (ms)")
            L.append("")
            L.append("_`opcode2` includes OpcodeList2 dispatch; when Halide MapPolynomial is enabled, see `[OpcodeList2Timing]` for its inner kernel/readback split. `doBuildStage2` is SDK linearization / Stage2 image materialization._")
            L.append("")
            L.append("| Case | saveDecision | rawPreOpcode1 | opcode1 | rawPostOpcode1 | linearizationPostParse | doBuildStage2 | releaseStage1 | clearLinearization | opcode2 | postOpcode2 | defloat | rawPostOpcode2 | total |")
            L.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
            for r in results:
                L.append(
                    f"| {r.case_name} "
                    f"| {_fmt_ms(r.stage2_probe_avg('saveDecision'))} "
                    f"| {_fmt_ms(r.stage2_probe_avg('rawPreOpcode1'))} "
                    f"| {_fmt_ms(r.stage2_probe_avg('opcode1'))} "
                    f"| {_fmt_ms(r.stage2_probe_avg('rawPostOpcode1'))} "
                    f"| {_fmt_ms(r.stage2_probe_avg('linearizationPostParse'))} "
                    f"| {_fmt_ms(r.stage2_probe_avg('doBuildStage2'))} "
                    f"| {_fmt_ms(r.stage2_probe_avg('releaseStage1'))} "
                    f"| {_fmt_ms(r.stage2_probe_avg('clearLinearization'))} "
                    f"| {_fmt_ms(r.stage2_probe_avg('opcode2'))} "
                    f"| {_fmt_ms(r.stage2_probe_avg('postOpcode2'))} "
                    f"| {_fmt_ms(r.stage2_probe_avg('defloat'))} "
                    f"| {_fmt_ms(r.stage2_probe_avg('rawPostOpcode2'))} "
                    f"| {_fmt_ms(r.stage2_probe_avg('total'))} |"
                )
            L.append("")

        has_opcode2_probe = any(r.opcode2_probe_avg("t") is not None for r in results)
        if has_opcode2_probe:
            L.append("## Stage2 OpcodeList2 MapPolynomial Breakdown (ms)")
            L.append("")
            L.append("_`non-MapPolynomial` = Stage2 SDK total minus `[OpcodeList2Timing]` MapPolynomial total. This keeps SDK orchestration / linearization separate from the Halide MapPolynomial kernel path._")
            L.append("")
            L.append("| Case | prewarm | gather | kernel | copy_to_host | scatter | MapPolynomial total | MapPolynomial % Stage2 | non-MapPolynomial | non-MapPolynomial % Stage2 |")
            L.append("|---|---|---|---|---|---|---|---|---|---|")
            for r in results:
                map_total = r.opcode2_probe_avg("t")
                stage2_total = r.stage2_probe_avg("total")
                non_map = None
                if map_total is not None and stage2_total is not None:
                    non_map = max(0.0, stage2_total - map_total)
                L.append(
                    f"| {r.case_name} "
                    f"| {_fmt_ms(r.opcode2_probe_avg('prewarm'))} "
                    f"| {_fmt_ms(r.opcode2_probe_avg('gather'))} "
                    f"| {_fmt_ms(r.opcode2_probe_avg('kernel'))} "
                    f"| {_fmt_ms(r.opcode2_probe_avg('copy_to_host'))} "
                    f"| {_fmt_ms(r.opcode2_probe_avg('scatter'))} "
                    f"| {_fmt_ms(map_total)} "
                    f"| {_fmt_pct(map_total, stage2_total)} "
                    f"| {_fmt_ms(non_map)} "
                    f"| {_fmt_pct(non_map, stage2_total)} |"
                )
            L.append("")

        has_ffi_opcode2_probe = any(
            r.opcode2_probe_avg("t") is not None for r in ffi_results
        )
        if has_ffi_opcode2_probe:
            L.append("## FFI Stage2 OpcodeList2 MapPolynomial Breakdown (ms)")
            L.append("")
            L.append("| Sample | prewarm | gather | kernel | copy_to_host | scatter | MapPolynomial total | MapPolynomial % Stage2 | non-MapPolynomial | non-MapPolynomial % Stage2 |")
            L.append("|---|---|---|---|---|---|---|---|---|---|")
            for r in ffi_results:
                map_total = r.opcode2_probe_avg("t")
                stage2_total = r.stage2_total_ms
                non_map = None
                if map_total is not None and stage2_total is not None:
                    non_map = max(0.0, stage2_total - map_total)
                L.append(
                    f"| {r.sample_name} "
                    f"| {_fmt_ms(r.opcode2_probe_avg('prewarm'))} "
                    f"| {_fmt_ms(r.opcode2_probe_avg('gather'))} "
                    f"| {_fmt_ms(r.opcode2_probe_avg('kernel'))} "
                    f"| {_fmt_ms(r.opcode2_probe_avg('copy_to_host'))} "
                    f"| {_fmt_ms(r.opcode2_probe_avg('scatter'))} "
                    f"| {_fmt_ms(map_total)} "
                    f"| {_fmt_pct(map_total, stage2_total)} "
                    f"| {_fmt_ms(non_map)} "
                    f"| {_fmt_pct(non_map, stage2_total)} |"
                )
            L.append("")

        if has_stage3_probe:
            L.append("## Stage3 Probe Breakdown (ms)")
            L.append("")
            L.append("_`prealloc` is intentionally outside Stage3 timing; `extractStage3` / `sdkExtract` are validation-only raw-buffer materialization costs._")
            L.append("")
            L.append("| Case | prealloc | resize | makeImage | demosaic | fused | applyOpcode3 | put | extractStage3 | sdkBuild | sdkExtract | unaccounted | production | validation extract | total |")
            L.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
            for r in results:
                L.append(
                    f"| {r.case_name} "
                    f"| {_fmt_ms(r.stage3_probe_avg('prealloc'))} "
                    f"| {_fmt_ms(r.stage3_probe_avg('resize'))} "
                    f"| {_fmt_ms(r.stage3_probe_avg('makeImage'))} "
                    f"| {_fmt_ms(r.stage3_probe_avg('demosaic'))} "
                    f"| {_fmt_ms(r.stage3_probe_avg('fused'))} "
                    f"| {_fmt_ms(r.stage3_probe_avg('applyOpcode3'))} "
                    f"| {_fmt_ms(r.stage3_probe_avg('put'))} "
                    f"| {_fmt_ms(r.stage3_probe_avg('extractStage3'))} "
                    f"| {_fmt_ms(r.stage3_probe_avg('sdkBuild'))} "
                    f"| {_fmt_ms(r.stage3_probe_avg('sdkExtract'))} "
                    f"| {_fmt_ms(r.stage3_probe_avg('unaccounted'))} "
                    f"| {_fmt_ms(r.stage_production_ms(3))} "
                    f"| {_fmt_ms(r.stage_validation_extract_ms(3))} "
                    f"| {_fmt_ms(r.stage3_probe_avg('total'))} |"
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

    # --timing injects DiagnosticConfig env switches that are still honored.
    # Retired switches (DNG_RENDER_HALIDE_TIMING, DNG_DEMOSAIC_HALIDE_TIMING,
    # DNG_DEMOSAIC_WARP_HALIDE_TIMING, DNG_WARP_HALIDE_TIMING, DNG_DEMOSAIC_AOT)
    # were swept in commit 49d8111 and are no longer read by the native pipeline.
    # See dng_pipeline_config.h for the current env category catalogue.
    timing_env = {
        "DNG_STAGE1_TIMING": "1",          # DiagnosticConfig (ConcurrentDngHost)
        "DNG_MAP_POLY_TIMING": "1",        # DiagnosticConfig (Stage2 OL2 bridge)
        "DNG_STAGE2_SDK_TIMING": "1",      # DiagnosticConfig (vendor DNG SDK)
    } if enable_timing else {}

    # NOTE: DNG_WARP_BIT_EXACT was retired in 49d8111; setting it explicitly
    # is a no-op now. Lossless / Halide path therefore inherits the same env
    # as the SDK case plus the diagnostic timing envs.
    sdk_env = {**extra_env, **timing_env}
    halide_env_lossless = {**extra_env, **timing_env}
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
        help=(
            "Enable DiagnosticConfig timing envs (DNG_STAGE1_TIMING, "
            "DNG_MAP_POLY_TIMING, DNG_STAGE2_SDK_TIMING). The historical "
            "DNG_RENDER_HALIDE_TIMING / halideFull breakdown was retired in "
            "commit 49d8111; the corresponding columns may render as '-'."
        ),
    )
    ap.add_argument(
        "--ffi-harness",
        default="",
        help="Optional FFI timing harness path. When set, adds an independent FFI timing table.",
    )
    ap.add_argument("--output", default="", help="Optional Markdown output file path")
    ap.add_argument(
        "--artifact-dir",
        default="",
        help=(
            "Directory for test_decode intermediate raw outputs. Defaults to "
            "<output-stem>_artifacts when --output is set, otherwise "
            "decode_matrix_artifacts under the repo root."
        ),
    )
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

    if args.artifact_dir:
        artifact_dir = Path(args.artifact_dir)
        if not artifact_dir.is_absolute():
            artifact_dir = (root / artifact_dir).resolve()
    elif args.output:
        output_path = Path(args.output)
        if not output_path.is_absolute():
            output_path = (root / output_path).resolve()
        artifact_dir = output_path.with_suffix("")
        artifact_dir = artifact_dir.parent / f"{artifact_dir.name}_artifacts"
    else:
        artifact_dir = root / "decode_matrix_artifacts"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    print(f"[INFO] Intermediate artifacts: {artifact_dir}")

    agg_results: list[AggResult] = []
    for case_name, cmd, env in cases:
        runs: list[RunResult] = []
        for i in range(args.repeat):
            label = f"[RUN {i+1}/{args.repeat}] {case_name}"
            print(label)
            try:
                run = _run_case(artifact_dir, cmd, case_name, env)
                runs.append(run)
            except RuntimeError as exc:
                print(f"  ERROR: {exc}")
                raise SystemExit(1)
        agg_results.append(AggResult(case_name=case_name, runs=runs))

    ffi_results: list[FfiAggResult] = []
    if args.ffi_harness:
        ffi_harness = (root / args.ffi_harness).resolve()
        if not ffi_harness.exists():
            ap.error(f"FFI harness not found: {ffi_harness}")
        # DNG_RENDER_HALIDE_TIMING was retired in 49d8111; keep only the
        # currently-honored DiagnosticConfig timing envs.
        ffi_env = {
            **extra_env,
            "DNG_STAGE1_TIMING": "1",
            "DNG_MAP_POLY_TIMING": "1",
            "DNG_STAGE2_SDK_TIMING": "1",
        }
        ffi_cases = [
            ("Lossless / FFI", lossless),
            ("Lossy / FFI", lossy),
        ]
        for sample_name, dng_path in ffi_cases:
            runs: list[FfiRunResult] = []
            for i in range(args.repeat):
                print(f"[FFI {i+1}/{args.repeat}] {sample_name}")
                try:
                    runs.append(_run_ffi_case(root, str(ffi_harness), sample_name,
                                              dng_path, ffi_env))
                except RuntimeError as exc:
                    print(f"  ERROR: {exc}")
                    raise SystemExit(1)
            ffi_results.append(FfiAggResult(sample_name=sample_name, runs=runs))

    generated_at = dt.datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")
    md = _build_markdown(
        agg_results,
        ffi_results,
        generated_at=generated_at,
        repeat=args.repeat,
        show_halide_timing=args.timing,
        artifact_dir=artifact_dir,
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

    # W5-3 / TD-10: Post-matrix PSNR gate.
    # For Halide cases (non-SDK baselines) enforce per-stage minimum PSNR thresholds.
    # Lossless: Stage1/2 ≥999, Stage3 ≥100, Stage4 ≥75.
    # Lossy:    All stages ≥999 (SDK YCbCr passthrough Stage3, bit-exact Stage4).
    _PSNR_THRESHOLDS = {
        # (case_name_fragment, stage_attr): threshold_db
        ("Lossless / Halide", "stage1"): 999.0,
        ("Lossless / Halide", "stage2"): 999.0,
        ("Lossless / Halide", "stage3"): 100.0,
        ("Lossless / Halide", "stage4"): 75.0,
        ("Lossy / Halide",    "stage1"): 999.0,
        ("Lossy / Halide",    "stage2"): 999.0,
        ("Lossy / Halide",    "stage3"): 999.0,
        ("Lossy / Halide",    "stage4"): 999.0,
    }
    gate_failed = False
    for r in agg_results:
        for stage_attr in ("stage1", "stage2", "stage3", "stage4"):
            for frag, s_attr in _PSNR_THRESHOLDS:
                if frag in r.case_name and s_attr == stage_attr:
                    threshold = _PSNR_THRESHOLDS[(frag, s_attr)]
                    stage_result = getattr(r, stage_attr)
                    psnr = stage_result.psnr_db
                    if psnr is None:
                        # PSNR not available (baseline case or missing baseline file)
                        continue
                    ok = psnr >= threshold
                    marker = "PASS" if ok else "FAIL"
                    print(
                        f"[MATRIX PSNR GATE] {r.case_name} {stage_attr}: "
                        f"{psnr:.2f} dB {'≥' if ok else '<'} {threshold} dB  [{marker}]"
                    )
                    if not ok:
                        gate_failed = True

    if gate_failed:
        print("[MATRIX PSNR GATE] FAIL — one or more cases below threshold")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
