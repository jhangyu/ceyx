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

    import re as _re
    # W5-6 / TD-25: step_map now uses regex patterns so partial / reworded
    # AOT step names still match correctly.
    step_map = [
        (_re.compile(r"Rectilinear Warp", _re.IGNORECASE), "dng_warp_aot_target"),
        (_re.compile(r"Stage4.*Render|Halide AOT Stage4", _re.IGNORECASE), "dng_render_aot_target"),
        (_re.compile(r"No-?Map Render", _re.IGNORECASE), "dng_render_nomap_aot_target"),
        (_re.compile(r"Maps.*NoEncod|Maps-?NoEncoding", _re.IGNORECASE), "dng_render_maps_noencode_aot_target"),
        (_re.compile(r"Tail Render", _re.IGNORECASE), "dng_render_tail_aot_target"),
        (_re.compile(r"Tone.*Tail|ToneTail", _re.IGNORECASE), "dng_render_tonetail_aot_target"),
        (_re.compile(r"Demosaic.*Warp|fused.*demosaic", _re.IGNORECASE), "dng_demosaic_warp_aot_target"),
        (_re.compile(r"Demosaic Bilinear|demosaic_bilinear", _re.IGNORECASE), "dng_demosaic_aot_target"),
        (_re.compile(r"Polynomial3|polynomial3", _re.IGNORECASE), "dng_opcode_polynomial3_aot_target"),
        (_re.compile(r"Polynomial|OpcodePolynomial", _re.IGNORECASE), "dng_opcode_polynomial_aot_target"),
    ]
    for pattern, target in step_map:
        if pattern.search(last_step):
            build_make = build_dir / "CMakeFiles" / f"{target}.dir" / "build.make"
            print(f"[HINT] Check generated rule: {build_make}", file=sys.stderr)
            return


