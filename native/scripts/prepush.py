#!/usr/bin/env python3
"""`ci.py prepush` -- the single merged pre-push gate (user ruling 2026-10-03).

ONE command, run from a working tree, that:

  1. clones the LOCAL repo's committed HEAD into a fresh scratch directory
     (uncommitted edits are invisible to it by construction -- that is the
     point: the gate judges what a push would publish, not the working tree);
  2. seeds the clone with the gitignored heavy inputs a fresh clone lacks
     (the ~540 MB Halide v21 binary dist and the image_samples/ test
     fixtures), COPIED from this working tree and verified file-by-file
     (count, bytes, sha256) -- never re-downloaded;
  3. hands off to the CLONE's own copy of this module (`--inner`), which runs
     every step against the clone: the CI-equivalent compile/verify steps
     derived from .github/workflows (same `ci.py` verbs, same argv) AND the
     repo's automated test suites.

Local red = no push. Remote CI stays compile-only (CI 純編譯鐵律 2026-10-02);
this module is a LOCAL gate and must never be referenced by a workflow --
the `policy-no-prepush-in-workflows` step fails if one does.

SCOPE IS DERIVED FROM THE WORKFLOWS, not mirrored from a build leg. Every job
of every .github/workflows/*.yml is classified in `JOB_SCOPE` below as
included / skip-on-this-host / excluded, with the reason. The
`scope-derivation` step re-parses the workflow files on every run and FAILS
when a job exists that `JOB_SCOPE` does not classify (or vice versa), so a
new workflow job cannot silently fall outside the gate.

SKIPS ARE COUNTED, NEVER SILENT. A step not runnable on this host prints a
`PREPUSH_SKIP(<step>): <reason>` line and is counted under `skipped=` in the
summary; a skip never counts as a pass. A step that SHOULD run on this host
but has no implementation here is a FAILURE (`unimplemented`), not a skip.

ARTIFACT CONTRACT:
  * every step prints `PREPUSH_STEP_BEGIN(<step>)` and, immediately after its
    child process(es) return, `PREPUSH_STEP_RC(<step>)=<rc>` -- the RC is the
    child's own `returncode`, captured in this process, never read off a pipe;
  * exactly one
    `PREPUSH-SUMMARY host=... head=... total=N passed=N failed=N skipped=N skipped_host=N partial=0|1`
    line per run; `partial=1` whenever --step/--skip-step narrowed the run,
    so a narrowed green can never be mistaken for the gate;
  * exit code is 0 iff failed == 0.

TWO KINDS OF SKIP, counted apart:
  * `skipped`      -- the item belongs to another host's leg (macOS leg on a
                      Windows host) or is declared manual in gates.py;
  * `skipped_host` -- the item SHOULD be portable but is not yet runnable on
                      this host. Each entry in `HOST_UNSUPPORTED` names an
                      evidence class and evidence, is printed as
                      `PREPUSH_HOST_UNSUPPORTED ...` (the machine-readable
                      inventory block at the end of the log), and is
                      re-proven on every run where that is mechanical: a
                      declared-unbuildable test target that starts building
                      turns the gate RED as a stale entry.

GUARDS: with a Docker daemon the guards step runs the digest-pinned
container form; without one it runs `ci.py guards`'s host form (same roster)
and prints `guards: host-form (no docker on host)`.

Usage (from a working tree):
    python3 native/scripts/ci.py prepush [--log FILE] [--scratch DIR] [--keep-scratch]
                                         [--step NAME ...] [--skip-step NAME ...]
    python3 native/scripts/ci.py prepush --list
"""
from __future__ import annotations

import argparse
import hashlib
import os
import platform as platform_module
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

REPO_ROOT = Path(__file__).resolve().parents[2]

# Every child process inherits UTF-8 mode: CI runners run with a UTF-8
# locale, a Windows developer host defaults to a legacy code page (cp950 on
# this project's machine) under which `Path.read_text()` of the repo's UTF-8
# files raises. Mirroring the runner's locale is not masking a defect.
CHILD_ENV_OVERRIDES = {"PYTHONUTF8": "1", "PYTHONIOENCODING": "utf-8"}

WORKFLOWS_DIR = Path(".github") / "workflows"

# Gitignored inputs copied into the clone. Each is (relative path, why).
# The Halide dist seed stands in for every build leg's `build_deps.py fetch
# halide` step (~540 MB download); image_samples/ is the test-fixture corpus the runners
# hash-verify themselves (run_decode_matrix.py `_verify_fixture_hashes`).
SEEDS = {
    "halide": Path("native/third_party/halide"),
    "samples": Path("image_samples"),
}

WINDOWS_X64 = "windows-x86_64"
MACOS_ARM64 = "macos-arm64"
LINUX_X64 = "linux-x86_64"
ALL_HOSTS = frozenset({WINDOWS_X64, MACOS_ARM64, LINUX_X64})


# ---------------------------------------------------------------------------
# Output primitives. Everything goes through `emit` so the outer process can
# tee one stream into the artifact.
# ---------------------------------------------------------------------------
def emit(line: str = "") -> None:
    print(line, flush=True)


def host_key() -> str:
    system = platform_module.system()
    machine = platform_module.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "x86_64" if machine in ("amd64", "x86_64") else machine
    if system == "Windows":
        return f"windows-{arch}"
    if system == "Darwin":
        return f"macos-{arch}"
    return f"linux-{arch}"


def child_env(extra: Optional[dict] = None) -> dict:
    env = dict(os.environ)
    env.update(CHILD_ENV_OVERRIDES)
    if extra:
        env.update(extra)
    return env


