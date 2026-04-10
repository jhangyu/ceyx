#!/usr/bin/env python3
import argparse
from collections import deque
import os
import select
import subprocess
import sys
import time
from pathlib import Path


def print_dependency_hints(native_dir: Path, build_dir: Path, last_step: str) -> None:
    cmake_file = native_dir / "CMakeLists.txt"
    print(f"[HINT] Check: {cmake_file}", file=sys.stderr)

    step_map = [
        ("Rectilinear Warp", "dng_warp_aot_target"),
        ("Stage4 Render", "dng_render_aot_target"),
        ("Stage4 No-Map Render", "dng_render_nomap_aot_target"),
        ("Stage4 Maps-NoEncoding Render", "dng_render_maps_noencode_aot_target"),
        ("Stage4 Tail Render", "dng_render_tail_aot_target"),
        ("Stage4 Tone+Tail Render", "dng_render_tonetail_aot_target"),
    ]
    for marker, target in step_map:
        if marker in last_step:
            build_make = build_dir / "CMakeFiles" / f"{target}.dir" / "build.make"
            print(f"[HINT] Check generated rule: {build_make}", file=sys.stderr)
            return


def print_known_issue_hints(lines: list[str], suspected_step: str) -> None:
    joined = "\n".join(lines)

    if "Generating Halide AOT Rectilinear Warp" in suspected_step:
        print(
            "[HINT] Warp AOT is the current long-running step. If this repeats often, "
            "check DngWarpGenerator schedule complexity (tile/unroll/vectorize) to reduce codegen load.",
            file=sys.stderr,
        )

    if "Killed: 9" in joined:
        print(
            "[HINT] Build subprocess was killed (signal 9). This is often manual kill by user.",
            file=sys.stderr,
        )

    if "Undefined symbols for architecture arm64" in joined and "_halide_" in joined:
        print(
            "[HINT] Halide runtime symbols are missing at link time. Check AOT target/runtime pairing: "
            "rectilinear_warp should usually use target=${AOT_TARGET} (not -no_runtime), "
            "while stage4 kernels may use -no_runtime when Halide runtime is linked separately.",
            file=sys.stderr,
        )


def run_with_watchdog(cmd: list[str], cwd: Path, idle_timeout_sec: int, native_dir: Path, build_dir: Path) -> int:
    print(f"[CMD] {' '.join(cmd)}")
    print(f"[CWD] {cwd}")

    proc = subprocess.Popen(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    assert proc.stdout is not None
    last_output_time = time.time()
    last_step = "(no output yet)"
    active_generator_step = ""
    recent_lines = deque(maxlen=200)

    while True:
        if proc.poll() is not None:
            # Drain remaining lines after process exits.
            for tail in proc.stdout:
                line = tail.rstrip("\n")
                if line:
                    print(line)
                    last_step = line
                    recent_lines.append(line)
            if proc.returncode != 0:
                print_known_issue_hints(list(recent_lines), last_step)
            return proc.returncode

        ready, _, _ = select.select([proc.stdout], [], [], 1.0)
        if ready:
            line = proc.stdout.readline()
            if line:
                line = line.rstrip("\n")
                if line:
                    print(line)
                    last_step = line
                    recent_lines.append(line)
                    if "Generating Halide AOT" in line:
                        active_generator_step = line
                last_output_time = time.time()
            continue

        idle = time.time() - last_output_time
        if idle > idle_timeout_sec:
            suspected_step = active_generator_step if active_generator_step else last_step
            print(
                f"[ERROR] Build appears stuck for >{idle_timeout_sec}s at step:\n"
                f"        {suspected_step}\n"
                "        Please check build dependencies/custom commands in CMakeLists.txt "
                "(OUTPUT/DEPENDS/target wiring).",
                file=sys.stderr,
            )
            print_dependency_hints(native_dir, build_dir, suspected_step)
            print_known_issue_hints(list(recent_lines), suspected_step)
            proc.kill()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.terminate()
            return 124


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build native targets with max cores and a no-output watchdog."
    )
    parser.add_argument(
        "--native-dir",
        default="dng_processor/native",
        help="Path to native project root (default: dng_processor/native)",
    )
    parser.add_argument(
        "--build-dir",
        default="build",
        help="Build directory relative to --native-dir, or absolute path (default: build)",
    )
    parser.add_argument(
        "--target",
        default="test_decode",
        help="Build target (default: test_decode)",
    )
    parser.add_argument(
        "--idle-timeout-sec",
        type=int,
        default=60,
        help="Fail if no build output appears for this many seconds (default: 60)",
    )
    parser.add_argument(
        "--skip-configure",
        action="store_true",
        help="Skip cmake configure step",
    )
    args = parser.parse_args()

    native_dir = Path(args.native_dir).resolve()
    if not native_dir.exists():
        print(f"[ERROR] native dir not found: {native_dir}", file=sys.stderr)
        return 2

    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = (native_dir / build_dir).resolve()

    cores = os.cpu_count() or 1
    print(f"[INFO] Using max cores: {cores}")
    print(f"[INFO] Idle timeout: {args.idle_timeout_sec}s")
    print(f"[INFO] Target: {args.target}")

    if not args.skip_configure:
        configure_cmd = ["cmake", "-S", str(native_dir), "-B", str(build_dir)]
        code = run_with_watchdog(
            configure_cmd,
            cwd=native_dir.parent,
            idle_timeout_sec=args.idle_timeout_sec,
            native_dir=native_dir,
            build_dir=build_dir,
        )
        if code != 0:
            return code

    build_cmd = [
        "cmake",
        "--build",
        str(build_dir),
        "--target",
        args.target,
        f"-j{cores}",
    ]
    return run_with_watchdog(
        build_cmd,
        cwd=native_dir.parent,
        idle_timeout_sec=args.idle_timeout_sec,
        native_dir=native_dir,
        build_dir=build_dir,
    )


if __name__ == "__main__":
    raise SystemExit(main())
