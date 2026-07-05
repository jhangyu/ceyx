#!/usr/bin/env python3
# ---
# file_summary: >
#   SDK vs Halide Metal pipeline 比較矩陣工具。
#   執行 Lossless/Lossy × SDK/Halide Metal 四組 case，
#   解析 stdout 的 Stage1-4 時間與 PSNR，支援 repeat timing 平均與逐輪 correctness gate，輸出 Markdown 表格。
#
# cases:
#   Lossless / SDK:      baseline mode，全 SDK Stage3+Stage4，作為 reference（存 .raw）
#   Lossless / Halide:   Halide bilinear/fused Stage3 + Halide Metal render
#   Lossless / Halide Metal (standalone fallback):
#                        同一 fixture + cmd，env 加 DNG_FUSED_DEMOSAIC_WARP=0，
#                        強制 dng_pipeline_v2.cpp 走 !fused 分支（standalone
#                        demosaic_bilinear_halide_aot + rectilinear_warp 兩顆
#                        AOT kernel）。D-A (2026-07-05) 新增，補上此分支從未
#                        被 matrix 覆蓋到的缺口；PSNR 門檻沿用 "Lossless /
#                        Halide" 前綴比對（同一組 kernel 數學，僅排程不同，
#                        實測輸出與 fused 路徑 byte-identical）。
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
#
# harness_auto_enable: >
#   The FFI (dng_ffi_harness) and device-handoff (test_device_handoff) cases
#   are auto-enabled on macOS whenever their default build outputs exist
#   (dng_processor/native/build/dng_ffi_harness,
#   dng_processor/native/build/test_device_handoff) — no flag needed once
#   both targets are built. Use --ffi-harness/--device-handoff-harness to
#   point at a non-default binary, or --no-ffi-harness/
#   --no-device-handoff-harness to opt out even if the default binary exists.
#
# 注意：--output 只寫 markdown 報告；raw/ppm/pgm 中間產物一律落到 <repo>/artifacts/。
# artifacts/ 是 ephemeral workspace；每次 matrix run 開始前會清空，避免跨 run 囤積不同版本。
# 嚴禁透過 --artifact-dir 把 raw 檔導向 docs/logs（會被 .gitignore 阻擋並造成空間污染）。
#
# auto_diff:
#   - name: "_auto_diff_on_failure"
#     description: "gate FAIL 時寫入文字診斷；同次 raw 齊全時執行 contract-first PSNR 與 heatmap"
#     lines: "1270-1381"
# ---
import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional


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
    contract_pass: bool
    rgb_match_pass: bool
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

    def _aggregate_stage(self, attr: str) -> StageResult:
        stages = [getattr(r, attr) for r in self.runs]
        psnr_values = [s.psnr_db for s in stages if s.psnr_db is not None]
        return StageResult(
            time_ms=mean_optional([s.time_ms for s in stages]),
            psnr_db=min(psnr_values) if psnr_values else None,
        )

    @property
    def stage1(self) -> StageResult: return self._aggregate_stage("stage1")
    @property
    def stage2(self) -> StageResult: return self._aggregate_stage("stage2")
    @property
    def stage3(self) -> StageResult: return self._aggregate_stage("stage3")
    @property
    def stage4(self) -> StageResult: return self._aggregate_stage("stage4")

    @property
    def total_ms(self) -> Optional[float]:
        return mean_optional([r.total_ms for r in self.runs])

    def stage4_runs_str(self) -> str:
        vals = [r.stage4.time_ms for r in self.runs if r.stage4.time_ms is not None]
        return ", ".join(f"{v:.1f}" for v in vals) if vals else "N/A"

    def stage3_probe_avg(self, key: str) -> Optional[float]:
        return mean_optional([r.stage3_probe.get(key) for r in self.runs])

    def stage3_workspace_acquire_avg(self) -> Optional[float]:
        # W2-02 rename: the Stage3 probe key was renamed resize -> acquire
        # (production field DngPipelineStage3Timing::workspace_acquire_ms).
        # Accept the canonical new key, the emitted short key, and the legacy
        # key for migration robustness.
        def pick(probe: dict[str, float]) -> Optional[float]:
            for key in ("workspaceAcquire", "acquire", "resize"):
                if key in probe:
                    return probe[key]
            return None
        return mean_optional([pick(r.stage3_probe) for r in self.runs])

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


@dataclass
class FailedGateInfo:
    gate_kind: str
    case_name: str
    stage: str
    actual_psnr: Optional[float]
    threshold: Optional[float]
    raw_prefix: str
    reference_raw: Optional[Path]
    candidate_raw: Optional[Path]
    expected_hash: Optional[str] = None
    actual_hash: Optional[str] = None


@dataclass
class DeviceHandoffResult:
    sample_name: str
    psnr_db: float
    gate_pass: bool
    byte_exact: bool


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

_STAGE_RE = re.compile(r"^--- Stage (\d+):")
_TIME_RE = re.compile(r"^\s*Time:\s*([0-9]+(?:\.[0-9]+)?)\s*ms")
_PSNR_RE = re.compile(r"^\s*PSNR vs baseline:\s*([0-9]+(?:\.[0-9]+)?)\s*dB")
_TOTAL_RE = re.compile(r"^\s*(?:DECODE )?TOTAL:\s*([0-9]+(?:\.[0-9]+)?)\s*ms")
_STAGE_CONTRACT_RE = re.compile(
    r"^\s*\[Contract\]\s+(Stage[1-4])\b.*->\s*(PASS|FAIL)\s*$"
)
_EXPECTED_NATIVE_CONTRACT_STAGES = ("Stage1", "Stage2", "Stage3", "Stage4")
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
_CONTRACT_PASS_RE = re.compile(r"^\s*\[Contract\] PASS\b")
_CONTRACT_FAIL_RE = re.compile(r"^\s*\[Contract\] FAIL\b")
_POOL_PASS_RE = re.compile(r"^\[Pool\] PASS\b")
_FFI_RGB_MATCH_RE = re.compile(
    r"^\[FFI RGB MATCH\]\s+render:\s+byte_exact=(\d+)\s+"
    r"psnr=([0-9]+(?:\.[0-9]+)?)\s+dB\s+\[(PASS|FAIL)\]"
)
_HANDOFF_PSNR_RE = re.compile(
    r"^\s*PSNR\(handoff ON vs OFF\):\s*([0-9]+(?:\.[0-9]+)?)\s+"
    r"dB\s+\[(PASS|FAIL)\]"
)
_KV_FLOAT_RE = re.compile(r"\b([A-Za-z0-9_]+)=(-?[0-9]+(?:\.[0-9]+)?)")
_DEFAULT_ANDROID_TEST_DECODE = (
    "dng_processor/native/build-android/android-arm64/test_decode_android"
)
_DEFAULT_FFI_HARNESS = "dng_processor/native/build/dng_ffi_harness"
_DEFAULT_DEVICE_HANDOFF_HARNESS = "dng_processor/native/build/test_device_handoff"
_DEFAULT_ANDROID_FFI_HARNESS = (
    "dng_processor/native/build-android/android-arm64/dng_ffi_harness_android"
)
_DEFAULT_ANDROID_DEVICE_HANDOFF_HARNESS = (
    "dng_processor/native/build-android/android-arm64/test_device_handoff_android"
)


def _accumulate_opcode2_timing(dst: dict[str, float], payload: str) -> None:
    values = {k: float(v) for k, v in _KV_FLOAT_RE.findall(payload)}
    for key in ("prewarm", "gather", "kernel", "copy_to_host", "scatter", "t"):
        if key in values:
            dst[key] = dst.get(key, 0.0) + values[key]


# ---------------------------------------------------------------------------
# Android ADB helpers (ported from run_android_w0_gate.py)
# ---------------------------------------------------------------------------

