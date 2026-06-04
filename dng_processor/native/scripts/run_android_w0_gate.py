#!/usr/bin/env python3
import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional


EXPECTED_STAGES = ("Stage1", "Stage2", "Stage3", "Stage4")
CONTRACT_RE = re.compile(r"\[Contract\]\s+(Stage[1-4])\b.*->\s*(PASS|FAIL)")


def fail(message: str) -> int:
    print(f"[ERROR] {message}", file=sys.stderr)
    return 1


def run(cmd: list[str], cwd: Optional[Path] = None, check: bool = True) -> subprocess.CompletedProcess[str]:
    print(f"[CMD] {' '.join(shlex.quote(part) for part in cmd)}")
    proc = subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
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


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def parse_attached_devices(adb: str) -> tuple[list[str], str]:
    proc = run([adb, "devices"], check=True)
    devices: list[str] = []
    for line in proc.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            devices.append(parts[0])
    return devices, proc.stdout


def resolve_serial(adb: str, requested: Optional[str]) -> str:
    if requested:
        proc = run([adb, "-s", requested, "get-state"], check=False)
        if proc.returncode != 0 or proc.stdout.strip() != "device":
            raise RuntimeError(f"ADB device is not ready: {requested}")
        return requested

    devices, raw = parse_attached_devices(adb)
    if not devices:
        raise RuntimeError(f"No attached ADB device in 'device' state.\n{raw}")
    if len(devices) > 1:
        raise RuntimeError(
            "Multiple attached ADB devices; pass --serial. "
            f"Devices: {', '.join(devices)}"
        )
    return devices[0]