def print_known_issue_hints(lines: list[str], suspected_step: str) -> None:
    joined = "\n".join(lines)

    if "Generating Halide AOT Rectilinear Warp" in suspected_step:
        print(
            "[HINT] Warp AOT is the current long-running step. If this repeats often, "
            "check RectilinearWarpGenerator schedule complexity (tile/unroll/vectorize) to reduce codegen load.",
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
            # W5-6 / TD-25: dump last 20 lines for easier diagnosis
            recent_snapshot = list(recent_lines)[-20:]
            if recent_snapshot:
                print(
                    f"[ERROR] Last {len(recent_snapshot)} output lines before timeout:",
                    file=sys.stderr,
                )
                for dump_line in recent_snapshot:
                    print(f"  | {dump_line}", file=sys.stderr)
            print_dependency_hints(native_dir, build_dir, suspected_step)
            print_known_issue_hints(list(recent_lines), suspected_step)
            proc.kill()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.terminate()
            return 124


def repo_root_from_native_dir(native_dir: Path) -> Path:
    # Post-restructure layout: <repo>/native/ and <repo>/app/ are siblings.
    return native_dir.parent


def app_dir_from_native_dir(native_dir: Path) -> Path:
    return repo_root_from_native_dir(native_dir) / "app"


def dylib_from_native_dir(native_dir: Path) -> Path:
    # Derived from native_dir directly, NOT via app_dir.
    return native_dir / "build" / "libdng_decoder_native.dylib"


def configuration_for_mode(mode: str) -> str:
    return {
        "debug": "Debug",
        "profile": "Profile",
        "release": "Release",
    }[mode]


def embed_macos_dylib(native_dir: Path, app_bundle: Optional[Path] = None) -> int:
    native_dylib = dylib_from_native_dir(native_dir)
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


def publish_macos_dist(app_dir: Path, native_dir: Path, mode: str) -> int:
    config = configuration_for_mode(mode)
    app_source = app_dir / "build" / "macos" / "Build" / "Products" / config / "ceyx_example.app"
    native_dylib = dylib_from_native_dir(native_dir)
    if not app_source.exists():
        print(f"[ERROR] Missing Flutter app bundle: {app_source}", file=sys.stderr)
        return 1
    if not native_dylib.exists():
        print(f"[ERROR] Missing native dylib: {native_dylib}", file=sys.stderr)
        return 1

    dist_dir = app_dir / "dist"
    dist_app = dist_dir / "ceyx_example.app"
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


def strip_android_so(ndk_path: Path, so_path: Path) -> int:
    """Strip the built Android shared library via the NDK's llvm-strip.

    No prior artifact convention exists for keeping an unstripped copy
    alongside the publish output, so this strips in place and does not
    invent a new layout.
    """
    strip_candidates = sorted(ndk_path.glob("toolchains/llvm/prebuilt/*/bin/llvm-strip"))
    if not strip_candidates:
        print(
            f"[WARN] llvm-strip not found under {ndk_path}/toolchains/llvm/prebuilt/*/bin; "
            "skipping strip.",
            file=sys.stderr,
        )
        return 0
    llvm_strip = strip_candidates[0]
    print(f"[INFO] Stripping {so_path} with {llvm_strip}")
    result = subprocess.run([str(llvm_strip), "--strip-unneeded", str(so_path)])
    if result.returncode != 0:
        print(f"[ERROR] llvm-strip failed on {so_path}", file=sys.stderr)
    return result.returncode


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

    dest = app_dir / "dist" / "ceyx_example.apk"
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


def build_android(args, native_dir: Path, cores: int) -> int:
    android_build_dir = (native_dir / "build-android").resolve()
    host_gen_dir = (android_build_dir / "host-generators").resolve()
    aot_dir = host_gen_dir / "halide_generated"

    aot_target = "arm-64-android-vulkan-vk_int8-vk_int16-vk_int64-no_asserts-no_bounds_query"

    # --- Stage 1: Host generators + AOT ---
    print("[INFO] === Stage 1: Host generators + AOT ===")
    if not args.skip_configure:
        stage1_configure = [
            "cmake", "-S", str(native_dir), "-B", str(host_gen_dir),
            "-DDNG_HOST_GENERATORS_ONLY=ON",
            f"-DDNG_AOT_TARGET_OVERRIDE={aot_target}",
            # Perf fix (2026-07-04): explicit Release even though CMakeLists.txt
            # now defaults to it, so this stays correct if the cache already
            # pinned a different CMAKE_BUILD_TYPE from a prior configure.
            "-DCMAKE_BUILD_TYPE=Release",
            *args.cmake_arg,
        ]
        code = run_with_watchdog(stage1_configure, cwd=native_dir.parent,
                                  idle_timeout_sec=args.idle_timeout_sec,
                                  native_dir=native_dir, build_dir=host_gen_dir)
        if code != 0:
            return code

    stage1_build = [
        "cmake", "--build", str(host_gen_dir), f"-j{cores}",
    ]
    code = run_with_watchdog(stage1_build, cwd=native_dir.parent,
                              idle_timeout_sec=args.idle_timeout_sec,
                              native_dir=native_dir, build_dir=host_gen_dir)
    if code != 0:
        return code

    # Verify AOT artifacts
    expected_aot = [
        # dng_pipeline.a/.h dropped: that kernel was retired 2026-07-02 (4ec5664).
        "halide_runtime.a",
        "dng_demosaic_bilinear.a", "dng_demosaic_bilinear.h",
        "dng_demosaic_warp.a", "dng_demosaic_warp.h",
        "rectilinear_warp.a", "rectilinear_warp.h",
        "dng_render_stage4.a", "dng_render_stage4.h",
        "dng_opcode_polynomial.a", "dng_opcode_polynomial.h",
        "dng_opcode_polynomial3.a", "dng_opcode_polynomial3.h",
    ]
    missing = [f for f in expected_aot if not (aot_dir / f).exists()]
    if missing:
        print(f"[ERROR] Missing AOT artifacts after Stage 1:", file=sys.stderr)
        for m in missing:
            print(f"  {aot_dir / m}", file=sys.stderr)
        return 1
    print(f"[OK] Stage 1 complete: {len(expected_aot)} AOT artifacts verified in {aot_dir}")

    if args.skip_stage2:
        print("[INFO] --skip-stage2: skipping NDK cross-compile")
        return 0

    # --- Stage 2: NDK cross-compile (NDK required from here) ---
    ndk_home = args.android_ndk or os.environ.get("ANDROID_NDK_HOME")
    if not ndk_home:
        print("[ERROR] ANDROID_NDK_HOME not set and --android-ndk not provided.", file=sys.stderr)
        print("[HINT] Set ANDROID_NDK_HOME or pass --android-ndk /path/to/ndk", file=sys.stderr)
        return 1
    ndk_path = Path(ndk_home)
    if not ndk_path.exists():
        print(f"[ERROR] NDK path does not exist: {ndk_path}", file=sys.stderr)
        return 1
    toolchain_file = ndk_path / "build" / "cmake" / "android.toolchain.cmake"
    if not toolchain_file.exists():
        print(f"[ERROR] NDK toolchain not found: {toolchain_file}", file=sys.stderr)
        return 1
    cross_dir = (android_build_dir / "android-arm64").resolve()

    print("[INFO] === Stage 2: NDK cross-compile ===")
    if not args.skip_configure:
        stage2_configure = [
            "cmake", "-S", str(native_dir), "-B", str(cross_dir),
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file}",
            f"-DANDROID_ABI={args.android_abi}",
            f"-DANDROID_PLATFORM={args.android_platform}",
            "-DDNG_CROSS_BUILD=ON",
            f"-DDNG_PREBUILT_AOT_DIR={aot_dir}",
            # Perf fix (2026-07-04): explicit Release even though CMakeLists.txt
            # now defaults to it, so this stays correct if the cache already
            # pinned a different CMAKE_BUILD_TYPE from a prior configure.
            "-DCMAKE_BUILD_TYPE=Release",
            *args.cmake_arg,
        ]
        code = run_with_watchdog(stage2_configure, cwd=native_dir.parent,
                                  idle_timeout_sec=args.idle_timeout_sec,
                                  native_dir=native_dir, build_dir=cross_dir)
        if code != 0:
            return code

    target = args.target
    stage2_build = [
        "cmake", "--build", str(cross_dir),
        "--target", target, f"-j{cores}",
    ]
    code = run_with_watchdog(stage2_build, cwd=native_dir.parent,
                              idle_timeout_sec=args.idle_timeout_sec,
                              native_dir=native_dir, build_dir=cross_dir)
    if code != 0:
        return code

    # Verify output. Android Stage 2 can build either shared libraries or
    # executable smoke-test targets such as test_decode_android.
    target_path = cross_dir / target
    so_path = cross_dir / f"lib{target}.so"
    if target_path.exists():
        print(f"[OK] Stage 2 complete: {target_path}")
    elif so_path.exists():
        print(f"[OK] Stage 2 complete: {so_path}")
        strip_code = strip_android_so(ndk_path, so_path)
        if strip_code != 0:
            return strip_code
    else:
        print(
            f"[WARN] Expected {target_path} or {so_path} not found; check build output.",
            file=sys.stderr,
        )

    return 0