def _parse_attached_devices(adb: str) -> tuple[list[str], str]:
    """Parse `adb devices` output, return (serials, raw_output)."""
    proc = subprocess.run(
        [adb, "devices"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"adb devices failed with exit {proc.returncode}\n{proc.stdout}")
    devices: list[str] = []
    for line in proc.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            devices.append(parts[0])
    return devices, proc.stdout


def _adb_device_attached(adb: str) -> bool:
    """Best-effort check for at least one authorized ADB device.

    Returns False (never raises) when adb is missing/not on PATH, the
    `adb devices` invocation fails, or no device is currently in the
    'device' state. Used only to decide whether `--platform all` should
    gracefully skip Android cases; the explicit `--platform android` path
    still hard-fails via `_resolve_serial`/`ap.error` so an explicit
    request is never silently skipped.
    """
    if not adb:
        return False
    try:
        devices, _raw = _parse_attached_devices(adb)
    except (RuntimeError, OSError):
        return False
    return bool(devices)


def _resolve_serial(adb: str, requested: Optional[str]) -> str:
    """Resolve ADB serial: use requested, or auto-detect if exactly one device."""
    if requested:
        proc = subprocess.run(
            [adb, "-s", requested, "get-state"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if proc.returncode != 0 or proc.stdout.strip() != "device":
            raise RuntimeError(f"ADB device is not ready: {requested}")
        return requested

    devices, raw = _parse_attached_devices(adb)
    if not devices:
        raise RuntimeError(f"No attached ADB device in 'device' state.\n{raw}")
    if len(devices) > 1:
        raise RuntimeError(
            "Multiple attached ADB devices; pass --android-serial. "
            f"Devices: {', '.join(devices)}"
        )
    return devices[0]


def _adb_cmd(adb: str, serial: str, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    """Run `adb -s <serial> <args>` and return result."""
    cmd = [adb, "-s", serial, *args]
    print(f"[ADB] {' '.join(shlex.quote(part) for part in cmd)}")
    proc = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if proc.stdout:
        print(proc.stdout, end="" if proc.stdout.endswith("\n") else "\n")
    if check and proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, cmd, output=proc.stdout)
    return proc


def _libcxx_path(ndk_path: Path, abi: str) -> Optional[Path]:
    """Locate libc++_shared.so in the NDK for the given ABI."""
    triples = {
        "arm64-v8a": "aarch64-linux-android",
        "armeabi-v7a": "arm-linux-androideabi",
        "x86": "i686-linux-android",
        "x86_64": "x86_64-linux-android",
    }
    triple = triples.get(abi)
    if not triple:
        return None
    matches = sorted(
        (ndk_path / "toolchains" / "llvm" / "prebuilt").glob(
            f"*/sysroot/usr/lib/{triple}/libc++_shared.so"
        )
    )
    return matches[0] if matches else None


def _adb_stage_binaries(
    adb: str,
    serial: str,
    test_decode_local: Path,
    probe_local: Optional[Path],
    shared_libs: list[Path],
    lossless_local: Path,
    lossy_local: Path,
    remote_dir: str,
) -> None:
    """Push all required files to Android device in one batch.

    Steps:
    1. adb shell rm -rf <remote_dir> && mkdir -p <remote_dir>/artifacts <remote_dir>/samples
    2. adb push test_decode_android -> <remote_dir>/
    3. adb push test_android_vulkan_capability -> <remote_dir>/ (if exists)
    4. adb push shared libs -> <remote_dir>/ (if any)
    5. adb push lossless.dng -> <remote_dir>/samples/
    6. adb push lossy.dng -> <remote_dir>/samples/
    """
    remote_artifacts = f"{remote_dir}/artifacts"
    remote_samples = f"{remote_dir}/samples"
    remote_bin = f"{remote_dir}/test_decode_android"
    remote_probe = f"{remote_dir}/test_android_vulkan_capability"

    # Clean and create directories
    _adb_cmd(adb, serial, "shell",
             f"rm -rf {shlex.quote(remote_dir)} && "
             f"mkdir -p {shlex.quote(remote_artifacts)} {shlex.quote(remote_samples)}")

    # Push test binary
    _adb_cmd(adb, serial, "push", str(test_decode_local), remote_bin)
    _adb_cmd(adb, serial, "shell", f"chmod 755 {shlex.quote(remote_bin)}")

    # Push probe binary (optional)
    if probe_local and probe_local.exists():
        _adb_cmd(adb, serial, "push", str(probe_local), remote_probe)
        _adb_cmd(adb, serial, "shell", f"chmod 755 {shlex.quote(remote_probe)}")

    # Push shared libraries if this build needs any sidecar .so files.
    for lib in shared_libs:
        _adb_cmd(adb, serial, "push", str(lib), f"{remote_dir}/{lib.name}")

    # Push DNG samples
    _adb_cmd(adb, serial, "push", str(lossless_local), f"{remote_samples}/{lossless_local.name}")
    _adb_cmd(adb, serial, "push", str(lossy_local), f"{remote_samples}/{lossy_local.name}")


def _adb_run_vulkan_probe(adb: str, serial: str, remote_dir: str) -> None:
    """Run Vulkan capability probe on device. Raise RuntimeError if fails."""
    remote_probe = f"{remote_dir}/test_android_vulkan_capability"
    probe_cmd = (
        f"cd {shlex.quote(remote_dir)} && "
        f"export LD_LIBRARY_PATH={shlex.quote(remote_dir)}:$LD_LIBRARY_PATH && "
        f"{shlex.quote(remote_probe)}"
    )
    probe_proc = _adb_cmd(adb, serial, "shell", probe_cmd, check=False)
    if probe_proc.returncode != 0:
        raise RuntimeError(
            "Android Vulkan capability probe failed; "
            "device cannot run the configured Halide Vulkan AOT target"
        )


def _adb_pidof(adb: str, serial: str, process_name: str) -> list[str]:
    """Return the list of PIDs for `process_name` on device (empty if none)."""
    proc = subprocess.run(
        [adb, "-s", serial, "shell", "pidof", process_name],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if proc.returncode != 0 or not proc.stdout.strip():
        return []
    return proc.stdout.split()


def _adb_wait_process_exit(
    adb: str,
    serial: str,
    process_name: str,
    timeout_sec: float = 20.0,
    poll_interval_sec: float = 0.3,
) -> bool:
    """Poll until no process named `process_name` remains on device.

    Root cause (2026-07-04 investigation, see Task_android_perf_diag_measure.md
    Gap 1): the GPU/Vulkan device teardown for `test_decode_android` /
    `dng_ffi_harness_android` continues briefly *after* `adb shell` returns to
    the caller (stdout closes before the process fully exits). Launching the
    next invocation (baseline->test within one round, or round N -> N+1)
    while the previous process is still tearing down its Vulkan device causes
    intermittent corruption: the next process's baseline-file reads fail
    ("Could not load baseline file") or the decode itself throws a DNG
    Exception. Confirmed via `pidof` polling in a real repro (dbg log:
    docs/logs/2026-07-04/android_repeat_investigation.md).

    Returns True once no matching process is found, False on timeout (the
    caller should log a warning, not hard-fail, since `pidof` itself can be
    momentarily flaky and a false-timeout should not abort a whole run).
    """
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        if not _adb_pidof(adb, serial, process_name):
            return True
        time.sleep(poll_interval_sec)
    return not _adb_pidof(adb, serial, process_name)


def _build_android_cases(
    lossless_remote: str,
    lossy_remote: str,
    remote_artifacts: str,
    extra_env: dict[str, str],
    enable_timing: bool,
    include_lossy: bool,
) -> list[tuple[str, list[str], list[str], dict[str, str], dict[str, str]]]:
    """Return Android test cases with device-side baseline and Vulkan test.

    Returns:
        [(case_name, baseline_args, test_args, baseline_env, test_env), ...]

    test_env includes:
        DNG_GPU_BACKEND=vulkan
        DNG_TEST_DECODE_ARTIFACT_DIR=<remote_artifacts>
        Optional timing envs when enable_timing is True.
    """
    baseline_env: dict[str, str] = {
        "DNG_TEST_DECODE_ARTIFACT_DIR": remote_artifacts,
        **extra_env,
    }
    test_env: dict[str, str] = {
        **baseline_env,
        "DNG_GPU_BACKEND": "vulkan",
    }
    if enable_timing:
        timing_env = {
            "DNG_STAGE1_TIMING": "1",
            "DNG_MAP_POLY_TIMING": "1",
            "DNG_STAGE2_SDK_TIMING": "1",
        }
        baseline_env.update(timing_env)
        test_env.update(timing_env)

    cases = [
        (
            "Lossless / Halide GPU (Android)",
            [lossless_remote, "baseline", "halide-gpu", "cpu"],
            [lossless_remote, "test", "halide-gpu", "halide-gpu"],
            dict(baseline_env),
            dict(test_env),
        ),
    ]
    if include_lossy:
        cases.append(
            (
                "Lossy / Halide GPU (Android)",
                [lossy_remote, "baseline", "auto", "cpu"],
                [lossy_remote, "test", "auto", "halide-gpu"],
                dict(baseline_env),
                dict(test_env),
            )
        )
    return cases


def _parse_test_decode_output(
    output: str,
    returncode: int,
    case_name: str,
    *,
    require_android_vulkan_gate: bool = False,
) -> RunResult:
    """Parse test_decode stdout and validate contract markers.

    Shared between macOS (_run_case) and Android (_run_android_case) since
    both compile from the same test_decode.cpp and emit identical output.
    """
    stages: dict[str, StageResult] = {str(i): StageResult() for i in range(1, 5)}
    cur: Optional[str] = None
    total_ms: Optional[float] = None
    stage3_probe: dict[str, float] = {}
    stage2_probe: dict[str, float] = {}
    opcode2_probe: dict[str, float] = {}
    contract_statuses: dict[str, list[str]] = {}

    for line in output.splitlines():
        m = _STAGE_CONTRACT_RE.match(line)
        if m:
            contract_statuses.setdefault(m.group(1), []).append(m.group(2))
            continue
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

    missing_contracts = [
        stage for stage in _EXPECTED_NATIVE_CONTRACT_STAGES
        if "PASS" not in contract_statuses.get(stage, [])
    ]
    failed_contracts = [
        stage for stage, statuses in contract_statuses.items()
        if "FAIL" in statuses
    ]
    if returncode != 0 or missing_contracts or failed_contracts:
        details = []
        if missing_contracts:
            details.append(f"missing PASS markers: {', '.join(missing_contracts)}")
        if failed_contracts:
            details.append(f"FAIL markers: {', '.join(sorted(failed_contracts))}")
        suffix = f" ({'; '.join(details)})" if details else ""
        raise RuntimeError(f"[{case_name}] exit={returncode}{suffix}\n{output}")

    if require_android_vulkan_gate:
        details = []
        if "GPU backend: vulkan" not in output:
            details.append("missing 'GPU backend: vulkan' marker")
        if "[PSNR GATE] ALL PASS" not in output:
            details.append("missing PSNR gate pass marker")
        if details:
            raise RuntimeError(
                f"[{case_name}] exit={returncode} ({'; '.join(details)})\n{output}"
            )

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


def _adb_repush_binary(
    adb: str, serial: str, local_path: Path, remote_path: str
) -> None:
    """Re-push a single binary and restore its exec bit (127-recovery path)."""
    print(f"[WARN] re-pushing {remote_path} (recovering from exit=127)")
    _adb_cmd(adb, serial, "push", str(local_path), remote_path)
    _adb_cmd(adb, serial, "shell", f"chmod 755 {shlex.quote(remote_path)}")


def _adb_shell_with_repush_retry(
    adb: str,
    serial: str,
    shell_command: str,
    *,
    local_bin: Path,
    remote_bin: str,
) -> subprocess.CompletedProcess:
    """Run `adb shell <shell_command>`, retrying once after re-pushing the
    binary if the shell reports exit=127 ("inaccessible or not found").

    See Task_android_perf_diag_measure.md Gap 1: the pushed binary was
    observed to become unreachable between matrix repeats. `_adb_wait_process_exit`
    (called by the caller between invocations) addresses the confirmed root
    cause (GPU teardown race); this retry is a defensive second layer in case
    the remote binary is genuinely missing/corrupted for an unrelated reason
    (e.g. a concurrent device cleanup).
    """
    proc = _adb_cmd(adb, serial, "shell", shell_command, check=False)
    if proc.returncode == 127:
        _adb_repush_binary(adb, serial, local_bin, remote_bin)
        proc = _adb_cmd(adb, serial, "shell", shell_command, check=False)
    return proc


def _run_android_case(
    adb: str,
    serial: str,
    remote_dir: str,
    baseline_args: list[str],
    test_args: list[str],
    case_name: str,
    baseline_env: dict[str, str],
    test_env: dict[str, str],
    *,
    local_bin: Path,
    process_name: str = "test_decode_android",
) -> RunResult:
    """Execute one Android test case via ADB shell.

    Steps:
    1. Clear remote artifacts.
    2. Run device-side SDK baseline to generate raw references.
    3. Wait for the baseline process to fully exit (GPU/Vulkan device
       teardown continues briefly after `adb shell` returns; see
       `_adb_wait_process_exit` docstring for the confirmed root cause of the
       Gap-1 --repeat>1 corruption).
    4. Run Vulkan test against those references.
    5. Wait for the test process to fully exit before returning, so the next
       round (or the next case) never overlaps with this one's teardown.
    6. Parse stdout using shared _parse_test_decode_output.
    """
    remote_artifacts = f"{remote_dir}/artifacts"
    remote_bin = f"{remote_dir}/test_decode_android"

    _adb_cmd(adb, serial, "shell",
             f"rm -f {shlex.quote(remote_artifacts)}/*.raw", check=False)

    def shell_cmd(remote_args: list[str], env: dict[str, str]) -> str:
        exports = " ; ".join(
            f"export {k}={shlex.quote(v)}"
            for k, v in env.items()
        )
        args_str = " ".join(shlex.quote(a) for a in remote_args)
        return (
            f"cd {shlex.quote(remote_artifacts)} ; "
            f"export LD_LIBRARY_PATH={shlex.quote(remote_dir)}:$LD_LIBRARY_PATH ; "
            f"{exports} ; "
            f"{shlex.quote(remote_bin)} {args_str}"
        )

    baseline = _adb_shell_with_repush_retry(
        adb, serial, shell_cmd(baseline_args, baseline_env),
        local_bin=local_bin, remote_bin=remote_bin,
    )
    if baseline.returncode != 0:
        raise RuntimeError(
            f"[{case_name} baseline] exit={baseline.returncode}\n{baseline.stdout}"
        )
    if not _adb_wait_process_exit(adb, serial, process_name):
        print(
            f"[WARN] {case_name}: {process_name} still visible after baseline "
            "teardown timeout; proceeding anyway (best-effort wait)"
        )

    proc = _adb_shell_with_repush_retry(
        adb, serial, shell_cmd(test_args, test_env),
        local_bin=local_bin, remote_bin=remote_bin,
    )
    result = _parse_test_decode_output(
        proc.stdout,
        proc.returncode,
        case_name,
        require_android_vulkan_gate=True,
    )
    if not _adb_wait_process_exit(adb, serial, process_name):
        print(
            f"[WARN] {case_name}: {process_name} still visible after test "
            "teardown timeout; proceeding anyway (best-effort wait)"
        )
    return result


def _run_case(cwd: Path, cmd: list[str], case_name: str, env: dict[str, str]) -> RunResult:
    merged = os.environ.copy()
    merged.update(env)
    merged["DNG_TEST_DECODE_ARTIFACT_DIR"] = str(cwd)
    proc = subprocess.run(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=merged,
        check=False,
    )
    return _parse_test_decode_output(proc.stdout, proc.returncode, case_name)


def _run_ffi_case(cwd: Path, harness: str, sample_name: str, dng_path: str,
                  env: dict[str, str], artifact_dir: Path) -> FfiRunResult:
    merged = os.environ.copy()
    merged.update(env)
    proc = subprocess.run(
        [harness, dng_path, "1", "--save-raw", str(artifact_dir)],
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
    contract_pass = False
    contract_failed = False
    rgb_match: Optional[re.Match[str]] = None
    for line in output.splitlines():
        m = _FFI_RUN_RE.match(line)
        if m:
            ffi_match = m
        if _CONTRACT_PASS_RE.match(line):
            contract_pass = True
        if _CONTRACT_FAIL_RE.match(line):
            contract_failed = True
        m = _FFI_RGB_MATCH_RE.match(line)
        if m:
            rgb_match = m
        m = _STAGE2_PROBE_RE.match(line)
        if m:
            stage2_probe = {k: float(v) for k, v in _KV_FLOAT_RE.findall(m.group(1))}
        m = _OPCODE2_TIMING_RE.match(line)
        if m:
            _accumulate_opcode2_timing(opcode2_probe, m.group(1))
    rgb_match_pass = bool(
        rgb_match and rgb_match.group(1) == "1" and rgb_match.group(3) == "PASS"
    )
    if (
        proc.returncode != 0
        or not ffi_match
        or not contract_pass
        or contract_failed
        or not rgb_match_pass
    ):
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
        contract_pass=contract_pass,
        rgb_match_pass=rgb_match_pass,
        opcode2_probe=opcode2_probe,
    )


def _shell_export_prefix(env: dict[str, str]) -> str:
    """Build a ` ; `-joined `export K=V` prefix, empty string when env is
    empty (avoids emitting a stray `; ;` that /system/bin/sh rejects as a
    syntax error)."""
    if not env:
        return ""
    exports = " ; ".join(f"export {k}={shlex.quote(v)}" for k, v in env.items())
    return f"{exports} ; "


def _adb_push_extra_binary(
    adb: str, serial: str, local_path: Path, remote_dir: str
) -> str:
    """Push one extra standalone binary into remote_dir, chmod +x, return its
    remote path."""
    remote_path = f"{remote_dir}/{local_path.name}"
    _adb_cmd(adb, serial, "push", str(local_path), remote_path)
    _adb_cmd(adb, serial, "shell", f"chmod 755 {shlex.quote(remote_path)}")
    return remote_path


def _parse_android_ffi_output(
    output: str,
    returncode: int,
    sample_name: str,
    expected_runs: int,
) -> list[FfiRunResult]:
    """Parse `dng_ffi_harness_android <dng> <repeat_count>` stdout.

    The harness loops `repeat_count` times inside a single process (no
    --save-raw: there is no repo root / host Halide reference render
    on-device, see dng_ffi_harness.cpp's resolveArtifactDir), so this parser
    splits the output into one segment per `[FFI run k]` line and extracts a
    FfiRunResult per segment. `rgb_match_pass` is trivially True — RGB
    byte-exact comparison against a macOS-generated reference is out of
    scope on-device and is never attempted (writeAndCompareRgb is only
    called when --save-raw is passed).
    """
    lines = output.splitlines()
    segments: list[list[str]] = []
    current: list[str] = []
    for line in lines:
        current.append(line)
        if _FFI_RUN_RE.match(line):
            segments.append(current)
            current = []

    pool_pass = any(_POOL_PASS_RE.match(line) for line in lines)

    results: list[FfiRunResult] = []
    for seg in segments:
        stage2_probe: dict[str, float] = {}
        opcode2_probe: dict[str, float] = {}
        ffi_match: Optional[re.Match[str]] = None
        contract_pass = False
        contract_failed = False
        for line in seg:
            m = _FFI_RUN_RE.match(line)
            if m:
                ffi_match = m
            if _CONTRACT_PASS_RE.match(line):
                contract_pass = True
            if _CONTRACT_FAIL_RE.match(line):
                contract_failed = True
            m = _STAGE2_PROBE_RE.match(line)
            if m:
                stage2_probe = {k: float(v) for k, v in _KV_FLOAT_RE.findall(m.group(1))}
            m = _OPCODE2_TIMING_RE.match(line)
            if m:
                _accumulate_opcode2_timing(opcode2_probe, m.group(1))
        if ffi_match is None or not contract_pass or contract_failed:
            continue
        results.append(FfiRunResult(
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
            contract_pass=contract_pass,
            rgb_match_pass=True,
            opcode2_probe=opcode2_probe,
        ))

    if returncode != 0 or not pool_pass or len(results) != expected_runs:
        raise RuntimeError(
            f"[Android FFI {sample_name}] exit={returncode} "
            f"parsed_runs={len(results)}/{expected_runs} pool_pass={pool_pass}\n{output}"
        )
    return results


def _run_android_ffi_case(
    adb: str,
    serial: str,
    remote_dir: str,
    remote_bin: str,
    sample_name: str,
    remote_dng_path: str,
    env: dict[str, str],
    repeat_count: int,
    *,
    local_bin: Path,
    process_name: str = "dng_ffi_harness_android",
) -> list[FfiRunResult]:
    """Execute the Android FFI harness for `repeat_count` runs in ONE process
    invocation (the harness's own internal repeat loop), which sidesteps the
    inter-process GPU/Vulkan teardown race entirely (see
    `_adb_wait_process_exit`) since there is only one process per call.
    """
    cmd_str = (
        f"cd {shlex.quote(remote_dir)} ; "
        f"export LD_LIBRARY_PATH={shlex.quote(remote_dir)}:$LD_LIBRARY_PATH ; "
        f"{_shell_export_prefix(env)}"
        f"{shlex.quote(remote_bin)} {shlex.quote(remote_dng_path)} {repeat_count}"
    )
    proc = _adb_shell_with_repush_retry(
        adb, serial, cmd_str, local_bin=local_bin, remote_bin=remote_bin,
    )
    results = _parse_android_ffi_output(proc.stdout, proc.returncode, sample_name, repeat_count)
    if not _adb_wait_process_exit(adb, serial, process_name):
        print(
            f"[WARN] Android FFI {sample_name}: {process_name} still visible "
            "after teardown timeout; proceeding anyway (best-effort wait)"
        )
    return results


def _run_device_handoff(
    cwd: Path,
    harness: str,
    lossless: str,
    lossy: str,
    env: dict[str, str],
) -> list[DeviceHandoffResult]:
    merged = os.environ.copy()
    merged.update(env)
    proc = subprocess.run(
        [harness, lossless, lossy],
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=merged,
        check=False,
    )
    output = proc.stdout
    contract_passes = sum(
        1 for line in output.splitlines() if _CONTRACT_PASS_RE.match(line)
    )
    contract_failed = any(
        _CONTRACT_FAIL_RE.match(line) for line in output.splitlines()
    )
    matches = [
        _HANDOFF_PSNR_RE.match(line)
        for line in output.splitlines()
        if _HANDOFF_PSNR_RE.match(line)
    ]
    if proc.returncode != 0 or contract_failed or contract_passes != 2 or len(matches) != 2:
        raise RuntimeError(f"[Device Handoff] exit={proc.returncode}\n{output}")

    labels = ("Lossless / Stage3-Stage4", "Lossy / Stage2-Stage4")
    results: list[DeviceHandoffResult] = []
    for label, match in zip(labels, matches):
        assert match is not None
        psnr = float(match.group(1))
        gate_pass = match.group(2) == "PASS" and psnr >= 99.0
        if not gate_pass:
            raise RuntimeError(f"[Device Handoff] gate failed\n{output}")
        results.append(DeviceHandoffResult(
            sample_name=label,
            psnr_db=psnr,
            gate_pass=gate_pass,
            byte_exact=psnr >= 998.0,
        ))
    return results


def _run_android_device_handoff(
    adb: str,
    serial: str,
    remote_dir: str,
    remote_bin: str,
    remote_lossless: str,
    remote_lossy: str,
    env: dict[str, str],
    *,
    local_bin: Path,
    process_name: str = "test_device_handoff_android",
) -> list[DeviceHandoffResult]:
    """Run `test_device_handoff_android <lossless> <lossy>` on device.

    Mirrors `_run_device_handoff` (macOS): same CLI signature, same
    [Contract]/PSNR(handoff ON vs OFF) output contract (see
    test_device_handoff.cpp), just executed over adb shell instead of a
    local subprocess.
    """
    cmd_str = (
        f"cd {shlex.quote(remote_dir)} ; "
        f"export LD_LIBRARY_PATH={shlex.quote(remote_dir)}:$LD_LIBRARY_PATH ; "
        f"{_shell_export_prefix(env)}"
        f"{shlex.quote(remote_bin)} {shlex.quote(remote_lossless)} {shlex.quote(remote_lossy)}"
    )
    proc = _adb_shell_with_repush_retry(
        adb, serial, cmd_str, local_bin=local_bin, remote_bin=remote_bin,
    )
    output = proc.stdout
    contract_passes = sum(
        1 for line in output.splitlines() if _CONTRACT_PASS_RE.match(line)
    )
    contract_failed = any(
        _CONTRACT_FAIL_RE.match(line) for line in output.splitlines()
    )
    matches = [
        _HANDOFF_PSNR_RE.match(line)
        for line in output.splitlines()
        if _HANDOFF_PSNR_RE.match(line)
    ]
    if not _adb_wait_process_exit(adb, serial, process_name):
        print(
            f"[WARN] Android Device Handoff: {process_name} still visible "
            "after teardown timeout; proceeding anyway (best-effort wait)"
        )
    if proc.returncode != 0 or contract_failed or contract_passes != 2 or len(matches) != 2:
        raise RuntimeError(f"[Android Device Handoff] exit={proc.returncode}\n{output}")

    labels = ("Lossless / Stage3-Stage4 (Android)", "Lossy / Stage2-Stage4 (Android)")
    results: list[DeviceHandoffResult] = []
    for label, match in zip(labels, matches):
        assert match is not None
        psnr = float(match.group(1))
        gate_pass = match.group(2) == "PASS" and psnr >= 99.0
        if not gate_pass:
            raise RuntimeError(f"[Android Device Handoff] gate failed\n{output}")
        results.append(DeviceHandoffResult(
            sample_name=label,
            psnr_db=psnr,
            gate_pass=gate_pass,
            byte_exact=psnr >= 998.0,
        ))
    return results


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
    handoff_results: list[DeviceHandoffResult],
    android_results: list[AggResult],
    android_ffi_results: list[FfiAggResult],
    generated_at: str,
    repeat: int,
    show_halide_timing: bool,
    artifact_dir: Path,
    android_serial: Optional[str] = None,
) -> str:
    L: list[str] = []

    L.append("# DNG Pipeline Matrix — SDK vs Halide Metal")
    L.append("")
    L.append(
        f"_Generated: {generated_at} | Repeat: {repeat} "
        "(timing arithmetic mean; PSNR minimum across runs)_"
    )
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
    for r in android_results:
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
        L.append("| Sample | decode_ms | process_ms | wall_ms | Stage2 total | Stage2 doBuildStage2 | Stage2 opcode2 | rgb_bytes | Contract | RGB exact |")
        L.append("|---|---|---|---|---|---|---|---|---|---|")
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
                f"| {rgb_bytes} "
                f"| {'PASS' if all(run.contract_pass for run in r.runs) else 'FAIL'} "
                f"| {'PASS' if all(run.rgb_match_pass for run in r.runs) else 'FAIL'} |"
            )
        L.append("")

    if android_ffi_results:
        L.append("## Android FFI Production Timing (ms, production C ABI, single process)")
        L.append("")
        L.append("_Runs execute in one on-device process invocation (repeat_count passed directly "
                 "to the harness) so no inter-process GPU/Vulkan teardown race is possible. There is "
                 "no host-side Halide reference render on-device, so RGB byte-exact match against a "
                 "macOS baseline is not attempted here — only `[Contract]` and the pool leak-check gate."
                 " Contract PASS means the interleaved RGBA8 output passed shape/alpha validation on "
                 "every run._")
        L.append("")
        L.append("| Sample | decode_ms | process_ms | wall_ms | Stage2 total | Stage2 doBuildStage2 | Stage2 opcode2 | rgb_bytes | Contract |")
        L.append("|---|---|---|---|---|---|---|---|---|")
        for r in android_ffi_results:
            rgb_bytes = str(r.rgb_bytes) if r.rgb_bytes is not None else "N/A"
            L.append(
                f"| {r.sample_name} "
                f"| {_fmt_ms(r.decode_ms)} "
                f"| {_fmt_ms(r.process_ms)} "
                f"| {_fmt_ms(r.wall_ms)} "
                f"| {_fmt_ms(r.stage2_total_ms)} "
                f"| {_fmt_ms(r.stage2_do_build_ms)} "
                f"| {_fmt_ms(r.stage2_opcode2_ms)} "
                f"| {rgb_bytes} "
                f"| {'PASS' if all(run.contract_pass for run in r.runs) else 'FAIL'} |"
            )
        L.append("")

    if handoff_results:
        L.append("## Device Handoff PSNR")
        L.append("")
        L.append("| Sample | PSNR | >=99dB gate | Byte exact |")
        L.append("|---|---:|---|---|")
        for r in handoff_results:
            L.append(
                f"| {r.sample_name} | {r.psnr_db:.2f} dB "
                f"| {'PASS' if r.gate_pass else 'FAIL'} "
                f"| {'yes' if r.byte_exact else 'no'} |"
            )
        L.append("")

    # --- PSNR table ---
    L.append("## PSNR vs SDK Baseline (dB, minimum across runs)")
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
    for r in android_results:
        L.append(
            f"| {r.case_name} "
            f"| {_fmt_db(r.stage1.psnr_db)} "
            f"| {_fmt_db(r.stage2.psnr_db)} "
            f"| {_fmt_db(r.stage3.psnr_db)} "
            f"| {_fmt_db(r.stage4.psnr_db)} |"
        )
    L.append("")

    # --- Android Vulkan Timing (dedicated section) ---
    if android_results:
        L.append("## Android Vulkan Timing")
        L.append("")
        device_label = f"Device: {android_serial} | " if android_serial else ""
        L.append(f"_{device_label}GPU backend: Vulkan | device-side SDK baseline + PSNR gate_")
        L.append("")
        L.append("| Case | Stage1 | Stage2 | Stage3 | Stage4 | Total |")
        L.append("|---|---|---|---|---|---|")
        for r in android_results:
            L.append(
                f"| {r.case_name} "
                f"| {_fmt_ms(r.stage1.time_ms)} "
                f"| {_fmt_ms(r.stage2.time_ms)} "
                f"| {_fmt_ms(r.stage3.time_ms)} "
                f"| {_fmt_ms(r.stage4.time_ms)} "
                f"| {_fmt_ms(r.total_ms)} |"
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
            L.append("| Case | prealloc | workspace acquire | makeImage | demosaic | fused | applyOpcode3 | put | extractStage3 | sdkBuild | sdkExtract | unaccounted | production | validation extract | total |")
            L.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
            for r in results:
                L.append(
                    f"| {r.case_name} "
                    f"| {_fmt_ms(r.stage3_probe_avg('prealloc'))} "
                    f"| {_fmt_ms(r.stage3_workspace_acquire_avg())} "
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

        # --- Android Stage3 Probe Breakdown ---
        has_android_stage3_probe = any(
            r.stage3_probe_avg("total") is not None for r in android_results
        )
        if has_android_stage3_probe:
            L.append("## Android Stage3 Probe Breakdown (ms)")
            L.append("")
            L.append("| Case | workspace acquire | demosaic | fused | applyOpcode3 | total |")
            L.append("|---|---|---|---|---|---|")
            for r in android_results:
                L.append(
                    f"| {r.case_name} "
                    f"| {_fmt_ms(r.stage3_workspace_acquire_avg())} "
                    f"| {_fmt_ms(r.stage3_probe_avg('demosaic'))} "
                    f"| {_fmt_ms(r.stage3_probe_avg('fused'))} "
                    f"| {_fmt_ms(r.stage3_probe_avg('applyOpcode3'))} "
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
    for r in android_results:
        L.append(f"| {r.case_name} | {r.stage4_runs_str()} |")
    L.append("")

    return "\n".join(L)


# ---------------------------------------------------------------------------
# Regression baselines (JSON manifest)
# ---------------------------------------------------------------------------

def _compute_sha256(path: Path) -> str:
    """Compute SHA-256 hex digest for a file."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)  # 1 MiB
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def _load_regression_baselines(path: Path) -> dict[str, Any]:
    """Load kernel_regression_baselines.json and return as dict."""
    if not path.exists():
        raise FileNotFoundError(f"Regression baselines file not found: {path}")
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    print(f"[INFO] Loaded regression baselines v{data.get('version', '?')} from {path}")
    return data


def _build_psnr_thresholds_from_baselines(baselines: dict[str, Any]) -> dict[tuple[str, str], float]:
    """Convert JSON cases.*.thresholds to the (case_fragment, stage_attr) dict."""
    fallback: dict[tuple[str, str], float] = {
        ("Lossless / Halide", "stage1"): 999.0,
        ("Lossless / Halide", "stage2"): 999.0,
        ("Lossless / Halide", "stage3"): 100.0,
        ("Lossless / Halide", "stage4"): 75.0,
        ("Lossy / Halide",    "stage1"): 999.0,
        ("Lossy / Halide",    "stage2"): 999.0,
        ("Lossy / Halide",    "stage3"): 999.0,
        ("Lossy / Halide",    "stage4"): 999.0,
    }

    cases = baselines["cases"]

    result: dict[tuple[str, str], float] = {}
    # Map JSON case keys to runner case name fragments
    case_fragment_map = {
        "lossless": "Lossless / Halide",
        "lossy": "Lossy / Halide",
    }
    for json_key, fragment in case_fragment_map.items():
        case_data = cases.get(json_key, {})
        thresholds = case_data.get("thresholds", {})
        for stage in ("stage1", "stage2", "stage3", "stage4"):
            if stage in thresholds:
                result[(fragment, stage)] = float(thresholds[stage])
            elif (fragment, stage) in fallback:
                result[(fragment, stage)] = fallback[(fragment, stage)]
    return result


_SELECTED_ARTIFACT_CONTRACT_SEMANTICS = {
    "stage3": {
        "pixelType": "uint16",
        "pixelSize": 2,
        "layout": "interleaved RGB",
        "opcodeScope": "post-OpcodeList3",
    },
    "stage4": {
        "pixelType": "uint8",
        "pixelSize": 1,
        "layout": "interleaved RGB",
        "opcodeScope": "final post-Stage4 render",
    },
}


def _validate_selected_artifact_manifest(item: dict[str, Any]) -> bool:
    artifact_id = item.get("id", "unknown")
    contract = item.get("contract")
    required = (
        "width", "height", "planes", "pixelType", "pixelSize",
        "bufferSize", "layout", "opcodeScope",
    )
    if not isinstance(contract, dict):
        print(f"[ARTIFACT CONTRACT] {artifact_id}: FAIL — contract is missing")
        return False
    missing = [key for key in required if key not in contract]
    if missing:
        print(f"[ARTIFACT CONTRACT] {artifact_id}: FAIL — missing fields: {', '.join(missing)}")
        return False

    for key in ("width", "height", "planes", "pixelSize", "bufferSize"):
        if type(contract[key]) is not int or contract[key] <= 0:
            print(f"[ARTIFACT CONTRACT] {artifact_id}: FAIL — {key} must be a positive integer")
            return False
    calculated_size = (
        contract["width"]
        * contract["height"]
        * contract["planes"]
        * contract["pixelSize"]
    )
    if calculated_size != contract["bufferSize"]:
        print(
            f"[ARTIFACT CONTRACT] {artifact_id}: FAIL — "
            f"bufferSize={contract['bufferSize']}, calculated={calculated_size}"
        )
        return False

    stage = item.get("stage")
    semantics = _SELECTED_ARTIFACT_CONTRACT_SEMANTICS.get(stage)
    if semantics is None:
        print(f"[ARTIFACT CONTRACT] {artifact_id}: FAIL — unsupported stage: {stage!r}")
        return False
    for key, expected in semantics.items():
        if contract[key] != expected:
            print(
                f"[ARTIFACT CONTRACT] {artifact_id}: FAIL — "
                f"{key}={contract[key]!r}, expected {expected!r} for {stage}"
            )
            return False
    if contract["planes"] != 3:
        print(f"[ARTIFACT CONTRACT] {artifact_id}: FAIL — interleaved RGB requires planes=3")
        return False
    return True


def _validate_regression_baselines_schema(baselines: dict[str, Any]) -> bool:
    if baselines.get("version") != 1:
        print("[REGRESSION BASELINES] FAIL — expected schema version 1")
        return False
    fixtures = baselines.get("fixtures")
    cases = baselines.get("cases")
    selected = baselines.get("selected_artifacts")
    if not isinstance(fixtures, dict) or not {"lossless", "lossy"} <= fixtures.keys():
        print("[REGRESSION BASELINES] FAIL — fixtures must define lossless and lossy")
        return False
    if not isinstance(cases, dict) or not {"lossless", "lossy"} <= cases.keys():
        print("[REGRESSION BASELINES] FAIL — cases must define lossless and lossy")
        return False
    for case_name in ("lossless", "lossy"):
        thresholds = cases[case_name].get("thresholds")
        if not isinstance(thresholds, dict) or not {
            "stage1", "stage2", "stage3", "stage4"
        } <= thresholds.keys():
            print(f"[REGRESSION BASELINES] FAIL — {case_name} thresholds incomplete")
            return False
    if not isinstance(selected, list) or not selected:
        print("[REGRESSION BASELINES] FAIL — selected_artifacts must not be empty")
        return False
    for item in selected:
        if not isinstance(item, dict) or not _validate_selected_artifact_manifest(item):
            print("[REGRESSION BASELINES] FAIL — invalid selected artifact contract")
            return False
    return True


def _verify_fixture_hashes(baselines: dict[str, Any], root: Path) -> bool:
    """Verify DNG fixture SHA-256 hashes against manifest.

    Returns True if all locked fixture hashes match. Prints results.
    """
    fixtures = baselines.get("fixtures")
    if not fixtures:
        print("[FIXTURE HASH] FAIL — manifest has no locked fixtures")
        return False

    all_ok = True
    for name, info in fixtures.items():
        expected = info.get("sha256", "")
        if not expected:
            print(f"[FIXTURE HASH] {name}: FAIL — locked sha256 is empty")
            all_ok = False
            continue
        fixture_relpath = info.get("path", "")
        if not fixture_relpath:
            print(f"[FIXTURE HASH] {name}: FAIL — fixture path is empty")
            all_ok = False
            continue
        fixture_path = root / fixture_relpath
        if not fixture_path.exists():
            print(f"[FIXTURE HASH] {name}: FAIL — file not found: {fixture_path}")
            all_ok = False
            continue
        actual = _compute_sha256(fixture_path)
        ok = actual == expected
        marker = "PASS" if ok else "FAIL"
        print(f"[FIXTURE HASH] {name}: {marker}")
        if not ok:
            print(f"  expected: {expected}")
            print(f"  actual:   {actual}")
            all_ok = False
    return all_ok


def _run_sha256_gate(
    baselines: dict[str, Any],
    artifact_dir: Path,
    case_artifact_map: dict[str, Path],
    selected_artifacts: Optional[list[dict[str, Any]]] = None,
) -> tuple[bool, list[dict[str, str]]]:
    """Check SHA-256 of selected artifacts against manifest.

    Args:
        baselines: The loaded JSON manifest.
        artifact_dir: The directory containing current run artifacts.
        case_artifact_map: Maps "<fixture>/<artifact_pattern>" to actual file path.

    Returns:
        (all_passed, entries) where entries is a list of dicts for diff output.
    """
    selected = (
        baselines.get("selected_artifacts", [])
        if selected_artifacts is None
        else selected_artifacts
    )
    if not selected:
        print("[SHA256 GATE] FAIL — selected_artifacts is empty")
        return False, []

    all_ok = True
    entries: list[dict[str, str]] = []
    for item in selected:
        artifact_id = item.get("id", "unknown")
        expected_hash = item.get("sha256", "")
        pattern = item.get("artifact_pattern", "")
        fixture = item.get("fixture", "")
        lookup_key = f"{fixture}/{pattern}"

        actual_path = case_artifact_map.get(lookup_key)
        if actual_path is None or not actual_path.exists():
            print(f"[SHA256 GATE] {artifact_id}: FAIL — selected artifact not found")
            entries.append({
                "id": artifact_id,
                "expected": expected_hash,
                "actual": "<missing>",
                "fixture": fixture,
                "stage": item.get("stage", "unknown"),
                "path": str(artifact_dir / "matrix-current" / fixture / pattern),
            })
            all_ok = False
            continue

        actual_hash = _compute_sha256(actual_path)
        entry = {
            "id": artifact_id,
            "expected": expected_hash,
            "actual": actual_hash,
            "fixture": fixture,
            "stage": item.get("stage", "unknown"),
            "path": str(actual_path),
        }
        entries.append(entry)

        if not _validate_selected_artifact_contract(item, actual_path):
            all_ok = False

        if not expected_hash:
            print(f"[SHA256 GATE] {artifact_id}: FAIL — locked sha256 is empty")
            all_ok = False
            continue

        ok = actual_hash == expected_hash
        marker = "PASS" if ok else "FAIL"
        print(f"[SHA256 GATE] {artifact_id}: {marker}")
        if not ok:
            print(f"  expected: {expected_hash}")
            print(f"  actual:   {actual_hash}")
            all_ok = False

    return all_ok, entries


def _validate_selected_artifact_contract(item: dict[str, Any], path: Path) -> bool:
    artifact_id = item.get("id", "unknown")
    contract = item.get("contract")
    if not _validate_selected_artifact_manifest(item):
        return False
    assert isinstance(contract, dict)

    calculated_size = (
        contract["width"]
        * contract["height"]
        * contract["planes"]
        * contract["pixelSize"]
    )
    expected_size = contract["bufferSize"]
    actual_size = path.stat().st_size
    ok = calculated_size == expected_size and actual_size == expected_size
    marker = "PASS" if ok else "FAIL"
    print(
        f"[ARTIFACT CONTRACT] {artifact_id}: {marker} — "
        f"{contract['width']}x{contract['height']} planes={contract['planes']} "
        f"pixelType={contract['pixelType']} pixelSize={contract['pixelSize']} "
        f"bufferSize={actual_size} layout={contract['layout']} "
        f"opcodeScope={contract['opcodeScope']}"
    )
    if not ok:
        print(
            f"  expected bufferSize={expected_size} calculated={calculated_size} "
            f"actual={actual_size}"
        )
    return ok


def _write_baseline_candidate_diff(
    entries: list[dict[str, str]],
    output_path: Path,
) -> None:
    """Write baseline_candidate_diff.txt for --propose-baseline-update."""
    lines = [
        "# Baseline Candidate Diff",
        f"# Generated: {dt.datetime.now().astimezone().isoformat()}",
        "#",
        "# Review these changes and update kernel_regression_baselines.json manually.",
        "# Only user-approved changes should be committed.",
        "",
    ]
    changed = False
    for e in entries:
        status = "MATCH" if e["expected"] == e["actual"] else "CHANGED"
        if e["expected"] == "":
            status = "NEW (was uninitialized)"
        lines.append(f"[{status}] {e['id']}")
        if status != "MATCH":
            changed = True
            if e["expected"]:
                lines.append(f"  - old: {e['expected']}")
            lines.append(f"  + new: {e['actual']}")
        lines.append("")

    if not changed:
        lines.append("No changes detected — all hashes match manifest.")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"[INFO] Baseline candidate diff written: {output_path}")


def _clear_auto_diff_outputs(artifact_dir: Path) -> None:
    """Remove reports owned by this runner so a PASS run leaves no stale diff."""
    for pattern in ("diff_report_*.txt", "heatmap_*.png"):
        for path in artifact_dir.glob(pattern):
            path.unlink()


def _reset_artifact_dir(root: Path, artifact_dir: Path) -> None:
    """Clear ephemeral matrix artifacts before writing the current run."""
    artifact_dir = artifact_dir.resolve()
    default_artifact_dir = (root / "artifacts").resolve()
    try:
        artifact_dir.relative_to(root)
    except ValueError as exc:
        raise ValueError(f"refusing to clean artifact dir outside repo: {artifact_dir}") from exc
    if artifact_dir.name != "artifacts" and artifact_dir.parent != default_artifact_dir:
        raise ValueError(
            "refusing to clean artifact dir outside <repo>/artifacts or its direct children: "
            f"{artifact_dir}"
        )

    if artifact_dir.exists():
        for path in artifact_dir.iterdir():
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()
    artifact_dir.mkdir(parents=True, exist_ok=True)


def _clear_halide_case_outputs(artifact_dir: Path) -> None:
    """Avoid staging raw files emitted by an earlier Halide case."""
    for name in ("halide_demosaic_output.raw", "halide_render_output.raw"):
        path = artifact_dir / name
        if path.exists():
            path.unlink()


def _stage_halide_artifacts(
    artifact_dir: Path,
    matrix_current: Path,
    fixture: str,
) -> dict[str, Path]:
    """Stage one Halide repeat without retaining files from an earlier round."""
    fixture_staging = matrix_current / fixture
    if fixture_staging.exists():
        shutil.rmtree(fixture_staging)
    fixture_staging.mkdir(parents=True, exist_ok=True)

    case_artifact_map: dict[str, Path] = {}
    for artifact_name in ("halide_demosaic_output.raw", "halide_render_output.raw"):
        src = artifact_dir / artifact_name
        if src.exists():
            dst = fixture_staging / artifact_name
            shutil.copy2(str(src), str(dst))
            case_artifact_map[f"{fixture}/{artifact_name}"] = dst
    return case_artifact_map


def _psnr_raw_pair(
    artifact_dir: Path,
    matrix_current: Path,
    fixture: str,
    stage: str,
) -> tuple[Optional[Path], Optional[Path]]:
    """Return the same-run SDK reference and Halide candidate when available."""
    if stage == "stage3" and fixture == "lossless":
        reference = artifact_dir / "lossless_stage3.raw"
        candidate = matrix_current / fixture / "halide_demosaic_output.raw"
    elif stage == "stage4" and fixture in ("lossless", "lossy"):
        reference = artifact_dir / (
            "lossless_render.raw" if fixture == "lossless" else "lossy_render.raw"
        )
        candidate = matrix_current / fixture / "halide_render_output.raw"
    else:
        return None, None

    return reference if reference.exists() else None, candidate if candidate.exists() else None


def _run_psnr_gate(
    run: RunResult,
    thresholds: dict[tuple[str, str], float],
    artifact_dir: Path,
    matrix_current: Path,
    round_label: str,
) -> list[FailedGateInfo]:
    failed: list[FailedGateInfo] = []
    for (fragment, stage_attr), threshold in thresholds.items():
        if fragment not in run.case_name:
            continue
        stage_result = getattr(run, stage_attr)
        psnr = stage_result.psnr_db
        fixture = "lossless" if "Lossless" in run.case_name else "lossy"
        reference_raw, candidate_raw = _psnr_raw_pair(
            artifact_dir, matrix_current, fixture, stage_attr
        )
        if psnr is None:
            print(
                f"[MATRIX PSNR GATE] {round_label} {run.case_name} {stage_attr}: "
                "missing PSNR  [FAIL]"
            )
        else:
            ok = psnr >= threshold
            marker = "PASS" if ok else "FAIL"
            print(
                f"[MATRIX PSNR GATE] {round_label} {run.case_name} {stage_attr}: "
                f"{psnr:.2f} dB {'≥' if ok else '<'} {threshold} dB  [{marker}]"
            )
            if ok:
                continue
        failed.append(FailedGateInfo(
            gate_kind="psnr",
            case_name=f"{run.case_name} ({round_label})",
            stage=stage_attr,
            actual_psnr=psnr,
            threshold=threshold,
            raw_prefix=fixture,
            reference_raw=reference_raw,
            candidate_raw=candidate_raw,
        ))
    return failed


def _failed_sha_gate_infos(
    entries: list[dict[str, str]],
    round_label: str,
) -> list[FailedGateInfo]:
    return [
        FailedGateInfo(
            gate_kind="sha256",
            case_name=f"{entry['id']} ({round_label})",
            stage=entry["stage"],
            actual_psnr=None,
            threshold=None,
            raw_prefix=entry["fixture"],
            reference_raw=None,
            candidate_raw=Path(entry["path"]),
            expected_hash=entry["expected"],
            actual_hash=entry["actual"],
        )
        for entry in entries
        if entry["expected"] != entry["actual"]
    ]


def _stage_ffi_test_render(
    artifact_dir: Path,
    matrix_current: Path,
    fixture: str,
    dng_path: str,
) -> Path:
    source = matrix_current / fixture / "halide_render_output.raw"
    if not source.is_file():
        raise RuntimeError(f"[FFI {fixture}] Halide test render missing: {source}")

    reference = artifact_dir / (
        "lossless_render.raw" if fixture == "lossless" else "lossy_render.raw"
    )
    if not reference.is_file() or reference.stat().st_size != source.stat().st_size:
        raise RuntimeError(f"[FFI {fixture}] SDK render reference missing or size mismatch")

    destination = matrix_current / fixture / (
        "lossless_test_render.raw" if fixture == "lossless" else "lossy_test_render.raw"
    )
    shutil.copy2(source, destination)
    return destination.parent


def _comparison_scope(stage: str) -> str:
    if stage == "stage3":
        return "post OpcodeList3 Stage3 SDK baseline vs Halide Stage3 output"
    if stage == "stage4":
        return "final Stage4 rendered RGB; no pre/post OpcodeList3 artifact mixing"
    return "no same-run raw diagnostic pair for this stage"


def _raw_dimension_args(reference_raw: Path) -> list[str]:
    if reference_raw.name == "lossless_stage3.raw":
        return ["--width", "6048", "--height", "4024", "--planes", "3"]
    if reference_raw.name in ("lossless_render.raw", "lossy_render.raw"):
        return ["--width", "6000", "--height", "4000", "--planes", "3"]
    return []


def _auto_diff_on_failure(
    artifact_dir: Path,
    failed_cases: list[FailedGateInfo],
    script_dir: Path,
    baselines: dict[str, Any],
    root: Path,
) -> None:
    """Write gate diagnostics and run optional same-run raw comparisons."""
    for failed in failed_cases:
        report_path = artifact_dir / "diff_report.txt"
        heatmap_path = artifact_dir / "heatmap.png"
        fixture = baselines.get("fixtures", {}).get(failed.raw_prefix, {})
        fixture_path = root / fixture.get("path", "")
        fixture_expected = fixture.get("sha256") or "n/a"
        fixture_actual = (
            _compute_sha256(fixture_path) if fixture_path.is_file() else "unavailable"
        )
        candidate_actual = failed.actual_hash
        if candidate_actual is None and failed.candidate_raw and failed.candidate_raw.is_file():
            candidate_actual = _compute_sha256(failed.candidate_raw)

        lines = [
            "# Gate Failure Auto Diff",
            f"gate kind: {failed.gate_kind}",
            f"case: {failed.case_name}",
            f"stage: {failed.stage}",
            f"comparison scope: {_comparison_scope(failed.stage)}",
            f"actual PSNR: {failed.actual_psnr if failed.actual_psnr is not None else 'n/a'}",
            f"threshold: {failed.threshold if failed.threshold is not None else 'n/a'}",
            f"fixture path: {fixture_path if fixture else 'n/a'}",
            f"fixture expected SHA-256: {fixture_expected}",
            f"fixture actual SHA-256: {fixture_actual}",
            f"expected artifact SHA-256: {failed.expected_hash or 'n/a'}",
            f"actual artifact SHA-256: {candidate_actual or 'n/a'}",
            (
                f"reference raw: {failed.reference_raw} "
                f"(exists={bool(failed.reference_raw and failed.reference_raw.is_file())})"
            ),
            (
                f"candidate raw: {failed.candidate_raw} "
                f"(exists={bool(failed.candidate_raw and failed.candidate_raw.is_file())})"
            ),
        ]

        have_raw_pair = bool(
            failed.reference_raw
            and failed.reference_raw.is_file()
            and failed.candidate_raw
            and failed.candidate_raw.is_file()
        )
        if not have_raw_pair:
            lines.append("contract / PSNR diagnostic: SKIP (same-run raw pair unavailable)")
            lines.append("heatmap: SKIP (same-run raw pair unavailable)")
        else:
            compare_cmd = [
                sys.executable,
                str(script_dir / "compare_psnr.py"),
                str(failed.reference_raw),
                str(failed.candidate_raw),
                "--min-psnr",
                "0",
                *_raw_dimension_args(failed.reference_raw),
            ]
            compare = subprocess.run(
                compare_cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
            )
            lines.extend([
                "",
                "## compare_psnr.py",
                f"exit code: {compare.returncode}",
                compare.stdout.rstrip(),
            ])

            if compare.returncode != 0:
                lines.append("heatmap: SKIP (compare_psnr contract / diagnostic failed)")
            else:
                heatmap_cmd = [
                    sys.executable,
                    str(script_dir / "heatmap_diff.py"),
                    str(failed.reference_raw),
                    str(failed.candidate_raw),
                    "--output",
                    str(heatmap_path),
                    "--plane",
                    "-1",
                    *_raw_dimension_args(failed.reference_raw),
                ]
                heatmap = subprocess.run(
                    heatmap_cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    check=False,
                )
                lines.extend([
                    "",
                    "## heatmap_diff.py",
                    f"exit code: {heatmap.returncode}",
                    heatmap.stdout.rstrip(),
                ])
                if heatmap.returncode == 0:
                    lines.append(f"heatmap: {heatmap_path}")
                else:
                    lines.append("heatmap: SKIP (optional dependency or heatmap command failed)")

        report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"[AUTO DIFF] report: {report_path}")


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
    # D-A (2026-07-05): standalone-kernel fallback coverage. DNG_FUSED_DEMOSAIC_WARP=0
    # forces dng_pipeline_v2.cpp's Stage3 host path to skip the fused
    # demosaic+WarpRectilinear dispatch and fall through to the two standalone
    # AOT kernels (demosaic_bilinear_halide_aot + rectilinear_warp via
    # applyOpcodeList3) -- a real production branch taken for any DNG shape
    # whose OpcodeList3 isn't exactly one WarpRectilinear opcode, previously
    # untested (see docs/logs/2026-07-05/Task_r4_c1_generator_verdict.md §1).
    # Same cmd as "Lossless / Halide Metal": the fixture already satisfies the
    # fused precondition, so only the env flip changes which kernels run.
    halide_env_lossless_standalone_fallback = {
        **extra_env, **timing_env, "DNG_FUSED_DEMOSAIC_WARP": "0",
    }

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
            "Lossless / Halide Metal (standalone fallback)",
            [test_decode, lossless, "test", "halide-metal", "halide-metal"],
            halide_env_lossless_standalone_fallback,
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
        help=(
            "Production C ABI harness path (relative to repo-root). Gates "
            "contract and RGB exact match. Auto-enabled when the default "
            f"build output exists ({_DEFAULT_FFI_HARNESS}); pass --no-ffi-harness "
            "to skip explicitly."
        ),
    )
    ap.add_argument(
        "--no-ffi-harness",
        action="store_true",
        default=False,
        help="Disable the FFI harness case even if the default binary exists.",
    )
    ap.add_argument(
        "--device-handoff-harness",
        default="",
        help=(
            "Device handoff harness path (relative to repo-root). Gates both "
            "handoff routes. Auto-enabled when the default build output exists "
            f"({_DEFAULT_DEVICE_HANDOFF_HARNESS}); pass --no-device-handoff-harness "
            "to skip explicitly."
        ),
    )
    ap.add_argument(
        "--no-device-handoff-harness",
        action="store_true",
        default=False,
        help="Disable the device handoff harness case even if the default binary exists.",
    )
    ap.add_argument("--output", default="", help="Optional Markdown output file path")
    ap.add_argument(
        "--artifact-dir",
        default="",
        help=(
            "Directory for test_decode intermediate raw outputs. "
            "Defaults to <repo_root>/artifacts."
        ),
    )
    ap.add_argument(
        "--env",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="Extra env var override, repeatable (applies to all cases)",
    )
    ap.add_argument(
        "--regression-baselines",
        default="",
        help=(
            "Path to kernel_regression_baselines.json. "
            "Default: <script_dir>/kernel_regression_baselines.json"
        ),
    )
    ap.add_argument(
        "--propose-baseline-update",
        action="store_true",
        default=False,
        help=(
            "After matrix completes, write artifacts/baseline_candidate_diff.txt "
            "with current hashes vs manifest. Does NOT modify the manifest."
        ),
    )
    # --- Android device testing ---
    ap.add_argument(
        "--android-test-decode",
        default="",
        help=(
            "Android test_decode binary path (relative to repo-root). "
            f"Default: {_DEFAULT_ANDROID_TEST_DECODE}"
        ),
    )
    ap.add_argument(
        "--android-serial",
        default=os.environ.get("ADB_SERIAL"),
        help="ADB device serial; auto-detected when exactly one device is attached.",
    )
    ap.add_argument(
        "--adb",
        default=shutil.which("adb") or "",
        help="adb executable path (default: auto-detect from PATH).",
    )
    ap.add_argument(
        "--android-remote-dir",
        default="/data/local/tmp/dng_matrix",
        help="Remote working directory on the Android device.",
    )
    ap.add_argument(
        "--android-ndk",
        default=os.environ.get("ANDROID_NDK_HOME"),
        help="Android NDK root (needed to locate libc++_shared.so for push).",
    )
    ap.add_argument(
        "--android-abi",
        default="arm64-v8a",
        help="Android ABI for locating NDK shared libs.",
    )
    ap.add_argument(
        "--platform",
        choices=("all", "macos", "android"),
        default="all",
        help="Which platform(s) to test: all (default), macos, or android.",
    )
    ap.add_argument(
        "--android-cooldown-sec",
        type=int,
        default=5,
        help="Seconds to wait between Android repeats to avoid thermal throttling.",
    )
    ap.add_argument(
        "--android-include-lossy",
        action="store_true",
        default=False,
        help=(
            "Also run the Android lossy DNG case. Disabled by default because "
            "the current Android DNG SDK build does not enable JPEG decode."
        ),
    )
    ap.add_argument(
        "--android-ffi-harness",
        default="",
        help=(
            "Android FFI harness binary path (relative to repo-root). Runs "
            "the production C ABI entry on-device (repeat_count passed "
            "directly to the harness, single process). Gates [Contract] + "
            "pool leak-check only (no host RGB reference on-device). "
            "Auto-enabled when the default build output exists "
            f"({_DEFAULT_ANDROID_FFI_HARNESS}); pass --no-android-ffi-harness "
            "to skip explicitly."
        ),
    )
    ap.add_argument(
        "--no-android-ffi-harness",
        action="store_true",
        default=False,
        help="Disable the Android FFI harness case even if the default binary exists.",
    )
    ap.add_argument(
        "--android-device-handoff-harness",
        default="",
        help=(
            "Android device-handoff harness binary path (relative to "
            "repo-root). Auto-enabled when the default build output exists "
            f"({_DEFAULT_ANDROID_DEVICE_HANDOFF_HARNESS}); pass "
            "--no-android-device-handoff-harness to skip explicitly. As of "
            "2026-07-04 no CMake target cross-compiles this binary yet "
            "(request pending with build-eng), so this case is normally "
            "skipped with an [INFO] note."
        ),
    )
    ap.add_argument(
        "--no-android-device-handoff-harness",
        action="store_true",
        default=False,
        help="Disable the Android device handoff harness case even if the default binary exists.",
    )
    args = ap.parse_args()

    if args.repeat < 1:
        ap.error("--repeat must be >= 1")

    root = Path(args.repo_root).resolve()

    # --- Platform flags ---
    requested_android_test_decode = args.android_test_decode
    if not args.android_test_decode:
        args.android_test_decode = _DEFAULT_ANDROID_TEST_DECODE
    default_android_bin = (root / args.android_test_decode).resolve()
    android_enabled = args.platform == "android" or (
        args.platform == "all" and default_android_bin.exists()
    )
    macos_enabled = args.platform in ("all", "macos")
    if args.platform == "all" and not android_enabled and not requested_android_test_decode:
        print(f"[INFO] Android binary not found; skipping Android: {default_android_bin}")

    # `--platform all` (default) is a best-effort "test whatever is
    # available" mode: if the Android binary exists but no ADB device is
    # attached/authorized (or adb itself is missing), skip Android cases
    # gracefully instead of hard-failing the whole matrix run. An explicit
    # `--platform android` request is unaffected and still hard-fails below
    # (via _resolve_serial / ap.error) when no device is available.
    if args.platform == "all" and android_enabled and not _adb_device_attached(args.adb):
        print("[INFO] no ADB device attached; skipping Android cases")
        android_enabled = False

    # Validate macOS binary only when macOS testing is enabled
    bin_path = None
    if macos_enabled:
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

    cases: list[tuple[str, list[str], dict[str, str]]] = []
    if macos_enabled:
        cases = _build_cases(
            str(bin_path), lossless, lossy, extra_env, enable_timing=args.timing
        )

    if args.artifact_dir:
        artifact_dir = Path(args.artifact_dir)
        if not artifact_dir.is_absolute():
            artifact_dir = (root / artifact_dir).resolve()
    else:
        artifact_dir = root / "artifacts"
    try:
        _reset_artifact_dir(root, artifact_dir)
    except ValueError as exc:
        ap.error(str(exc))
    print(f"[INFO] Intermediate artifacts: {artifact_dir}")

    # --- Load regression baselines ---
    script_dir = Path(__file__).resolve().parent
    baselines_path = (
        Path(args.regression_baselines)
        if args.regression_baselines
        else script_dir / "kernel_regression_baselines.json"
    )
    if not baselines_path.is_absolute():
        baselines_path = (root / baselines_path).resolve()
    try:
        baselines = _load_regression_baselines(baselines_path)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[REGRESSION BASELINES] FAIL — {exc}")
        return 1
    if not _validate_regression_baselines_schema(baselines):
        return 1

    # --- Verify fixture hashes ---
    if not _verify_fixture_hashes(baselines, root):
        print("[FIXTURE HASH] FAIL — fixture DNG files do not match manifest")
        return 1

    # --- Clean matrix-current/ to avoid stale artifacts ---
    matrix_current = artifact_dir / "matrix-current"
    if matrix_current.exists():
        shutil.rmtree(matrix_current)
    matrix_current.mkdir(parents=True, exist_ok=True)
    print(f"[INFO] Clean artifact staging: {matrix_current}")

    # Map case_name fragments to fixture keys for artifact staging
    _CASE_FIXTURE_MAP: dict[str, str] = {
        "Lossless / Halide": "lossless",
        "Lossy / Halide": "lossy",
    }

    psnr_thresholds = _build_psnr_thresholds_from_baselines(baselines)
    gate_failed = False
    sha_ok = True
    sha_entries_by_id: dict[str, dict[str, str]] = {}

    agg_results: list[AggResult] = []
    if macos_enabled:
        for case_name, cmd, env in cases:
            runs: list[RunResult] = []
            fixture_key = next(
                (fixture for fragment, fixture in _CASE_FIXTURE_MAP.items() if fragment in case_name),
                None,
            )
            for i in range(args.repeat):
                round_label = f"[RUN {i+1}/{args.repeat}]"
                round_sha_failures: list[FailedGateInfo] = []
                print(f"{round_label} {case_name}")
                try:
                    if "Halide" in case_name:
                        _clear_halide_case_outputs(artifact_dir)
                    run = _run_case(artifact_dir, cmd, case_name, env)
                    runs.append(run)
                except RuntimeError as exc:
                    print(f"  ERROR: {exc}")
                    raise SystemExit(1)

                if fixture_key is not None:
                    case_artifact_map = _stage_halide_artifacts(
                        artifact_dir, matrix_current, fixture_key
                    )
                    selected = [
                        item for item in baselines["selected_artifacts"]
                        if item.get("fixture") == fixture_key
                    ]
                    print(f"[SHA256 GATE] {round_label} {case_name}")
                    round_sha_ok, round_sha_entries = _run_sha256_gate(
                        baselines,
                        artifact_dir,
                        case_artifact_map,
                        selected_artifacts=selected,
                    )
                    sha_ok = round_sha_ok and sha_ok
                    for entry in round_sha_entries:
                        sha_entries_by_id[entry["id"]] = entry
                    if not round_sha_ok:
                        round_sha_failures = _failed_sha_gate_infos(
                            round_sha_entries, round_label
                        )

                round_psnr_failures = _run_psnr_gate(
                    run,
                    psnr_thresholds,
                    artifact_dir,
                    matrix_current,
                    round_label,
                )
                gate_failed = bool(round_psnr_failures) or gate_failed
                if round_psnr_failures or round_sha_failures:
                    _auto_diff_on_failure(
                        artifact_dir,
                        [*round_psnr_failures, *round_sha_failures],
                        script_dir,
                        baselines,
                        root,
                    )

            agg_results.append(AggResult(case_name=case_name, runs=runs))

    # --- Android test execution ---
    android_agg_results: list[AggResult] = []
    android_ffi_agg_results: list[FfiAggResult] = []
    handoff_results: list[DeviceHandoffResult] = []
    android_serial: Optional[str] = None
    if android_enabled:
        android_bin = (root / args.android_test_decode).resolve()
        if not android_bin.exists():
            ap.error(f"Android test_decode binary not found: {android_bin}")

        adb = args.adb
        if not adb:
            ap.error("adb not found in PATH; use --adb to specify")

        if not args.android_remote_dir.startswith("/data/local/tmp/"):
            ap.error("--android-remote-dir must stay under /data/local/tmp/")

        try:
            android_serial = _resolve_serial(adb, args.android_serial)
        except RuntimeError as exc:
            ap.error(str(exc))
        print(f"[INFO] Android device: {android_serial}")

        # Locate optional shared libs. Some Android test_decode builds are
        # fully satisfied by system libraries; others need sidecar .so files.
        shared_libs: list[Path] = []
        seen_libs: set[Path] = set()
        for candidate in sorted(android_bin.parent.glob("*.so")):
            resolved = candidate.resolve()
            if resolved not in seen_libs:
                shared_libs.append(candidate)
                seen_libs.add(resolved)

        if args.android_ndk:
            ndk_path = Path(args.android_ndk).expanduser().resolve()
            libcxx = _libcxx_path(ndk_path, args.android_abi)
            if not libcxx or not libcxx.exists():
                ap.error(f"libc++_shared.so not found in NDK for ABI {args.android_abi}")
            resolved = libcxx.resolve()
            if resolved not in seen_libs:
                shared_libs.append(libcxx)
                seen_libs.add(resolved)

        # Probe binary (optional but recommended)
        probe_bin = android_bin.parent / "test_android_vulkan_capability"

        # Push files to device
        print("[INFO] Pushing binaries and samples to Android device...")
        try:
            _adb_stage_binaries(
                adb, android_serial, android_bin,
                probe_bin if probe_bin.exists() else None,
                shared_libs,
                Path(lossless), Path(lossy),
                args.android_remote_dir,
            )
        except subprocess.CalledProcessError as exc:
            print(f"  ERROR: ADB staging failed: {exc}")
            raise SystemExit(1)

        # Vulkan capability check
        if probe_bin.exists():
            print("[INFO] Running Vulkan capability probe...")
            try:
                _adb_run_vulkan_probe(adb, android_serial, args.android_remote_dir)
            except RuntimeError as exc:
                print(f"  ERROR: {exc}")
                raise SystemExit(1)

        # Build and run Android cases
        remote_lossless = f"{args.android_remote_dir}/samples/{Path(lossless).name}"
        remote_lossy = f"{args.android_remote_dir}/samples/{Path(lossy).name}"
        remote_artifacts = f"{args.android_remote_dir}/artifacts"
        android_cases = _build_android_cases(
            remote_lossless, remote_lossy, remote_artifacts,
            extra_env,
            enable_timing=args.timing,
            include_lossy=args.android_include_lossy,
        )
        if not args.android_include_lossy:
            print("[INFO] Android lossy DNG case skipped (JPEG decode is disabled in current Android SDK build)")

        for case_name, baseline_args, test_args, baseline_env, test_env in android_cases:
            android_runs: list[RunResult] = []
            for i in range(args.repeat):
                round_label = f"[RUN {i+1}/{args.repeat}]"
                print(f"{round_label} {case_name}")
                try:
                    run = _run_android_case(
                        adb, android_serial, args.android_remote_dir,
                        baseline_args, test_args, case_name, baseline_env, test_env,
                        local_bin=android_bin,
                    )
                    android_runs.append(run)
                except RuntimeError as exc:
                    print(f"  ERROR: {exc}")
                    raise SystemExit(1)

                # Cooldown between repeats (except after last)
                if i < args.repeat - 1 and args.android_cooldown_sec > 0:
                    print(f"  Cooldown: {args.android_cooldown_sec}s...")
                    time.sleep(args.android_cooldown_sec)

            android_agg_results.append(AggResult(case_name=case_name, runs=android_runs))

        # --- Android FFI harness (auto-enable pattern mirrors macOS FFI) ---
        requested_android_ffi_harness = args.android_ffi_harness
        if not args.no_android_ffi_harness and not args.android_ffi_harness:
            args.android_ffi_harness = _DEFAULT_ANDROID_FFI_HARNESS
        if args.no_android_ffi_harness:
            args.android_ffi_harness = ""
        default_android_ffi_bin = (
            (root / args.android_ffi_harness).resolve() if args.android_ffi_harness else None
        )
        if (
            args.android_ffi_harness
            and not requested_android_ffi_harness
            and default_android_ffi_bin is not None
            and not default_android_ffi_bin.exists()
        ):
            print(f"[INFO] Android FFI harness binary not found; skipping: {default_android_ffi_bin}")
            args.android_ffi_harness = ""

        if args.android_ffi_harness:
            android_ffi_bin = (root / args.android_ffi_harness).resolve()
            if not android_ffi_bin.exists():
                ap.error(f"Android FFI harness binary not found: {android_ffi_bin}")
            print("[INFO] Pushing Android FFI harness binary...")
            remote_ffi_bin = _adb_push_extra_binary(
                adb, android_serial, android_ffi_bin, args.android_remote_dir
            )
            android_ffi_env: dict[str, str] = {**extra_env}
            if args.timing:
                android_ffi_env.update({
                    "DNG_STAGE1_TIMING": "1",
                    "DNG_MAP_POLY_TIMING": "1",
                    "DNG_STAGE2_SDK_TIMING": "1",
                })
            android_ffi_cases = [("Lossless / FFI (Android)", remote_lossless)]
            if args.android_include_lossy:
                android_ffi_cases.append(("Lossy / FFI (Android)", remote_lossy))
            for sample_name, remote_dng in android_ffi_cases:
                print(f"[Android FFI] {sample_name} (repeat={args.repeat}, single process)")
                try:
                    ffi_runs = _run_android_ffi_case(
                        adb, android_serial, args.android_remote_dir, remote_ffi_bin,
                        sample_name, remote_dng, android_ffi_env, args.repeat,
                        local_bin=android_ffi_bin,
                    )
                except RuntimeError as exc:
                    print(f"  ERROR: {exc}")
                    raise SystemExit(1)
                android_ffi_agg_results.append(FfiAggResult(sample_name=sample_name, runs=ffi_runs))

        # --- Android device handoff harness (auto-enable; no CMake target
        # exists yet as of 2026-07-04 so this normally [INFO]-skips) ---
        requested_android_handoff_harness = args.android_device_handoff_harness
        if not args.no_android_device_handoff_harness and not args.android_device_handoff_harness:
            args.android_device_handoff_harness = _DEFAULT_ANDROID_DEVICE_HANDOFF_HARNESS
        if args.no_android_device_handoff_harness:
            args.android_device_handoff_harness = ""
        default_android_handoff_bin = (
            (root / args.android_device_handoff_harness).resolve()
            if args.android_device_handoff_harness else None
        )
        if (
            args.android_device_handoff_harness
            and not requested_android_handoff_harness
            and default_android_handoff_bin is not None
            and not default_android_handoff_bin.exists()
        ):
            print(
                "[INFO] Android device handoff harness binary not found "
                f"(no CMake cross-compile target yet); skipping: {default_android_handoff_bin}"
            )
            args.android_device_handoff_harness = ""

        if args.android_device_handoff_harness:
            android_handoff_bin = (root / args.android_device_handoff_harness).resolve()
            if not android_handoff_bin.exists():
                ap.error(f"Android device handoff harness not found: {android_handoff_bin}")
            print("[INFO] Pushing Android device handoff harness binary...")
            remote_handoff_bin = _adb_push_extra_binary(
                adb, android_serial, android_handoff_bin, args.android_remote_dir
            )
            try:
                android_handoff_results = _run_android_device_handoff(
                    adb, android_serial, args.android_remote_dir, remote_handoff_bin,
                    remote_lossless, remote_lossy, extra_env,
                    local_bin=android_handoff_bin,
                )
                handoff_results.extend(android_handoff_results)
            except RuntimeError as exc:
                print(f"  ERROR: {exc}")
                raise SystemExit(1)

    # --- Harness auto-enable (same pattern as Android: default path, skip
    # silently with an [INFO] note when the binary hasn't been built yet) ---
    requested_device_handoff_harness = args.device_handoff_harness
    if not args.no_device_handoff_harness and not args.device_handoff_harness:
        args.device_handoff_harness = _DEFAULT_DEVICE_HANDOFF_HARNESS
    if args.no_device_handoff_harness:
        args.device_handoff_harness = ""
    default_handoff_bin = (root / args.device_handoff_harness).resolve() if args.device_handoff_harness else None
    if (
        macos_enabled
        and args.device_handoff_harness
        and not requested_device_handoff_harness
        and default_handoff_bin is not None
        and not default_handoff_bin.exists()
    ):
        print(f"[INFO] Device handoff harness binary not found; skipping: {default_handoff_bin}")
        args.device_handoff_harness = ""

    requested_ffi_harness = args.ffi_harness
    if not args.no_ffi_harness and not args.ffi_harness:
        args.ffi_harness = _DEFAULT_FFI_HARNESS
    if args.no_ffi_harness:
        args.ffi_harness = ""
    default_ffi_bin = (root / args.ffi_harness).resolve() if args.ffi_harness else None
    if (
        macos_enabled
        and args.ffi_harness
        and not requested_ffi_harness
        and default_ffi_bin is not None
        and not default_ffi_bin.exists()
    ):
        print(f"[INFO] FFI harness binary not found; skipping: {default_ffi_bin}")
        args.ffi_harness = ""

    # handoff_results is initialized above (before the Android block) so
    # Android device-handoff results collected there survive into this
    # macOS section; here we only append the macOS results.
    if macos_enabled and args.device_handoff_harness:
        handoff_harness = (root / args.device_handoff_harness).resolve()
        if not handoff_harness.exists():
            ap.error(f"Device handoff harness not found: {handoff_harness}")
        try:
            handoff_results.extend(_run_device_handoff(
                root, str(handoff_harness), lossless, lossy, extra_env
            ))
        except RuntimeError as exc:
            print(f"  ERROR: {exc}")
            raise SystemExit(1)

    ffi_results: list[FfiAggResult] = []
    if macos_enabled and args.ffi_harness:
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
            ("Lossless / FFI", lossless, "lossless"),
            ("Lossy / FFI", lossy, "lossy"),
        ]
        for sample_name, dng_path, fixture in ffi_cases:
            runs: list[FfiRunResult] = []
            try:
                ffi_artifact_dir = _stage_ffi_test_render(
                    artifact_dir, matrix_current, fixture, dng_path
                )
            except RuntimeError as exc:
                print(f"  ERROR: {exc}")
                raise SystemExit(1)
            for i in range(args.repeat):
                print(f"[FFI {i+1}/{args.repeat}] {sample_name}")
                try:
                    runs.append(_run_ffi_case(root, str(ffi_harness), sample_name,
                                              dng_path, ffi_env, ffi_artifact_dir))
                except RuntimeError as exc:
                    print(f"  ERROR: {exc}")
                    raise SystemExit(1)
            ffi_results.append(FfiAggResult(sample_name=sample_name, runs=runs))

    generated_at = dt.datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")
    md = _build_markdown(
        agg_results,
        ffi_results,
        handoff_results,
        android_agg_results,
        android_ffi_agg_results,
        generated_at=generated_at,
        repeat=args.repeat,
        show_halide_timing=args.timing,
        artifact_dir=artifact_dir,
        android_serial=android_serial,
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

    sha_entries = list(sha_entries_by_id.values())
    if args.propose_baseline_update and sha_entries:
        diff_path = artifact_dir / "baseline_candidate_diff.txt"
        _write_baseline_candidate_diff(sha_entries, diff_path)

    if gate_failed or not sha_ok:
        if gate_failed:
            print("[MATRIX PSNR GATE] FAIL — one or more cases below threshold")
        if not sha_ok:
            print("[SHA256 GATE] FAIL — one or more artifacts do not match manifest")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
