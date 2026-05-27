#!/usr/bin/env python3
import argparse
from collections import deque
import os
import select
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional


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


def app_dir_from_native_dir(native_dir: Path) -> Path:
    return native_dir.parent


def configuration_for_mode(mode: str) -> str:
    return {
        "debug": "Debug",
        "profile": "Profile",
        "release": "Release",
    }[mode]


def embed_macos_dylib(app_dir: Path, app_bundle: Optional[Path] = None) -> int:
    native_dylib = app_dir / "native" / "build" / "libdng_decoder_native.dylib"
    if not native_dylib.exists():
        print(f"[ERROR] Missing native dylib: {native_dylib}", file=sys.stderr)
        return 1

    if app_bundle is not None:
        frameworks_dir = app_bundle / "Contents" / "Frameworks"
    else:
        target_build_dir = os.environ.get("TARGET_BUILD_DIR")
        frameworks_folder_path = os.environ.get("FRAMEWORKS_FOLDER_PATH")
        if not target_build_dir or not frameworks_folder_path:
            print(
                "[ERROR] Could not determine app Frameworks directory. "
                "Run from Xcode/Flutter or pass --app-bundle.",
                file=sys.stderr,
            )
            return 2
        frameworks_dir = Path(target_build_dir) / frameworks_folder_path

    frameworks_dir.mkdir(parents=True, exist_ok=True)
    dest = frameworks_dir / "libdng_decoder_native.dylib"
    shutil.copy2(native_dylib, dest)
    code = subprocess.run(["codesign", "--force", "--sign", "-", str(dest)]).returncode
    if code != 0:
        return code
    print(f"[OK] Embedded native dylib: {dest}")
    return 0


def publish_macos_dist(app_dir: Path, mode: str) -> int:
    config = configuration_for_mode(mode)
    app_source = app_dir / "build" / "macos" / "Build" / "Products" / config / "dng_processor.app"
    native_dylib = app_dir / "native" / "build" / "libdng_decoder_native.dylib"
    if not app_source.exists():
        print(f"[ERROR] Missing Flutter app bundle: {app_source}", file=sys.stderr)
        return 1
    if not native_dylib.exists():
        print(f"[ERROR] Missing native dylib: {native_dylib}", file=sys.stderr)
        return 1

    dist_dir = app_dir / "dist"
    dist_app = dist_dir / "dng_processor.app"
    dist_dir.mkdir(parents=True, exist_ok=True)

    shutil.copy2(native_dylib, dist_dir / "libdng_decoder_native.dylib")
    if dist_app.exists():
        shutil.rmtree(dist_app)
    shutil.copytree(app_source, dist_app, symlinks=True)
    print("[OK] macOS artifacts published")
    print(f"  App:   {dist_app}")
    print(f"  Dylib: {dist_dir / 'libdng_decoder_native.dylib'}")
    print(f"  Cache: {app_source}")
    return 0


def latest_file(paths: list[Path]) -> Optional[Path]:
    existing = [path for path in paths if path.exists()]
    if not existing:
        return None
    return max(existing, key=lambda path: path.stat().st_mtime)


def publish_android_dist(app_dir: Path, mode: str) -> int:
    mode_dir = "debug" if mode == "debug" else mode
    candidates = [
        app_dir / "build" / "app" / "outputs" / "flutter-apk" / f"app-{mode}.apk",
        app_dir / "build" / "app" / "outputs" / "apk" / mode_dir / f"app-{mode}.apk",
        app_dir / "build" / "app" / "outputs" / "apk" / mode_dir / "app.apk",
    ]
    apk_source = latest_file(candidates)
    if apk_source is None:
        print("[ERROR] Missing Android APK. Checked:", file=sys.stderr)
        for path in candidates:
            print(f"        {path}", file=sys.stderr)
        return 1

    dest = app_dir / "dist" / "dng_processor.apk"
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(apk_source, dest)
    print("[OK] Android artifact published")
    print(f"  APK:   {dest}")
    print(f"  Cache: {apk_source}")
    return 0