def main() -> int:
    argv = sys.argv[1:]
    target_was_explicit = any(
        arg == "--target" or arg.startswith("--target=") for arg in argv
    )
    parser = argparse.ArgumentParser(
        description="Build native targets with max cores and a no-output watchdog."
    )
    parser.add_argument(
        "--native-dir",
        default="native",
        help="Path to native project root (default: native)",
    )
    parser.add_argument(
        "--build-dir",
        default="build",
        help="Build directory relative to --native-dir, or absolute path (default: build)",
    )
    parser.add_argument(
        "--target",
        default="dng_decoder_native",
        help=(
            "Build target (default: dng_decoder_native). When --target is "
            "omitted, this script also builds and publishes the macOS app and "
            "Android APK by default."
        ),
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
        "--no-build-macos-app",
        action="store_true",
        help="Disable the default macOS app build when --target is omitted.",
    )
    parser.add_argument(
        "--build-android-app",
        action="store_true",
        help="After native build, run flutter build apk and publish dist/ceyx_example.apk.",
    )
    parser.add_argument(
        "--no-build-android-app",
        action="store_true",
        help="Disable the default Android APK build when --target is omitted.",
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
    parser.add_argument(
        "--platform",
        choices=["host", "android"],
        default="host",
        help="Target platform. 'android' triggers two-stage cross-compile (default: host).",
    )
    parser.add_argument(
        "--android-ndk",
        default=None,
        help="Android NDK root (default: $ANDROID_NDK_HOME env var).",
    )
    parser.add_argument(
        "--android-abi",
        default="arm64-v8a",
        help="Android ABI (default: arm64-v8a).",
    )
    parser.add_argument(
        "--android-platform",
        default="android-24",
        help="Android platform level (default: android-24, minimum for Vulkan).",
    )
    parser.add_argument(
        "--skip-stage2",
        action="store_true",
        help="(Android only) Only run Stage 1 (host generators + AOT), skip NDK cross-compile.",
    )
    args = parser.parse_args()

    native_dir = Path(args.native_dir).resolve()
    if not native_dir.exists():
        print(f"[ERROR] native dir not found: {native_dir}", file=sys.stderr)
        return 2
    app_dir = app_dir_from_native_dir(native_dir)

    if args.embed_macos_dylib_only:
        app_bundle = Path(args.app_bundle).resolve() if args.app_bundle else None
        return embed_macos_dylib(native_dir, app_bundle)

    if args.publish_macos_dist_only:
        return publish_macos_dist(app_dir, native_dir, args.macos_mode)
    if args.publish_android_dist_only:
        return publish_android_dist(app_dir, args.android_mode)
    if args.publish_web_dist_only:
        return publish_web_dist(app_dir)

    default_product_build = not target_was_explicit and not args.build_web_app
    build_macos_app = args.build_macos_app or (
        default_product_build and not args.no_build_macos_app
    )
    build_android_app = args.build_android_app or (
        default_product_build and not args.no_build_android_app
    )

    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = (native_dir / build_dir).resolve()

    cores = os.cpu_count() or 1
    print(f"[INFO] Using max cores: {cores}")
    print(f"[INFO] Idle timeout: {args.idle_timeout_sec}s")
    print(f"[INFO] Target: {args.target}")
    if args.cmake_arg:
        print(f"[INFO] Extra cmake args: {args.cmake_arg}")

    if args.platform == "android":
        return build_android(args, native_dir, cores)

    native_needed = args.target != "none" and not args.build_web_app
    flutter_idle_timeout_sec = max(args.idle_timeout_sec, 300)
    if native_needed and not args.skip_configure:
        # Perf fix (2026-07-04): explicit Release even though CMakeLists.txt now
        # defaults to it, so this stays correct if the cache already pinned a
        # different CMAKE_BUILD_TYPE from a prior configure.
        configure_cmd = [
            "cmake", "-S", str(native_dir), "-B", str(build_dir),
            "-DCMAKE_BUILD_TYPE=Release",
            *args.cmake_arg,
        ]
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

    if build_macos_app:
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
        app_bundle = app_dir / "build" / "macos" / "Build" / "Products" / config / "ceyx_example.app"
        code = embed_macos_dylib(native_dir, app_bundle)
        if code != 0:
            return code
        code = subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(app_bundle)]).returncode
        if code != 0:
            return code
        code = publish_macos_dist(app_dir, native_dir, args.macos_mode)
        if code != 0:
            return code

    if build_android_app:
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
        code = publish_android_dist(app_dir, args.android_mode)
        if code != 0:
            return code

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