def stream(argv, cwd, env=None, tee: Optional[Path] = None) -> int:
    """Run argv (list, shell=False), forwarding its combined output line by
    line to our stdout (and to ``tee`` if given) as it is produced, and
    return ITS returncode. A missing executable is rc 127 with the OSError
    text, never an exception."""
    argv = [os.fspath(a) for a in argv]
    emit(f"PREPUSH_EXEC: {' '.join(argv)}  (cwd={cwd})")
    try:
        proc = subprocess.Popen(
            argv,
            shell=False,
            cwd=os.fspath(cwd),
            env=env if env is not None else child_env(),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError as exc:
        emit(f"PREPUSH_EXEC_ERROR: {exc}")
        return 127
    assert proc.stdout is not None
    sink = tee.open("w", encoding="utf-8") if tee is not None else None
    try:
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            if sink is not None:
                sink.write(line)
    finally:
        if sink is not None:
            sink.close()
    return proc.wait()


def capture(argv, cwd, env=None) -> tuple:
    try:
        done = subprocess.run(
            [os.fspath(a) for a in argv], shell=False, cwd=os.fspath(cwd),
            env=env if env is not None else child_env(), capture_output=True,
            text=True, encoding="utf-8", errors="replace", stdin=subprocess.DEVNULL,
        )
    except OSError as exc:
        return 127, f"{exc}\n"
    return done.returncode, (done.stdout or "") + (done.stderr or "")


# ---------------------------------------------------------------------------
# Step model.
# ---------------------------------------------------------------------------
SKIP = "SKIP"
HOSTSKIP = "HOSTSKIP"


@dataclass
class Ctx:
    clone: Path
    host: str
    env: dict = field(default_factory=dict)  # toolchain env (Windows: vcvars)
    host_skips: list = field(default_factory=list)  # (item, class, evidence)

    def py(self, *args) -> list:
        return [sys.executable, *args]

    def run(self, argv, cwd: Optional[Path] = None) -> int:
        return stream(argv, cwd or self.clone, env=self.env or child_env())


@dataclass
class Step:
    name: str
    derivation: str  # which workflow job/step (or test entry) this comes from
    hosts: frozenset  # hosts on which it RUNS
    action: Optional[Callable[[Ctx], object]]  # returns int rc, or (SKIP, reason)
    skip_reason: str = ""  # printed on hosts outside `hosts`
    group: str = "ci"  # "ci" (workflow-derived) or "test" (test layer)


# Workflow-job classification. Key: "<workflow file>:<job id>". Value:
# (decision, reason). decision in {"included", "host-specific", "excluded"}.
# "included"      -- its locally-runnable steps are in STEPS on every host.
# "host-specific" -- runs only on the host named in its STEPS entries; other
#                    hosts print a counted SKIP.
# "excluded"      -- never part of the gate; reason says why.
JOB_SCOPE: dict = {
    "build.yml:macos": ("host-specific", "calls macos_build.yml (classified below)"),
    "build.yml:linux": ("host-specific", "calls linux_build.yml (classified below)"),
    "build.yml:windows": ("host-specific", "calls windows_build.yml (classified below)"),
    "build.yml:android": ("host-specific", "calls android_build.yml (classified below)"),
    "build.yml:heif-dist-windows": ("excluded", "dist leg: CI runs it only on v* tags / run_dists dispatch, never on a push; its output is a committed, pinned input"),
    "build.yml:jxl-dist-windows": ("excluded", "dist leg: tag / run_dists only, committed pinned input"),
    "build.yml:webp-dist-windows": ("excluded", "dist leg: tag / run_dists only, committed pinned input"),
    "build.yml:libomp-dist-windows": ("excluded", "dist leg: tag / run_dists only, committed pinned input"),
    "build.yml:heif-dist-android": ("excluded", "dist leg: tag / run_dists only, committed pinned input"),
    "build.yml:jxl-dist-android": ("excluded", "dist leg: tag / run_dists only, committed pinned input"),
    "build.yml:webp-dist-android": ("excluded", "dist leg: tag / run_dists only, committed pinned input"),
    "build.yml:verify-dart": ("included", "flutter pub get + dart analyze (app/) -- steps dart-analyze"),
    "build.yml:verify-native-tests": ("included", "selftest + vendored-LibRaw guards on every host; its test_decode/test_device_handoff build is the test layer's build step"),
    "build.yml:guards-container": ("included", "step guards: the digest-pinned docker form when a Docker daemon is up, else `ci.py guards` host form (same roster, form printed)"),
    "build.yml:all-platforms-green": ("excluded", "aggregator over other jobs' results; no command of its own"),
    "build.yml:publish": ("excluded", "release publication (needs downloaded run artifacts + GitHub token); not a verification step"),
    "android_build.yml:build-android": ("host-specific", "ubuntu runner + apt toolchain + NDK cross-build; not ported to a local host"),
    "linux_build.yml:build-linux": ("host-specific", "runs inside ubuntu:22.04 (glibc floor container); not ported to a local host"),
    "macos_build.yml:build": ("host-specific", "macOS arm64 native + x86_64 cross legs (Metal); macOS host only"),
    "windows_build.yml:build-windows": ("host-specific", "x86_64 row runs on a Windows x86_64 host; arm64 row needs an ARM64 host to LoadLibrary its probes"),
    "heif_dist_android.yml:build-heif-dist": ("excluded", "dist workflow (push trigger only on its own ci/** paths, tag/run_dists via build.yml); committed pinned input"),
    "heif_dist_windows.yml:build-heif-dist": ("excluded", "dist workflow; committed pinned input"),
    "jxl_dist_android.yml:build-jxl-dist": ("excluded", "dist workflow; committed pinned input"),
    "jxl_dist_windows.yml:build-jxl-dist": ("excluded", "dist workflow; committed pinned input"),
    "libomp_dist_windows.yml:build-libomp-dist": ("excluded", "dist workflow; committed pinned input"),
    "webp_dist_android.yml:build-webp-dist": ("excluded", "dist workflow; committed pinned input"),
    "webp_dist_windows.yml:build-webp-dist": ("excluded", "dist workflow; committed pinned input"),
}

_JOB_LINE = re.compile(r"^  ([A-Za-z0-9_-]+):\s*(#.*)?$")


def workflow_jobs(workflows_dir: Path) -> set:
    """Top-level job ids of every workflow, parsed as text (no PyYAML: the
    pinned CI interpreter does not ship it, and neither should this gate
    depend on it). A job id is a 2-space-indented key directly under the
    top-level `jobs:` key."""
    found = set()
    for path in sorted(workflows_dir.glob("*.yml")):
        in_jobs = False
        for raw in path.read_text(encoding="utf-8").splitlines():
            if raw and not raw.startswith((" ", "#")):
                in_jobs = raw.rstrip() == "jobs:"
                continue
            if in_jobs:
                m = _JOB_LINE.match(raw)
                if m:
                    found.add(f"{path.name}:{m.group(1)}")
    return found


def scope_drift(workflows_dir: Path) -> tuple:
    jobs = workflow_jobs(workflows_dir)
    return sorted(jobs - set(JOB_SCOPE)), sorted(set(JOB_SCOPE) - jobs)


def prepush_mentions(workflows_dir: Path) -> list:
    hits = []
    for path in sorted(workflows_dir.glob("*.yml")):
        for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            if "prepush" in line.lower():
                hits.append(f"{path.name}:{n}: {line.strip()}")
    return hits


# ---------------------------------------------------------------------------
# Host-agnostic step actions.
# ---------------------------------------------------------------------------
def a_policy(ctx: Ctx):
    hits = prepush_mentions(ctx.clone / WORKFLOWS_DIR)
    for hit in hits:
        emit(f"::error::workflow references the local-only prepush gate: {hit}")
    emit(f"PREPUSH_POLICY_WORKFLOW_HITS={len(hits)}")
    return 1 if hits else 0


def a_scope(ctx: Ctx):
    unclassified, stale = scope_drift(ctx.clone / WORKFLOWS_DIR)
    for job in sorted(JOB_SCOPE):
        decision, reason = JOB_SCOPE[job]
        emit(f"PREPUSH_SCOPE {job} -> {decision}: {reason}")
    for job in unclassified:
        emit(f"::error::workflow job {job} is not classified in prepush.JOB_SCOPE -- classify it (included / host-specific / excluded) before pushing")
    for job in stale:
        emit(f"::error::prepush.JOB_SCOPE classifies {job}, which no workflow defines any more")
    emit(f"PREPUSH_SCOPE_JOBS={len(JOB_SCOPE)} unclassified={len(unclassified)} stale={len(stale)}")
    return 1 if (unclassified or stale) else 0


def docker_available() -> tuple:
    rc, out = capture(["docker", "info", "--format", "{{.ServerVersion}}"], cwd=REPO_ROOT)
    return rc == 0, out.strip()


def a_guards(ctx: Ctx):
    """build.yml guards-container. With a Docker daemon: the digest-pinned
    container form, exactly as CI runs it. Without one: `ci.py guards`'s host
    form -- the SAME roster through the SAME code path (guards.run_checks,
    which the container itself invokes via --in-container), minus the pinned
    interpreter/site-packages. The form that ran is always printed."""
    ok, detail = docker_available()
    if ok:
        emit(f"guards: docker-form (docker server {detail})")
        return ctx.run(ctx.py("native/scripts/ci.py", "guards", "--docker"))
    emit(f"guards: host-form (no docker on host) -- docker said: {detail[-200:]}")
    return ctx.run(ctx.py("native/scripts/ci.py", "guards"))


def a_selftest(ctx: Ctx):
    return ctx.run(ctx.py("native/scripts/ci.py", "selftest"))


def a_deps_pytest(ctx: Ctx):
    return ctx.run(ctx.py("-m", "pytest", "native/scripts/deps/", "-q"))


def a_d6_layer1(ctx: Ctx):
    return ctx.run(ctx.py("native/tests/run_dist_equivalence.py", "--layers", "l1",
                          "--report", str(ctx.clone / "d6-layer1-report.md")))


def _flutter() -> str:
    return shutil.which("flutter") or shutil.which("flutter.bat") or "flutter"


def _dart() -> str:
    return shutil.which("dart") or shutil.which("dart.bat") or "dart"


def a_dart_analyze(ctx: Ctx):
    app = ctx.clone / "app"
    rc = ctx.run([_flutter(), "pub", "get"], cwd=app)
    if rc != 0:
        return rc
    return ctx.run([_dart(), "analyze"], cwd=app)


def a_fetch_libraw(ctx: Ctx):
    return ctx.run(ctx.py("native/scripts/build_deps.py", "fetch", "libraw"))


def a_raw_provenance(ctx: Ctx):
    return ctx.run(ctx.py("native/scripts/verify_raw_provenance.py"))


def a_alias_table(ctx: Ctx):
    return ctx.run(ctx.py("native/scripts/check_alias_table_convention.py",
                          "native/third_party/libraw/src/metadata/normalize_model.cpp"))


def a_check_test_manifest(ctx: Ctx):
    return ctx.run(ctx.py("native/tests/check_test_manifest.py"))


def a_matrix_parsers(ctx: Ctx):
    return ctx.run(ctx.py("native/tests/test_decode_matrix_parsers.py"))


def a_flutter_test(subdir: str):
    def action(ctx: Ctx):
        where = ctx.clone / subdir
        rc = ctx.run([_flutter(), "pub", "get"], cwd=where)
        if rc != 0:
            return rc
        return ctx.run([_flutter(), "test"], cwd=where)
    return action


# ---------------------------------------------------------------------------
# Windows x86_64: toolchain environment + the windows_build.yml x86_64 row.
# ---------------------------------------------------------------------------
WIN_BUILD_DIR = "native/build-windows"
WIN_CODEC_EXPECT = [
    "--expect", "jpeg:encode=1", "--expect", "jpeg:decode=0", "--expect", "webp:encode=1",
    "--expect", "webp:decode=1", "--expect", "heic:encode=1", "--expect", "heic:decode=1",
    "--expect", "avif:encode=1", "--expect", "avif:decode=1", "--expect", "jxl:encode=1",
    "--expect", "jxl:decode=1",
]
WIN_BUILD_EXPECT = [
    "--expect-cap", "ICC=0", "--expect-cap", "OPENMP=1", "--expect-cap", "HEIF=1",
    "--expect-cap", "WEBP=1", "--expect-cap", "JXL=1", "--expect-cap", "RAW=1",
]


def _vswhere() -> Optional[Path]:
    base = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    p = Path(base) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    return p if p.is_file() else None


def windows_msvc_env() -> tuple:
    """The vcvars64 environment (what ilammy/msvc-dev-cmd exports in CI),
    plus the VS-bundled LLVM bin dir appended when clang-cl is not already on
    PATH (CI's runner image has a standalone LLVM; a developer box usually
    has the VS-bundled one). Returns (env or None, diagnostic)."""
    vswhere = _vswhere()
    if vswhere is None:
        return None, "vswhere.exe not found (Visual Studio Installer absent)"
    rc, out = capture([vswhere, "-latest", "-products", "*", "-requires",
                       "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                       "-property", "installationPath"], cwd=REPO_ROOT)
    install = out.strip().splitlines()[0].strip() if rc == 0 and out.strip() else ""
    if not install:
        return None, f"vswhere found no VS install with the x64 VC tools (rc={rc}): {out.strip()}"
    vcvars = Path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not vcvars.is_file():
        return None, f"{vcvars} missing"
    rc, out = capture(["cmd.exe", "/d", "/c", "call", str(vcvars), ">nul", "&&", "set"], cwd=REPO_ROOT)
    if rc != 0:
        return None, f"vcvars64.bat failed rc={rc}: {out[-500:]}"
    env = {}
    for line in out.splitlines():
        key, sep, value = line.partition("=")
        if sep and key and not key.startswith(" "):
            env[key] = value
    path_key = next((k for k in env if k.upper() == "PATH"), "PATH")
    parts = env.get(path_key, "").split(os.pathsep)
    # Git-for-Windows' MSYS dirs AFTER System32: their GNU `tar` parses
    # `C:\...` as a remote host spec and their `curl` differs from the
    # runner's. CI's pwsh steps resolve System32's bsdtar/curl first.
    msys = [p for p in parts if re.search(r"\\Git\\(usr|mingw64)\\bin", p, re.IGNORECASE)]
    parts = [p for p in parts if p not in msys] + msys
    if shutil.which("clang-cl", path=os.pathsep.join(parts)) is None:
        llvm = Path(install) / "VC" / "Tools" / "Llvm" / "x64" / "bin"
        if (llvm / "clang-cl.exe").is_file():
            parts.append(str(llvm))
    env[path_key] = os.pathsep.join(parts)
    env.update(CHILD_ENV_OVERRIDES)
    return env, f"vcvars64={vcvars}"


def _ci(ctx: Ctx, *args) -> int:
    return ctx.run(ctx.py("native/scripts/ci.py", *args))


def w_locate_clang_cl(ctx: Ctx):
    return _ci(ctx, "provision", "locate-clang-cl")


def w_verify_vulkan(ctx: Ctx):
    sdk = ctx.env.get("VULKAN_SDK") or os.environ.get("VULKAN_SDK", "")
    return _ci(ctx, "verify-vulkan-lib", "--vulkan-sdk", sdk)


def w_build_zlib(ctx: Ctx):
    return _ci(ctx, "build-zlib", "--version", "1.3.1", "--workspace", str(ctx.clone))


def w_configure(ctx: Ctx):
    zlib = (ctx.clone / "zlib-install").as_posix()
    argv = ["cmake", "-S", "native", "-B", WIN_BUILD_DIR, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl",
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
            "-DDNG_DIAGNOSTIC_BUILD=OFF", "-DDNG_VK_PIPELINE_CACHE=OFF",
            f"-DDNG_ZLIB_ROOT={zlib}", f"-DZLIB_ROOT={zlib}", "-DZLIB_USE_STATIC_LIBS=ON"]
    rc = stream(argv, ctx.clone, env=ctx.env, tee=ctx.clone / "configure.log")
    emit(f"CONFIGURE_RC={rc}")
    return rc


def w_assert_jxl_static(ctx: Ctx):
    log = ctx.clone / "configure.log"
    found = log.is_file() and "JXL: static" in log.read_text(encoding="utf-8", errors="replace")
    rc = 0 if found else 1
    emit(f"ASSERT JXL static-link RC={rc}")
    if rc:
        emit("::error::configure.log does not show the expected 'JXL: static' line from cmake/jxl.cmake")
    return rc


def w_build(ctx: Ctx):
    rc = ctx.run(["cmake", "--build", WIN_BUILD_DIR, "--target", "dng_decoder_native", "--", "-k", "0"])
    emit(f"BUILD_RC={rc}")
    return rc


ARCH = ["--arch", "x86_64"]


def w_ci(*args):
    return lambda ctx: _ci(ctx, *args)


WINDOWS_LEG = [
    ("win-locate-clang-cl", "Locate clang-cl", w_locate_clang_cl),
    ("win-verify-vulkan-lib", "Verify vulkan-1.lib is present", w_verify_vulkan),
    ("win-build-zlib", "Build zlib 1.3.1 (static, /MT)", w_build_zlib),
    ("win-configure", "Configure (Ninja + clang-cl, Vulkan AOT target)", w_configure),
    ("win-assert-jxl-static", "Assert JXL was statically linked (G1)", w_assert_jxl_static),
    ("win-build", "Build dng_decoder_native", w_build),
    ("win-assert-no-avx512", "Assert no AVX-512 in in-tree Windows code", w_ci("assert-no-avx512", "--platform", "windows", *ARCH)),
    ("win-verify-artifact", "Verify Windows artifact", w_ci("verify-artifact", "--platform", "windows", *ARCH)),
    ("win-assert-exports", "Assert required FFI exports (AC-W4)", w_ci("assert-exports", "--platform", "windows", *ARCH)),
    ("win-import-closure", "Assert Windows DLL dependency closure", w_ci("import-closure", "--platform", "windows", *ARCH)),
    ("win-assert-orientation", "Assert fused-orientation kernel signals (G-E)", w_ci("assert-orientation", "--platform", "windows", *ARCH)),
    ("win-capability-codec", "Assert full codec capability vector via probe (G1)",
     w_ci("capability-vector", "--platform", "windows", "--kind", "codec", *WIN_CODEC_EXPECT,
          "--json-out", "native/scripts/deps/probe/capability.json")),
    ("win-capability-build", "Assert build capability vector via probe (S-E2)",
     w_ci("capability-vector", "--platform", "windows", "--kind", "build", *WIN_BUILD_EXPECT,
          "--json-out", "native/scripts/deps/probe/capability_build.json")),
    ("win-codec-probe", "Compile + run functional capability probe (CI-T3)",
     lambda ctx: _ci(ctx, "codec-probe", "--platform", "windows", "--workspace", str(ctx.clone),
                     "--dist-dir", "native/third_party/heif-dist-windows")),
    ("win-stage", "Stage native artifacts", w_ci("stage", "--platform", "windows", *ARCH, "--artifact-dir", "artifacts", "--source-dir", WIN_BUILD_DIR)),
    ("win-assert-pe-machine", "Assert every staged DLL is this leg's PE machine type (AC-C2)",
     w_ci("assert-pe-machine", "--platform", "windows", *ARCH, "--artifact-dir", "artifacts")),
    ("win-min-runtime", "Measure and emit the minimum runtime floor (S-F1)", w_ci("min-runtime", "--platform", "windows", *ARCH)),
    ("win-assert-staged-group", "Assert the Windows shipped-file group is complete",
     w_ci("assert-staged-group", "--platform", "windows", *ARCH, "--artifact-dir", "artifacts")),
]


# ---------------------------------------------------------------------------
# Test layer.
# ---------------------------------------------------------------------------
def gate_runners(clone: Path) -> tuple:
    """(non-manual runner scripts, their executables) derived from
    native/tests/gates.py: GATES kind `runner:<file>` minus every script
    SCRIPTS declares `manual:`."""
    ns: dict = {}
    exec(compile((clone / "native/tests/gates.py").read_text(encoding="utf-8"), "gates.py", "exec"), ns)
    manual = {k for k, v in ns.get("SCRIPTS", {}).items() if v.startswith("manual:")}
    runners: dict = {}
    for exe, kind in ns["GATES"].items():
        if kind.startswith("runner:"):
            script = kind.split(":", 1)[1]
            if script not in manual:
                runners.setdefault(script, []).append(exe)
    return runners, sorted(manual)


def _exe(name: str) -> str:
    return name + (".exe" if os.name == "nt" else "")


# Items that SHOULD be portable but are not yet runnable on a host -- the
# platform-fork / blind-instrument inventory (lead ruling 2026-10-03 (b)).
# Key "target:<cmake target>" or "runner:<script>"; value (class, evidence).
# Every entry is RE-PROVEN each run: an unsupported target is still built in
# isolation and must still fail; an unsupported runner is still run and must
# still exit non-zero. One that passes is a STALE entry and turns the gate
# red -- remove it from this table so the item is gated again.
_WIN_DLL_INTERNALS = ("links dng_decoder_native and calls non-FFI internals; a Windows DLL exports only "
                      "FFI_EXPORT symbols (macOS/Linux shared libs export all), so lld-link reports undefined: ")
HOST_UNSUPPORTED: dict = {
    WINDOWS_X64: {
        "target:test_device_handoff": ("compile-error-posix-header",
            "native/tests/test_device_handoff.cpp:49 #include <unistd.h> -> clang-cl: 'unistd.h' file not found"),
        "target:test_raw_end_to_end": ("compile-error-posix-api",
            "native/tests/test_raw_end_to_end.cpp:325 setenv/unsetenv undeclared (not in the MSVC CRT)"),
        "target:test_raw_hardening": ("compile-error-posix-api",
            "native/tests/test_raw_hardening.cpp:327 setenv/unsetenv undeclared (not in the MSVC CRT)"),
        "target:test_libraw_adapter": ("link-error-dll-internals",
            _WIN_DLL_INTERNALS + "raw_invert_3x3, raw_bayer_filters_check_2x2, LibRawFrontendContext::* (19 symbols)"),
        "target:test_raw_sized_decode": ("link-error-dll-internals",
            _WIN_DLL_INTERNALS + "raw_pipeline_probe_output_size, raw_pipeline_decode_file_into (2 symbols)"),
        "target:test_raw_render_params": ("link-error-dll-internals",
            _WIN_DLL_INTERNALS + "raw_build_render_params, dng_render_params_for_test, raw_pcs_white (15 symbols)"),
        "target:test_stage4_oriented": ("metal-link",
            "references halide_metal_device_interface and dng_render_stage4_scaled_preavg (Metal-only AOT objects, 4 undefined)"),
        "runner:native/tests/run_decode_matrix.py": ("metal-pinned-baseline",
            "native/tests/kernel_regression_baselines.json SHA256 gates lossless_halide_stage3/stage4 pin Metal output bytes; "
            "Windows Vulkan output differs, so the runner exits at its first gate before any harness case"),
        "runner:native/tests/run_raw_matrix.py": ("metal-pinned-baseline",
            "its mandatory dng-regression case runs run_decode_matrix.py (metal-pinned-baseline above), and 5 of its 13 "
            "binaries are unbuildable here (entries above)"),
    },
}


# Not under artifacts/: run_decode_matrix.py wipes that directory on start.
REPROOF_DIR = Path("prepush-reproof")


def host_unsupported(host: str) -> dict:
    return HOST_UNSUPPORTED.get(host, {})


def _declare_host_skip(ctx: Ctx, item: str, reproof: str, log: Path) -> None:
    cls, evidence = host_unsupported(ctx.host)[item]
    emit(f"PREPUSH_HOST_SKIP({item}): class={cls} reproof={reproof} -- {evidence}")
    # The scratch clone is deleted after the run; the observed failure is
    # copied into the gate's own output so the evidence survives in the log.
    lines = [ln for ln in log.read_text(encoding="utf-8", errors="replace").splitlines() if ln.strip()]
    hits = [ln for ln in lines if re.search(r"error|FAIL|undefined symbol", ln)]
    for ln in (hits or lines)[:8]:
        emit(f"PREPUSH_HOST_SKIP_EVIDENCE({item}): {ln[:300]}")
    ctx.host_skips.append((item, cls, evidence))


def t_build_tests(ctx: Ctx):
    runners, _ = gate_runners(ctx.clone)
    build_dir = WIN_BUILD_DIR if ctx.host == WINDOWS_X64 else "native/build"
    rc, out = capture(["cmake", "--build", build_dir, "--target", "help"], cwd=ctx.clone, env=ctx.env or None)
    available = set(re.findall(r"^([A-Za-z0-9_]+): ", out, re.MULTILINE))
    wanted = sorted({exe for exes in runners.values() for exe in exes})
    unsupported = {t for t in wanted if f"target:{t}" in host_unsupported(ctx.host)}
    targets = [t for t in wanted if t in available and t not in unsupported]
    absent = [t for t in wanted if t not in available]
    for t in absent:
        emit(f"PREPUSH_TEST_TARGET_NOT_CONFIGURED({t}): this host's configure defines no such target (android cross-build target)")
    emit(f"PREPUSH_TEST_TARGETS wanted={len(wanted)} building={len(targets)} host_unsupported={len(unsupported)} "
         f"not_configured={len(absent)}")
    if rc != 0 or not targets:
        emit(f"::error::cannot enumerate test targets in {build_dir} (rc={rc})")
        return rc or 1
    rc = ctx.run(["cmake", "--build", build_dir, "--target", *targets, "--", "-k", "0"])
    stale = []
    for t in sorted(unsupported):
        probe_rc, probe_out = capture(["cmake", "--build", build_dir, "--target", t], cwd=ctx.clone, env=ctx.env or None)
        log = ctx.clone / REPROOF_DIR / f"{t}.build.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(probe_out, encoding="utf-8")
        if probe_rc == 0:
            stale.append(t)
            emit(f"::error::STALE host-unsupported entry target:{t}: it now BUILDS on {ctx.host}; "
                 f"remove it from prepush.HOST_UNSUPPORTED so it is gated")
        else:
            _declare_host_skip(ctx, f"target:{t}", f"isolated-build-rc={probe_rc} log={log.relative_to(ctx.clone).as_posix()}", log)
    return rc or (1 if stale else 0)


def _runner_step(script: str):
    def action(ctx: Ctx):
        build_dir = WIN_BUILD_DIR if ctx.host == WINDOWS_X64 else "native/build"
        args = RUNNER_ARGS.get(script, lambda c, b: [])(ctx, build_dir)
        if isinstance(args, tuple) and args and args[0] == SKIP:
            return args
        item = f"runner:{script}"
        if item not in host_unsupported(ctx.host):
            return ctx.run(ctx.py(script, *args))
        log = ctx.clone / REPROOF_DIR / f"{Path(script).stem}.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        rc, out = capture(ctx.py(script, *args), cwd=ctx.clone, env=ctx.env or None)
        log.write_text(out, encoding="utf-8")
        if rc == 0:
            emit(f"::error::STALE host-unsupported entry {item}: it now PASSES on {ctx.host}; "
                 f"remove it from prepush.HOST_UNSUPPORTED so it is gated")
            return 1
        _declare_host_skip(ctx, item, f"runner-rc={rc} log={log.relative_to(ctx.clone).as_posix()}", log)
        return (HOSTSKIP, item)
    return action


# run_decode_matrix.py's auto-enabled harnesses default to extensionless
# native/build/<name> paths. On hosts whose build dir or executable suffix
# differs, each BUILT harness is passed explicitly; an unbuilt one is left to
# the runner's own skip accounting (it records and counts it).
_MATRIX_HARNESS_FLAGS = {
    "--ffi-harness": "dng_ffi_harness",
    "--device-handoff-harness": "test_device_handoff",
    "--cfa-phase-harness": "test_cfa_phase",
    "--cfa-color-harness": "test_cfa_color",
    "--sized-decode-harness": "test_sized_decode",
    "--stage4-oriented-harness": "test_stage4_oriented",
    "--abi-layout-harness": "test_abi_layout",
    "--encode-harness": "ceyx_encode_harness",
}


def _decode_matrix_args(ctx: Ctx, build_dir: str):
    if ctx.host == MACOS_ARM64:
        return []
    args = ["--test-decode", f"{build_dir}/{_exe('test_decode')}"]
    for flag, name in _MATRIX_HARNESS_FLAGS.items():
        rel = f"{build_dir}/{_exe(name)}"
        if (ctx.clone / rel).is_file():
            args += [flag, rel]
    return args


def _raw_matrix_args(ctx: Ctx, build_dir: str):
    return ["--build-dir", build_dir]


def _dist_equivalence_args(ctx: Ctx, build_dir: str):
    if ctx.host != MACOS_ARM64:
        return (SKIP, "layers 2-3 compare the macOS carrier-built HEIF dist against the committed macOS dist "
                      "(`--platform` accepts macos or linux only); layer 1 runs on every host as step d6-layer1")
    return []


RUNNER_ARGS = {
    "native/tests/run_decode_matrix.py": _decode_matrix_args,
    "native/tests/run_raw_matrix.py": _raw_matrix_args,
    "native/tests/run_dist_equivalence.py": _dist_equivalence_args,
}


# ---------------------------------------------------------------------------
# The step roster.
# ---------------------------------------------------------------------------
def build_steps(clone: Path) -> list:
    steps = [
        Step("policy-no-prepush-in-workflows", "gate policy (user ruling 2026-10-03): prepush is local-only", ALL_HOSTS, a_policy),
        Step("scope-derivation", "every .github/workflows job classified in JOB_SCOPE", ALL_HOSTS, a_scope),
        Step("guards", "build.yml:guards-container `ci.py guards --docker` (host form when no Docker daemon)", ALL_HOSTS, a_guards),
        Step("selftest", "build.yml:verify-native-tests `ci.py selftest`", ALL_HOSTS, a_selftest),
        Step("deps-pytest", "windows_build.yml/linux_build.yml `python -m pytest native/scripts/deps/ -q`", ALL_HOSTS, a_deps_pytest),
        Step("d6-layer1", "macos_build.yml:build `run_dist_equivalence.py --layers l1` (pure argv check, any host)", ALL_HOSTS, a_d6_layer1),
        Step("dart-analyze", "build.yml:verify-dart `flutter pub get` + `dart analyze` (app/)", ALL_HOSTS, a_dart_analyze),
        Step("fetch-libraw", "all build legs `build_deps.py fetch libraw`", ALL_HOSTS, a_fetch_libraw),
        Step("guard-raw-provenance", "build.yml:verify-native-tests `verify_raw_provenance.py`", ALL_HOSTS, a_raw_provenance),
        Step("guard-alias-table", "build.yml:verify-native-tests `check_alias_table_convention.py`", ALL_HOSTS, a_alias_table),
    ]
    for name, wf_step, action in WINDOWS_LEG:
        steps.append(Step(name, f"windows_build.yml:build-windows (x86_64 row) '{wf_step}'",
                          frozenset({WINDOWS_X64}), action,
                          skip_reason="windows_build.yml x86_64 row: needs a Windows x86_64 host"))
    steps += [
        Step("win-arm64-row", "windows_build.yml:build-windows (arm64 row)", frozenset(), None,
             skip_reason="needs a Windows ARM64 host: its LoadLibrary capability probes execute arm64 code"),
        Step("macos-leg", "macos_build.yml:build (arm64 native + x86_64 cross)", frozenset({MACOS_ARM64}), None,
             skip_reason="needs a macOS arm64 host (Metal AOT, Mach-O assertions)"),
        Step("linux-leg", "linux_build.yml:build-linux", frozenset(), None,
             skip_reason="runs inside the ubuntu:22.04 glibc-floor container with apt/vcpkg provisioning; not ported to a local host"),
        Step("android-leg", "android_build.yml:build-android", frozenset(), None,
             skip_reason="ubuntu runner + apt + NDK cross-build; not ported to a local host"),
    ]
    # Test layer.
    steps += [
        Step("test-manifest", "native/tests/check_test_manifest.py (every test executable classified)", ALL_HOSTS, a_check_test_manifest, group="test"),
        Step("test-matrix-parsers", "native/tests/test_decode_matrix_parsers.py (canned-stdout self-check)", ALL_HOSTS, a_matrix_parsers, group="test"),
        Step("test-build-targets", "build every non-manual gates.py runner executable this host configures",
             frozenset({WINDOWS_X64, MACOS_ARM64}), t_build_tests,
             skip_reason="no native build leg implemented for this host", group="test"),
    ]
    try:
        runners, manual = gate_runners(clone)
    except (OSError, KeyError, SyntaxError) as exc:
        runners, manual = {}, []
        emit(f"::error::cannot read native/tests/gates.py: {exc}")
    for script in sorted(runners):
        steps.append(Step(f"test-{Path(script).stem.replace('_', '-')}",
                          f"gates.py runner {script} (executables: {', '.join(runners[script])})",
                          frozenset({WINDOWS_X64, MACOS_ARM64}), _runner_step(script),
                          skip_reason="no native build leg implemented for this host", group="test"))
    for script in manual:
        steps.append(Step(f"test-{Path(script).stem.replace('_', '-')}", f"gates.py SCRIPTS {script}",
                          frozenset(), None, skip_reason="declared manual: in native/tests/gates.py SCRIPTS (not an automated gate)",
                          group="test"))
    steps += [
        Step("test-flutter-plugin", "plugin/ `flutter test`", ALL_HOSTS, a_flutter_test("plugin"), group="test"),
        Step("test-flutter-app", "app/ `flutter test`", ALL_HOSTS, a_flutter_test("app"), group="test"),
    ]
    return steps


# ---------------------------------------------------------------------------
# Inner run (inside the clone).
# ---------------------------------------------------------------------------
def _needs_msvc(step: Step) -> bool:
    """Steps that compile, link, or run toolchain binaries (dumpbin,
    llvm-readobj, clang-cl) -- CI runs all of them after msvc-dev-cmd."""
    return WINDOWS_X64 in step.hosts and (step.name.startswith("win-") or step.name.startswith("test-build"))


def run_inner(args) -> int:
    clone = REPO_ROOT
    host = host_key()
    steps = build_steps(clone)
    names = [s.name for s in steps]
    unknown = [n for n in (args.step or []) + (args.skip_step or []) if n not in names]
    if unknown:
        emit(f"::error::unknown step name(s) {unknown}; known: {names}")
        return 2
    selected = [s for s in steps if not args.step or s.name in args.step]

    ctx = Ctx(clone=clone, host=host)
    results: list = []  # (name, status, rc)
    for item in args.bootstrap_result or []:
        name, _, rc = item.partition("=")
        results.append((name, "PASS" if rc == "0" else "FAIL", int(rc)))
    for name in args.skip_step or []:
        emit(f"PREPUSH_SKIP({name}): skipped on request (--skip-step); this run is NOT a full gate")
        results.append((name, "SKIP", None))
    selected = [s for s in selected if s.name not in (args.skip_step or [])]
    partial = bool(args.step or args.skip_step)

    if host == WINDOWS_X64 and any(_needs_msvc(s) for s in selected):
        env, diag = windows_msvc_env()
        emit(f"PREPUSH_MSVC_ENV: {diag}")
        if env is None:
            emit(f"::error::MSVC developer environment unavailable: {diag}")
            results.append(("precondition-msvc-env", "FAIL", 1))
        else:
            ctx.env = env

    for step in selected:
        emit("")
        emit(f"==== PREPUSH STEP {step.name} [{step.group}] ====")
        emit(f"PREPUSH_STEP_DERIVATION({step.name}): {step.derivation}")
        if host not in step.hosts:
            emit(f"PREPUSH_SKIP({step.name}): host={host} -- {step.skip_reason}")
            results.append((step.name, "SKIP", None))
            continue
        if step.action is None:
            emit(f"::error::PREPUSH_UNIMPLEMENTED({step.name}): this step must run on host={host} but the gate has no implementation for it yet")
            results.append((step.name, "FAIL", 3))
            emit(f"PREPUSH_STEP_RC({step.name})=3")
            continue
        if host == WINDOWS_X64 and not ctx.env and _needs_msvc(step):
            emit(f"PREPUSH_STEP_RC({step.name})=1  (no MSVC developer environment)")
            results.append((step.name, "FAIL", 1))
            continue
        emit(f"PREPUSH_STEP_BEGIN({step.name})")
        started = time.monotonic()
        try:
            outcome = step.action(ctx)
        except Exception as exc:  # a crash in the gate is a red step, never a pass
            emit(f"::error::step {step.name} raised {type(exc).__name__}: {exc}")
            outcome = 4
        elapsed = time.monotonic() - started
        if isinstance(outcome, tuple) and outcome and outcome[0] == SKIP:
            emit(f"PREPUSH_SKIP({step.name}): host={host} -- {outcome[1]}")
            results.append((step.name, "SKIP", None))
            continue
        if isinstance(outcome, tuple) and outcome and outcome[0] == HOSTSKIP:
            emit(f"PREPUSH_STEP_HOSTSKIP({step.name}): {outcome[1]} (see PREPUSH_HOST_UNSUPPORTED inventory)")
            results.append((f"host-unsupported:{outcome[1]}", HOSTSKIP, None))
            continue
        rc = int(outcome)
        emit(f"PREPUSH_STEP_RC({step.name})={rc}")
        emit(f"PREPUSH_STEP_SECONDS({step.name})={elapsed:.1f}")
        results.append((step.name, "PASS" if rc == 0 else "FAIL", rc))
        for item, _, _ in ctx.host_skips:
            if item.startswith("target:") and (f"host-unsupported:{item}", HOSTSKIP, None) not in results:
                results.append((f"host-unsupported:{item}", HOSTSKIP, None))

    emit_inventory(ctx)
    return summarize(results, host, args.head or "unknown", partial)


def emit_inventory(ctx: Ctx) -> None:
    """Machine-readable host-unsupported inventory: one tab-separated line
    per item (host, item, class, evidence) -- plan input for the
    unification campaign."""
    emit("")
    emit(f"==== PREPUSH HOST-UNSUPPORTED INVENTORY host={ctx.host} count={len(ctx.host_skips)} ====")
    for item, cls, evidence in ctx.host_skips:
        emit(f"PREPUSH_HOST_UNSUPPORTED\t{ctx.host}\t{item}\t{cls}\t{evidence}")
    emit("==== END INVENTORY ====")


def summarize(results: list, host: str, head: str, partial: bool = False) -> int:
    emit("")
    emit("==== PREPUSH RESULTS ====")
    for name, status, rc in results:
        emit(f"PREPUSH_RESULT {status:8} {name}" + ("" if rc is None else f" rc={rc}"))
    passed = sum(1 for _, s, _ in results if s == "PASS")
    failed = [n for n, s, _ in results if s == "FAIL"]
    skipped = sum(1 for _, s, _ in results if s == "SKIP")
    skipped_host = sum(1 for _, s, _ in results if s == HOSTSKIP)
    emit(f"PREPUSH-SUMMARY host={host} head={head} total={len(results)} passed={passed} "
         f"failed={len(failed)} skipped={skipped} skipped_host={skipped_host} partial={int(partial)}"
         + (f" failed_steps={','.join(failed)}" if failed else ""))
    return 1 if failed else 0


# ---------------------------------------------------------------------------
# Outer run (in the working tree): clone, seed, hand off.
# ---------------------------------------------------------------------------
def tree_manifest(root: Path) -> dict:
    out = {}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            h = hashlib.sha256()
            with p.open("rb") as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
            out[p.relative_to(root).as_posix()] = (p.stat().st_size, h.hexdigest())
    return out


def seed(name: str, src: Path, dst: Path) -> int:
    emit(f"PREPUSH_SEED({name}): {src} -> {dst}")
    if not src.is_dir():
        emit(f"::error::seed source {src} does not exist in the working tree -- fetch it there first "
             f"({'python3 native/scripts/build_deps.py fetch halide' if name == 'halide' else 'owner-supplied fixtures'})")
        return 1
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst, symlinks=False)
    a, b = tree_manifest(src), tree_manifest(dst)
    files, size = len(b), sum(s for s, _ in b.values())
    mismatched = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
    emit(f"PREPUSH_SEED_VERIFY({name}): files={files} bytes={size} sha256_mismatches={len(mismatched)}")
    for k in mismatched[:20]:
        emit(f"::error::seed {name}: {k} differs after copy")
    return 1 if mismatched or files == 0 else 0


# Samples the test layer reads by default path (run_decode_matrix.py
# _DEFAULT_RAW_FFI_SAMPLE / _DEFAULT_BGGR_SAMPLE; plugin/test/support/
# native_fixtures.dart), on top of the sha256-locked fixtures in
# kernel_regression_baselines.json. A missing one turns the gate red.
_DEFAULT_SAMPLES = ("image_samples/raw_sample.arw", "image_samples/bayer_conc_a.dng",
                    "image_samples/lossless_dng_sample.dng")


def samples_ok(clone: Path) -> int:
    import json

    baselines = json.loads((clone / "native/tests/kernel_regression_baselines.json").read_text(encoding="utf-8"))
    bad = 0
    for name, info in sorted((baselines.get("fixtures") or {}).items()):
        path = clone / info.get("path", "")
        actual = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else "MISSING"
        ok = actual == info.get("sha256")
        bad += not ok
        emit(f"PREPUSH_SAMPLE_LOCKED({name}) path={info.get('path')} ok={int(ok)}" + ("" if ok else f" actual={actual}"))
    for rel in _DEFAULT_SAMPLES:
        ok = (clone / rel).is_file()
        bad += not ok
        emit(f"PREPUSH_SAMPLE_DEFAULT path={rel} present={int(ok)}")
    if not baselines.get("fixtures"):
        emit("::error::kernel_regression_baselines.json locks no fixtures")
        bad += 1
    if bad:
        emit(f"::error::{bad} required test sample(s) missing or hash-mismatched in the clone -- provision image_samples/ in the working tree")
    return 1 if bad else 0


def halide_version_ok(dst: Path) -> int:
    sys.path.insert(0, str(REPO_ROOT / "native" / "scripts" / "deps"))
    import fetch_halide  # type: ignore

    machine = "x86_64" if host_key() == WINDOWS_X64 else platform_module.machine()
    _, _, asset = fetch_halide.resolve_asset(platform_module.system(), machine)
    text = (dst / "VERSION").read_text(encoding="utf-8") if (dst / "VERSION").is_file() else ""
    # The Windows zip lays the import library out as lib/Release/Halide.lib,
    # which fetch_halide.already_present() (lib/Halide.lib) does not see.
    libs = ("lib/libHalide.a", "lib/Halide.lib", "lib/Release/Halide.lib")
    ok = f"asset: {asset}" in text and any((dst / lib).is_file() for lib in libs)
    emit(f"PREPUSH_SEED_HALIDE_ASSET expected={asset} matches={int(ok)}")
    if not ok:
        emit("::error::the working tree's Halide dist is not the pinned asset for this host; re-fetch it there")
    return 0 if ok else 1


def run_outer(args) -> int:
    host = host_key()
    rc, head = capture(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT)
    head = head.strip() if rc == 0 else "unknown"
    emit(f"PREPUSH_HOST={host}")
    emit(f"PREPUSH_HEAD={head}")
    emit(f"PREPUSH_PYTHON={sys.executable} {platform_module.python_version()}")
    emit("PREPUSH_CHILD_ENV: " + " ".join(f"{k}={v}" for k, v in CHILD_ENV_OVERRIDES.items())
         + " (CI runners use a UTF-8 locale; set for every child)")
    results: list = []

    scratch = Path(args.scratch).resolve() if args.scratch else Path(tempfile.mkdtemp(prefix="ceyx-prepush-"))
    clone = scratch / "ceyx"
    if clone.exists():
        emit(f"::error::{clone} already exists; pass an empty --scratch")
        return summarize([("bootstrap-clone", "FAIL", 1)], host, head)
    scratch.mkdir(parents=True, exist_ok=True)
    emit(f"PREPUSH_SCRATCH={scratch}")

    # LF on checkout, as windows_build.yml's first step forces before checkout.
    rc = stream(["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf", "clone", "--no-hardlinks",
                 str(REPO_ROOT), str(clone)], cwd=scratch)
    if rc == 0:
        crc, chead = capture(["git", "rev-parse", "HEAD"], cwd=clone)
        if chead.strip() != head:
            emit(f"::error::clone HEAD {chead.strip()} != working tree HEAD {head} (detached/odd branch state?)")
            rc = 1
        cfg = stream(["git", "config", "core.autocrlf", "false"], cwd=clone)
        rc = rc or cfg
    emit(f"PREPUSH_STEP_RC(bootstrap-clone)={rc}")
    results.append(("bootstrap-clone", "PASS" if rc == 0 else "FAIL", rc))
    if rc != 0:
        return summarize(results, host, head)

    for name, rel in SEEDS.items():
        rc = seed(name, REPO_ROOT / rel, clone / rel)
        if name == "halide" and rc == 0:
            rc = halide_version_ok(clone / rel)
        if name == "samples":
            rc = samples_ok(clone) or rc
        emit(f"PREPUSH_STEP_RC(bootstrap-seed-{name})={rc}")
        results.append((f"bootstrap-seed-{name}", "PASS" if rc == 0 else "FAIL", rc))

    inner = [sys.executable, str(clone / "native/scripts/ci.py"), "prepush", "--inner", "--head", head]
    for name, status, rc in results:
        inner += ["--bootstrap-result", f"{name}={rc}"]
    for s in args.step or []:
        inner += ["--step", s]
    for s in args.skip_step or []:
        inner += ["--skip-step", s]
    rc = stream(inner, cwd=clone)
    emit(f"PREPUSH_INNER_RC={rc}")

    if args.keep_scratch:
        emit(f"PREPUSH_SCRATCH_KEPT={scratch}")
    else:
        shutil.rmtree(scratch, onerror=_force_remove)
        emit(f"PREPUSH_SCRATCH_REMOVED={scratch} exists_after={int(scratch.exists())}")
    return rc


def _force_remove(func, path, _exc):
    os.chmod(path, 0o700)
    func(path)


def list_steps() -> int:
    for step in build_steps(REPO_ROOT):
        hosts = ",".join(sorted(step.hosts)) or "none"
        emit(f"{step.name:34} [{step.group}] hosts={hosts} :: {step.derivation}")
    return 0


class _Tee:
    def __init__(self, *streams):
        self.streams = streams

    def write(self, data):
        for s in self.streams:
            s.write(data)
        return len(data)

    def flush(self):
        for s in self.streams:
            s.flush()


def add_arguments(p: argparse.ArgumentParser) -> None:
    p.add_argument("--step", action="append", help="run only this step (repeatable); see --list")
    p.add_argument("--skip-step", action="append",
                   help="skip this step (repeatable); counted as SKIP and marks the run partial=1")
    p.add_argument("--list", action="store_true", help="print the step roster and exit")
    p.add_argument("--log", default=None, help="also write the full output to this artifact file")
    p.add_argument("--scratch", default=None, help="empty directory for the fresh clone (default: a new temp dir)")
    p.add_argument("--keep-scratch", action="store_true", help="keep the scratch clone after the run")
    p.add_argument("--inner", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--head", default=None, help=argparse.SUPPRESS)
    p.add_argument("--bootstrap-result", action="append", help=argparse.SUPPRESS)


def main(args) -> int:
    # Child output is UTF-8 (CHILD_ENV_OVERRIDES); a legacy-code-page console
    # or redirect (cp950) cannot encode all of it and would kill the gate
    # mid-run, orphaning the step it was streaming.
    for s in (sys.stdout, sys.stderr):
        if hasattr(s, "reconfigure"):
            s.reconfigure(encoding="utf-8", errors="replace")
    if args.list:
        return list_steps()
    if args.inner:
        return run_inner(args)
    if args.log:
        log_path = Path(args.log)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("w", encoding="utf-8") as fh:
            real = sys.stdout
            sys.stdout = _Tee(real, fh)
            try:
                rc = run_outer(args)
                emit(f"PREPUSH_EXIT_RC={rc}")
            finally:
                sys.stdout = real
        return rc
    rc = run_outer(args)
    emit(f"PREPUSH_EXIT_RC={rc}")
    return rc