def adb_cmd(adb: str, serial: str, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return run([adb, "-s", serial, *args], check=check)


def require_ndk(ndk: Optional[str]) -> Path:
    if not ndk:
        raise RuntimeError("ANDROID_NDK_HOME is not set; pass --android-ndk /path/to/ndk")
    ndk_path = Path(ndk).expanduser().resolve()
    toolchain = ndk_path / "build" / "cmake" / "android.toolchain.cmake"
    if not toolchain.exists():
        raise RuntimeError(f"Android NDK toolchain not found: {toolchain}")
    return ndk_path


def libcxx_path(ndk_path: Optional[Path], abi: str) -> Optional[Path]:
    if ndk_path is None:
        return None
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


def validate_contract_output(output: str) -> tuple[bool, str]:
    statuses: dict[str, list[str]] = {}
    for line in output.splitlines():
        match = CONTRACT_RE.search(line)
        if match:
            statuses.setdefault(match.group(1), []).append(match.group(2))

    missing = [stage for stage in EXPECTED_STAGES if "PASS" not in statuses.get(stage, [])]
    failed = [stage for stage, values in statuses.items() if "FAIL" in values]
    if missing or failed:
        details = []
        if missing:
            details.append(f"missing PASS markers: {', '.join(missing)}")
        if failed:
            details.append(f"FAIL markers: {', '.join(sorted(failed))}")
        return False, "; ".join(details)
    if "GPU backend: vulkan" not in output:
        return False, "missing 'GPU backend: vulkan' marker"
    if "[PSNR GATE] SKIP contract-only mode" not in output:
        return False, "missing contract-only PSNR skip marker"
    return True, "PASS"


def main() -> int:
    default_root = repo_root_from_script()
    parser = argparse.ArgumentParser(
        description="Build and run the Phase 14 W0 Android Vulkan ADB contract gate."
    )
    parser.add_argument("--repo-root", default=str(default_root), help="Repository root")
    parser.add_argument("--native-dir", default="", help="Native dir; default: <repo>/dng_processor/native")
    parser.add_argument("--android-ndk", default=os.environ.get("ANDROID_NDK_HOME"), help="Android NDK root")
    parser.add_argument("--android-abi", default="arm64-v8a", help="Android ABI")
    parser.add_argument("--android-platform", default="android-24", help="Android platform")
    parser.add_argument("--adb", default=shutil.which("adb") or "", help="adb executable")
    parser.add_argument("--serial", default=os.environ.get("ADB_SERIAL"), help="ADB serial; required when multiple devices are attached")
    parser.add_argument("--sample", default="image_samples/lossless_dng_sample.dng", help="DNG sample path")
    parser.add_argument("--remote-dir", default="/data/local/tmp/dng_w0_gate", help="Remote work directory")
    parser.add_argument("--idle-timeout-sec", default="120", help="Build watchdog idle timeout")
    parser.add_argument("--skip-build", action="store_true", help="Use existing build-android/android-arm64/test_decode_android")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).expanduser().resolve()
    native_dir = Path(args.native_dir).expanduser().resolve() if args.native_dir else repo_root / "dng_processor" / "native"
    sample = Path(args.sample)
    if not sample.is_absolute():
        sample = repo_root / sample

    if not repo_root.exists():
        return fail(f"Repository root not found: {repo_root}")
    if not native_dir.exists():
        return fail(f"Native directory not found: {native_dir}")
    if not sample.exists():
        return fail(f"DNG sample not found: {sample}")
    if not args.adb:
        return fail("adb not found in PATH; pass --adb /path/to/adb")
    if not args.remote_dir.startswith("/data/local/tmp/"):
        return fail("--remote-dir must stay under /data/local/tmp/")

    ndk_path: Optional[Path] = None
    try:
        serial = resolve_serial(args.adb, args.serial)
        if not args.skip_build:
            ndk_path = require_ndk(args.android_ndk)
    except RuntimeError as exc:
        return fail(str(exc))

    if args.skip_build and args.android_ndk:
        try:
            ndk_path = require_ndk(args.android_ndk)
        except RuntimeError:
            ndk_path = None

    build_script = native_dir / "scripts" / "build_native_watchdog.py"
    cross_dir = native_dir / "build-android" / "android-arm64"
    binary = cross_dir / "test_decode_android"
    probe_binary = cross_dir / "test_android_vulkan_capability"

    if not args.skip_build:
        try:
            run(
                [
                    sys.executable,
                    str(build_script),
                    "--platform",
                    "android",
                    "--target",
                    "test_decode_android",
                    "--android-ndk",
                    str(ndk_path),
                    "--android-abi",
                    args.android_abi,
                    "--android-platform",
                    args.android_platform,
                    "--idle-timeout-sec",
                    args.idle_timeout_sec,
                ],
                cwd=repo_root,
            )
        except subprocess.CalledProcessError as exc:
            return fail(f"Android native build failed with exit {exc.returncode}")

    if not binary.exists():
        return fail(f"Android test binary not found: {binary}")

    remote_dir = args.remote_dir.rstrip("/")
    remote_artifacts = f"{remote_dir}/artifacts"
    remote_samples = f"{remote_dir}/samples"
    remote_bin = f"{remote_dir}/test_decode_android"
    remote_probe = f"{remote_dir}/test_android_vulkan_capability"
    remote_sample = f"{remote_samples}/{sample.name}"

    shared_libs: list[Path] = []
    for candidate in (
        cross_dir / "libdng_decoder_native.so",
        cross_dir / "libc++_shared.so",
        libcxx_path(ndk_path, args.android_abi),
    ):
        if candidate and candidate.exists() and candidate.resolve() not in [p.resolve() for p in shared_libs]:
            shared_libs.append(candidate)

    try:
        adb_cmd(args.adb, serial, "shell", f"rm -rf {shlex.quote(remote_dir)} && mkdir -p {shlex.quote(remote_artifacts)} {shlex.quote(remote_samples)}")
        adb_cmd(args.adb, serial, "push", str(binary), remote_bin)
        adb_cmd(args.adb, serial, "shell", f"chmod 755 {shlex.quote(remote_bin)}")
        if probe_binary.exists():
            adb_cmd(args.adb, serial, "push", str(probe_binary), remote_probe)
            adb_cmd(args.adb, serial, "shell", f"chmod 755 {shlex.quote(remote_probe)}")
        adb_cmd(args.adb, serial, "push", str(sample), remote_sample)
        for lib in shared_libs:
            adb_cmd(args.adb, serial, "push", str(lib), f"{remote_dir}/{lib.name}")

        if probe_binary.exists():
            probe_cmd = (
                f"cd {shlex.quote(remote_artifacts)} && "
                f"export LD_LIBRARY_PATH={shlex.quote(remote_dir)}:$LD_LIBRARY_PATH && "
                f"{shlex.quote(remote_probe)}"
            )
            probe_proc = adb_cmd(args.adb, serial, "shell", probe_cmd, check=False)
            if probe_proc.returncode != 0:
                return fail(
                    "Android Vulkan capability probe failed; "
                    "device cannot run the configured Halide Vulkan AOT target"
                )

        shell_cmd = (
            f"cd {shlex.quote(remote_artifacts)} && "
            f"export LD_LIBRARY_PATH={shlex.quote(remote_dir)}:$LD_LIBRARY_PATH && "
            "export DNG_GPU_BACKEND=vulkan && "
            "export DNG_TEST_DECODE_CONTRACT_ONLY=1 && "
            f"{shlex.quote(remote_bin)} {shlex.quote(remote_sample)} test halide-gpu halide-gpu"
        )
        proc = adb_cmd(args.adb, serial, "shell", shell_cmd, check=False)
    except subprocess.CalledProcessError as exc:
        return fail(f"ADB staging failed with exit {exc.returncode}")

    if proc.returncode != 0:
        return fail(f"test_decode_android failed on device {serial} with exit {proc.returncode}")

    ok, details = validate_contract_output(proc.stdout)
    if not ok:
        return fail(f"Android W0 contract failed: {details}")

    print("[ANDROID W0 GATE] PASS")
    print(f"  device: {serial}")
    print("  target: test_decode_android")
    print(f"  sample: {sample}")
    print(f"  remote_artifacts: {remote_artifacts}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