def publish_web_dist(app_dir: Path) -> int:
    web_source = app_dir / "build" / "web"
    if not web_source.exists():
        print(f"[ERROR] Missing web build: {web_source}", file=sys.stderr)
        return 1

    web_dest = app_dir / "dist" / "web"
    if web_dest.exists():
        shutil.rmtree(web_dest)
    web_dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(web_source, web_dest, symlinks=True)
    print("[OK] Web artifact published")
    print(f"  Web:   {web_dest}")
    print(f"  Cache: {web_source}")
    return 0


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
    parser.add_argument(
        "--cmake-arg",
        action="append",
        default=[],
        help="Extra argument passed to cmake configure step; repeatable",
    )
    parser.add_argument(
        "--build-macos-app",
        action="store_true",
        help="After native build, run flutter build macos and publish shallow dist artifacts.",
    )
    parser.add_argument(
        "--build-android-app",
        action="store_true",
        help="After native build, run flutter build apk and publish dist/dng_processor.apk.",
    )
    parser.add_argument(
        "--build-web-app",
        action="store_true",
        help="Run flutter build web and publish dist/web.",
    )
    parser.add_argument(
        "--macos-mode",
        choices=["debug", "profile", "release"],
        default="debug",
        help="macOS app build mode used with --build-macos-app (default: debug).",
    )
    parser.add_argument(
        "--android-mode",
        choices=["debug", "profile", "release"],
        default="debug",
        help="Android APK build mode used with --build-android-app (default: debug).",
    )
    parser.add_argument(
        "--flutter-bin",
        default="flutter",
        help="Flutter executable used for app builds (default: flutter).",
    )
    parser.add_argument(
        "--embed-macos-dylib-only",
        action="store_true",
        help="Only embed native dylib into the current Xcode app bundle, then exit.",
    )
    parser.add_argument(
        "--app-bundle",
        default=None,
        help="Explicit .app path for --embed-macos-dylib-only.",
    )
    parser.add_argument(
        "--publish-macos-dist-only",
        action="store_true",
        help="Only publish existing macOS app/dylib artifacts to dist, then exit.",
    )
    parser.add_argument(
        "--publish-android-dist-only",
        action="store_true",
        help="Only publish an existing Android APK artifact to dist, then exit.",
    )
    parser.add_argument(
        "--publish-web-dist-only",
        action="store_true",
        help="Only publish an existing web artifact to dist, then exit.",
    )
    args = parser.parse_args()

    native_dir = Path(args.native_dir).resolve()
    if not native_dir.exists():
        print(f"[ERROR] native dir not found: {native_dir}", file=sys.stderr)
        return 2
    app_dir = app_dir_from_native_dir(native_dir)

    if args.embed_macos_dylib_only:
        app_bundle = Path(args.app_bundle).resolve() if args.app_bundle else None
        return embed_macos_dylib(app_dir, app_bundle)

    if args.publish_macos_dist_only:
        return publish_macos_dist(app_dir, args.macos_mode)
    if args.publish_android_dist_only:
        return publish_android_dist(app_dir, args.android_mode)
    if args.publish_web_dist_only:
        return publish_web_dist(app_dir)

    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = (native_dir / build_dir).resolve()

    cores = os.cpu_count() or 1
    print(f"[INFO] Using max cores: {cores}")
    print(f"[INFO] Idle timeout: {args.idle_timeout_sec}s")
    print(f"[INFO] Target: {args.target}")
    if args.cmake_arg:
        print(f"[INFO] Extra cmake args: {args.cmake_arg}")

    native_needed = args.target != "none" and not args.build_web_app
    flutter_idle_timeout_sec = max(args.idle_timeout_sec, 300)
    if native_needed and not args.skip_configure:
        configure_cmd = ["cmake", "-S", str(native_dir), "-B", str(build_dir), *args.cmake_arg]
        code = run_with_watchdog(
            configure_cmd,
            cwd=native_dir.parent,
            idle_timeout_sec=args.idle_timeout_sec,
            native_dir=native_dir,
            build_dir=build_dir,
        )
        if code != 0:
            return code

    if native_needed:
        build_cmd = [
            "cmake",
            "--build",
            str(build_dir),
            "--target",
            args.target,
            f"-j{cores}",
        ]
        code = run_with_watchdog(
            build_cmd,
            cwd=native_dir.parent,
            idle_timeout_sec=args.idle_timeout_sec,
            native_dir=native_dir,
            build_dir=build_dir,
        )
        if code != 0:
            return code

    if args.build_macos_app:
        flutter_cmd = [args.flutter_bin, "build", "macos", f"--{args.macos_mode}"]
        code = run_with_watchdog(
            flutter_cmd,
            cwd=app_dir,
            idle_timeout_sec=flutter_idle_timeout_sec,
            native_dir=native_dir,
            build_dir=build_dir,
        )
        if code != 0:
            return code
        config = configuration_for_mode(args.macos_mode)
        app_bundle = app_dir / "build" / "macos" / "Build" / "Products" / config / "dng_processor.app"
        code = embed_macos_dylib(app_dir, app_bundle)
        if code != 0:
            return code
        code = subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(app_bundle)]).returncode
        if code != 0:
            return code
        return publish_macos_dist(app_dir, args.macos_mode)

    if args.build_android_app:
        flutter_cmd = [args.flutter_bin, "build", "apk", f"--{args.android_mode}"]
        code = run_with_watchdog(
            flutter_cmd,
            cwd=app_dir,
            idle_timeout_sec=flutter_idle_timeout_sec,
            native_dir=native_dir,
            build_dir=build_dir,
        )
        if code != 0:
            return code
        return publish_android_dist(app_dir, args.android_mode)

    if args.build_web_app:
        flutter_cmd = [args.flutter_bin, "build", "web"]
        code = run_with_watchdog(
            flutter_cmd,
            cwd=app_dir,
            idle_timeout_sec=flutter_idle_timeout_sec,
            native_dir=native_dir,
            build_dir=build_dir,
        )
        if code != 0:
            return code
        return publish_web_dist(app_dir)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
