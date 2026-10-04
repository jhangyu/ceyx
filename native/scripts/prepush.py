#!/usr/bin/env python3
"""`ci.py prepush` -- the single merged pre-push gate (user ruling 2026-10-03).

ONE command, run from a working tree, that:

  1. clones the LOCAL repo's committed HEAD into a fresh scratch directory
     (uncommitted edits are invisible to it by construction -- the gate
     judges what a push would publish, not the working tree);
  2. seeds the clone with the gitignored inputs a fresh clone lacks (the
     Halide v21 binary dist, the image_samples/ test fixtures, and on macOS
     hosts the shipped plugin dylib), COPIED from this working tree and
     verified file-by-file (count, bytes, sha256) -- never re-downloaded;
  3. hands off to the CLONE's own copy of this module (`--inner`), which runs
     the CI-equivalent steps and the repo's automated test suites.

Local red = no push. Remote CI stays compile-only; this module is a LOCAL
gate and must never be referenced by a workflow (`policy-no-prepush-in-
workflows` fails if one does).

STEPS COME FROM THE WORKFLOW FILES (prepush_workflow.py parses them):
  * every job of every workflow is classified in `JOB_SCOPE`;
  * every STEP of every mirrored job is classified in `STEP_SCOPE`
    (`derive` / `impl:` / `seed:` / `covered:` / `provision:` / `ci-only`);
    `scope-derivation` re-parses the workflows each run and FAILS on any
    unclassified or stale job or step name, on any `if:` it cannot evaluate,
    and on a `derive` step whose body is no longer derivable;
  * a `derive` step runs the commands parsed out of its own `run:` body,
    with `${{ matrix.* }}` / `${{ github.workspace }}` / `$VAR` substituted
    and GITHUB_ENV / GITHUB_PATH honoured -- the gate holds no copy of them.

A HOST'S OWN PLATFORM LEG MUST RUN. Each matrix row maps to the local host
that can run it; on that host its steps run, and a leg with no
implementation there (the Linux leg on a Linux host) is a FAILURE. Rows
owned by another host are one counted `skipped` entry per row.
The macOS leg is verified on macOS host at 6b35c47f (2026-10-04, docs/logs/2026-10-04/ceyx-prepush-6b35c47f-090909.log).

COUNTERS, all printed in the one PREPUSH-SUMMARY line:
  passed / failed  -- executed steps;
  skipped          -- another host's leg/row, or declared `manual:` in gates.py;
  skipped_host     -- HOST_UNSUPPORTED items: should be portable, not yet
                      runnable here. Each carries an evidence class and a
                      failure SIGNATURE and is re-proven every run: the item
                      is still built/run and must fail with exactly its
                      declared signature. Passing = stale entry = RED; any
                      other failure = RED. Inventory block at the end of the
                      log (tab-separated PREPUSH_HOST_UNSUPPORTED lines);
  known_defect     -- real defects quarantined to the campaign by lead ruling,
                      signature-bound exactly like skipped_host entries
                      (`kind="known-defect"`);
  flaky_known      -- single tests quarantined as flaky (FLAKY_TESTS), never
                      retried; any other failing test stays red;
  skipped_inner    -- test-level skips INSIDE suites that ran (Dart `skip:`,
                      runner-declared per-sample skips), each listed by name;
  ci_only / covered -- workflow steps with no local meaning (upload,
                      cache...) or satisfied by the bootstrap (clone, seed).

GUARDS: with a Docker daemon, the digest-pinned container form; without
one, `ci.py guards`'s host form (same roster), printed as
`guards: host-form (no docker on host)`.

Every child process gets PYTHONUTF8=1 (CI runners use a UTF-8 locale).

Usage (from a working tree):
    python3 native/scripts/ci.py prepush [--log FILE] [--scratch DIR] [--keep-scratch]
                                         [--step NAME ...] [--skip-step NAME ...]
    python3 native/scripts/ci.py prepush --list
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import platform as platform_module
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import prepush_workflow as wf  # noqa: E402

CHILD_ENV_OVERRIDES = {"PYTHONUTF8": "1", "PYTHONIOENCODING": "utf-8"}
WORKFLOWS_DIR = Path(".github") / "workflows"

WINDOWS_X64 = "windows-x86_64"
MACOS_ARM64 = "macos-arm64"
LINUX_X64 = "linux-x86_64"
ALL_HOSTS = frozenset({WINDOWS_X64, MACOS_ARM64, LINUX_X64})

# Gitignored inputs copied into the clone. The Halide seed stands in for
# every build leg's `build_deps.py fetch halide` step (classified `seed:`);
# image_samples/ is the corpus the runners hash-verify themselves.
SEEDS = {
    "halide": Path("native/third_party/halide"),
    "samples": Path("image_samples"),
}
# The plugin's dylib-fixture suites load the SHIPPED macOS dylib
# (plugin/test/support/native_fixtures.dart shippedDylibPath), which `*.dylib`
# in .gitignore keeps out of every clone. Seed verified on macOS host at 6b35c47f (2026-10-04, docs/logs/2026-10-04/ceyx-prepush-6b35c47f-090909.log).
# It also loads the @rpath companions (@loader_path), i.e. the whole vendored directory the podspec ships
# (`vendored_libraries = 'Libraries/*'`), so the whole directory is seeded.
MACOS_LIBRARIES = Path("plugin/macos/Libraries")
HOST_SEEDS = {MACOS_ARM64: {"macos-dylib": MACOS_LIBRARIES}}
# Built libjxl dists, seeded only when the working tree holds one (its `.pins`
# stamp exists). The `Fetch vendored libjxl distribution` step still runs and
# fetch_libjxl.py's stamp (tag, commit, arch, submodules, sha256 of the script
# = CI's actions/cache key) decides skip vs. source build, exactly like a CI
# cache hit/miss. Absent = cold source build, as before.
OPTIONAL_SEEDS = {
    "libjxl": Path("native/third_party/libjxl-dist"),
    "libjxl-x86_64": Path("native/third_party/libjxl-dist-x86_64"),
}


# ---------------------------------------------------------------------------
# Output and process primitives.
# ---------------------------------------------------------------------------
def emit(line: str = "") -> None:
    print(line, flush=True)


def host_key() -> str:
    system = platform_module.system()
    machine = platform_module.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "x86_64" if machine in ("amd64", "x86_64") else machine
    return {"Windows": "windows", "Darwin": "macos"}.get(system, "linux") + f"-{arch}"


def py_exe() -> str:
    """Interpreter for python children. On Windows the `pythonw` sibling
    (user decree 2026-10-03): no console, so a console-close / Ctrl+C
    broadcast (0xC000013A) cannot kill the gate's python processes. Output
    still flows through the pipes the gate reads."""
    if os.name == "nt":
        pythonw = Path(sys.executable).with_name("pythonw.exe")
        if pythonw.is_file():
            return str(pythonw)
    return sys.executable


# Console children (cmake, ninja, cmd for .bat) of a console-less gate get a
# console of their own unless told not to; CREATE_NO_WINDOW keeps them off
# every console, so no console-close signal can reach them either.
_NO_WINDOW = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}


def child_env(extra: Optional[dict] = None) -> dict:
    env = dict(os.environ)
    env.update(CHILD_ENV_OVERRIDES)
    env.update(extra or {})
    return env


def stream(argv, cwd, env=None, tee: Optional[Path] = None) -> int:
    """Run argv (list, shell=False), forwarding combined output line by line
    to our stdout (and `tee`), and return ITS returncode. A missing
    executable is rc 127 with the OSError text, never an exception."""
    argv = [os.fspath(a) for a in argv]
    emit(f"PREPUSH_EXEC: {' '.join(argv)}  (cwd={cwd})")
    try:
        proc = subprocess.Popen(argv, shell=False, cwd=os.fspath(cwd),
                                env=env if env is not None else child_env(),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                text=True, encoding="utf-8", errors="replace", **_NO_WINDOW)
    except OSError as exc:
        emit(f"PREPUSH_EXEC_ERROR: {exc}")
        return 127
    assert proc.stdout is not None
    sink = None
    if tee is not None:
        tee.parent.mkdir(parents=True, exist_ok=True)
        sink = tee.open("w", encoding="utf-8")
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
        done = subprocess.run([os.fspath(a) for a in argv], shell=False, cwd=os.fspath(cwd),
                              env=env if env is not None else child_env(), capture_output=True,
                              text=True, encoding="utf-8", errors="replace", stdin=subprocess.DEVNULL, **_NO_WINDOW)
    except OSError as exc:
        return 127, f"{exc}\n"
    return done.returncode, (done.stdout or "") + (done.stderr or "")


# ---------------------------------------------------------------------------
# Step model.
# ---------------------------------------------------------------------------
SKIP, HOSTSKIP, CIONLY, COVERED = "SKIP", "HOSTSKIP", "CI_ONLY", "COVERED"
KNOWNDEFECT, FLAKY = "KNOWN_DEFECT", "FLAKY_KNOWN"
ROW_PREFIX = {HOSTSKIP: "host-unsupported", KNOWNDEFECT: "known-defect", FLAKY: "flaky-known"}


@dataclass
class Ctx:
    clone: Path
    host: str
    env: dict = field(default_factory=dict)  # host toolchain env (Windows: vcvars)
    host_skips: list = field(default_factory=list)  # (item, class, evidence)
    inner_skips: list = field(default_factory=list)  # (where, name)
    jobrows: dict = field(default_factory=dict)  # (job, row) -> JobRow

    def base_env(self) -> dict:
        return dict(self.env) if self.env else child_env()

    def py(self, *args) -> list:
        return [py_exe(), *args]

    def run(self, argv, cwd: Optional[Path] = None) -> int:
        return stream(argv, cwd or self.clone, env=self.base_env())


@dataclass
class Step:
    name: str
    derivation: str
    hosts: frozenset
    action: Optional[Callable]  # returns int rc, (SKIP|HOSTSKIP, text)
    skip_reason: str = ""
    group: str = "ci"
    status: Optional[str] = None  # CIONLY/COVERED: printed + counted, not executed


# ---------------------------------------------------------------------------
# Job classification.
# ---------------------------------------------------------------------------
# "mirrored"  -- its steps are classified in STEP_SCOPE and run from it;
# "caller"    -- a `uses:` caller of a classified reusable workflow;
# "unported"  -- a leg the gate does not implement: FAILS on the host that
#                owns it (UNPORTED_OWNER), counted skip elsewhere;
# "excluded"  -- never part of the gate; reason says why.
JOB_SCOPE: dict = {
    "build.yml:macos": ("caller", "calls macos_build.yml"),
    "build.yml:linux": ("caller", "calls linux_build.yml"),
    "build.yml:windows": ("caller", "calls windows_build.yml"),
    "build.yml:android": ("caller", "calls android_build.yml"),
    "build.yml:heif-dist-windows": ("excluded", "dist leg: CI runs it only on v* tags / run_dists dispatch, never on a push; its output is a committed, pinned input"),
    "build.yml:jxl-dist-windows": ("excluded", "dist leg: tag / run_dists only, committed pinned input"),
    "build.yml:webp-dist-windows": ("excluded", "dist leg: tag / run_dists only, committed pinned input"),
    "build.yml:libomp-dist-windows": ("excluded", "dist leg: tag / run_dists only, committed pinned input"),
    "build.yml:heif-dist-android": ("excluded", "dist leg: tag / run_dists only, committed pinned input"),
    "build.yml:jxl-dist-android": ("excluded", "dist leg: tag / run_dists only, committed pinned input"),
    "build.yml:webp-dist-android": ("excluded", "dist leg: tag / run_dists only, committed pinned input"),
    "build.yml:verify-dart": ("mirrored", "flutter pub get + dart analyze"),
    "build.yml:verify-native-tests": ("mirrored", "selftest, vendored-LibRaw guards, macOS test-target build"),
    "build.yml:guards-container": ("mirrored", "repo-static guard block"),
    "build.yml:all-platforms-green": ("excluded", "aggregator over other jobs' results (needs.*.result); no command of its own"),
    "build.yml:publish": ("excluded", "release publication (downloaded run artifacts + GitHub token); not a verification step"),
    "android_build.yml:build-android": ("unported", "ubuntu runner + apt toolchain + NDK cross-build"),
    "linux_build.yml:build-linux": ("unported", "ubuntu:22.04 glibc-floor container + apt/vcpkg provisioning"),
    "macos_build.yml:build": ("mirrored", "macOS arm64 native + x86_64 cross rows"),
    "windows_build.yml:build-windows": ("mirrored", "x86_64 row (Windows x86_64 host); arm64 row needs an ARM64 host"),
    "heif_dist_android.yml:build-heif-dist": ("excluded", "dist workflow; committed pinned input"),
    "heif_dist_windows.yml:build-heif-dist": ("excluded", "dist workflow; committed pinned input"),
    "jxl_dist_android.yml:build-jxl-dist": ("excluded", "dist workflow; committed pinned input"),
    "jxl_dist_windows.yml:build-jxl-dist": ("excluded", "dist workflow; committed pinned input"),
    "libomp_dist_windows.yml:build-libomp-dist": ("excluded", "dist workflow; committed pinned input"),
    "webp_dist_android.yml:build-webp-dist": ("excluded", "dist workflow; committed pinned input"),
    "webp_dist_windows.yml:build-webp-dist": ("excluded", "dist workflow; committed pinned input"),
}

# Unported legs: (alias, host that owns it or None). Android has no local
# host platform: its leg cross-compiles on an ubuntu runner for devices.
UNPORTED_OWNER = {
    "linux_build.yml:build-linux": ("linux", LINUX_X64),
    "android_build.yml:build-android": ("android", None),
}

# Mirrored jobs in execution order: (job key, alias, row key -> owning host).
# Row key is the row's `arch_tag` ("" for a job without a matrix).
MIRRORED = (
    ("build.yml:guards-container", "guards", {"": LINUX_X64}),
    ("build.yml:verify-native-tests", "native-tests", {"": MACOS_ARM64}),
    ("build.yml:verify-dart", "dart", {"": MACOS_ARM64}),
    ("windows_build.yml:build-windows", "windows", {"x86_64": WINDOWS_X64, "arm64": None}),
    ("macos_build.yml:build", "macos", {"arm64": MACOS_ARM64, "x86_64": MACOS_ARM64}),
)
# Job rows that CI runs on a machine of their own and the gate runs in a
# checkout of their own (every host), each CONCURRENTLY with the main lane:
# (job key, row arch_tag) -> seeds carried over from the clone (SEEDS: always;
# OPTIONAL_SEEDS: when the clone holds one). Only the rows whose CI job fetches
# Halide get it, as on CI.
# native-tests' default build dir (native/build) collides with the macos arm64
# row's; the x86_64 row's outputs are disjoint from arm64's, and nothing in the
# test-* steps reads them, so test-* only waits for the host's own build row
# (main lane). guards/dart read the tree only; their own checkout keeps them
# off the main lane's build tree (Windows/Linux hosts: their whole gain).
OWN_WORKSPACE = {
    ("build.yml:guards-container", ""): (),
    ("build.yml:verify-native-tests", ""): ("halide",),
    ("build.yml:verify-dart", ""): (),
    ("macos_build.yml:build", "x86_64"): ("halide", "libjxl-x86_64"),
}

ANY, OWN = "any", "own"
CI = ("ci-only", OWN)

# Every step of every mirrored job: workflow step name -> (kind, where, note).
# where=any: host-agnostic, runs once on every host; where=own: runs on the
# host owning the row (MIRRORED). The note is printed with the step.
STEP_SCOPE: dict = {
    "build.yml:guards-container": {
        "Checkout": (*CI, "the bootstrap clone is the checkout"),
        "Set up Python": (*CI, "host interpreter; its version is printed"),
        "Run repo-static guards in the digest-pinned container": ("impl:guards", ANY, "docker form, or host form without a daemon"),
    },
    "build.yml:verify-native-tests": {
        "Checkout": (*CI, "the bootstrap clone is the checkout"),
        "Set up Python": (*CI, "host interpreter"),
        "ci.py selftest (dispatch + report + run primitives)": ("derive", ANY, ""),
        "Derive vcpkg baseline from vcpkg.json": ("impl:vcpkg-baseline", OWN, "inline python reads vcpkg.json builtin-baseline -- the same field `ci.py vcpkg-baseline` exports"),
        "Install build prerequisites (Homebrew)": ("provision:brew", OWN, "host provisioning is checked, never performed"),
        "Cache vendored Halide v21 distribution": (*CI, "actions/cache"),
        "Fetch vendored Halide v21 distribution": ("seed:halide", OWN, "bootstrap seed"),
        "Cache vendored LibRaw distribution": (*CI, "actions/cache"),
        "Fetch vendored LibRaw distribution": ("derive", ANY, ""),
        "Guard — vendored LibRaw/RawSpeed provenance + licences": ("derive", ANY, ""),
        "Guard — normalize_model.cpp alias-table '@'-prefix convention (G2-2)": ("derive", ANY, ""),
        "Bootstrap vcpkg at the pinned baseline (D5)": ("derive", OWN, ""),
        "vcpkg install libwebp (arm64-osx-heif)": ("derive", OWN, ""),
        "Cache CMake build directory": (*CI, "actions/cache"),
        "Build test_decode target": ("derive", OWN, ""),
        "Build test_device_handoff target": ("derive", OWN, ""),
    },
    "build.yml:verify-dart": {
        "Checkout": (*CI, "the bootstrap clone is the checkout"),
        "Set up Flutter": ("provision:flutter", ANY, "flutter + dart on PATH"),
        "Flutter pub get": ("derive", ANY, ""),
        "dart analyze": ("derive", ANY, ""),
    },
    "windows_build.yml:build-windows": {
        "Force LF line endings for all git operations": ("covered:bootstrap-clone", OWN, "clone runs with -c core.autocrlf=false -c core.eol=lf; the step itself writes --global git config"),
        "Checkout": (*CI, "the bootstrap clone is the checkout"),
        "Set up Python": (*CI, "host interpreter"),
        "Install pytest for deps suite": ("provision:pytest", ANY, "pip-installing into the host interpreter is a host change; presence is checked"),
        "Run deps unit suite (native Windows Python)": ("impl:deps-pytest", ANY, "pwsh body: `python -m pytest native/scripts/deps/ -q`"),
        "Install Ninja": ("provision:ninja", OWN, "pip-installing into the host interpreter is a host change; presence is checked"),
        "Assert Visual Studio ARM64 VC tools are installed (vswhere)": ("derive", OWN, ""),
        "Set up MSVC developer environment (${{ matrix.msvc_arch }})": ("provision:msvc-env", OWN, "vcvars64 environment captured by the gate"),
        "Locate clang-cl": ("derive", OWN, ""),
        "Install Vulkan SDK (provides vulkan-1.lib)": ("provision:vulkan-sdk", OWN, "host Vulkan SDK; the next step verifies vulkan-1.lib"),
        "Verify vulkan-1.lib is present": ("derive", OWN, ""),
        "Cache vendored Halide v21 distribution (Windows x86_64)": (*CI, "actions/cache"),
        "Fetch vendored Halide v21 distribution": ("seed:halide", OWN, "bootstrap seed"),
        "Cache vendored LibRaw + RawSpeed3 + LibRaw-cmake (pinned revisions)": (*CI, "actions/cache"),
        "Fetch vendored LibRaw + RawSpeed3 + LibRaw-cmake": ("derive", OWN, ""),
        "Diagnose LibRaw patch failure (on failure only)": (*CI, "on-failure diagnostics"),
        "Set up MSVC developer environment (x64, cross stage 1 generators)": ("provision:msvc-env", OWN, "arm64 row only"),
        "Build x64 Halide generators + arm64 AOT (cross stage 1)": ("derive", OWN, ""),
        "Restore MSVC developer environment (${{ matrix.msvc_arch }}) for cross stage 2": ("provision:msvc-env", OWN, "arm64 row only"),
        "Build zlib 1.3.1 (static, /MT) for the Windows toolchain": ("derive", OWN, ""),
        "Configure (Ninja + clang-cl, Vulkan AOT target)": ("impl:win-configure", OWN, "body has a CROSS_ARGS if-block (arm64 row); x86_64-row argv implemented"),
        "Assert JXL was statically linked, not silently degraded (G1)": ("impl:win-assert-jxl", OWN, "body is a pattern search inside an if-block; ported"),
        "Diagnose configure failure (on failure only)": (*CI, "on-failure diagnostics"),
        "Build dng_decoder_native": ("derive", OWN, ""),
        "Assert no AVX-512 in in-tree Windows code (portable-baseline gate)": ("derive", OWN, ""),
        "Diagnose build failure (on failure only)": (*CI, "on-failure diagnostics"),
        "Verify Windows artifact": ("derive", OWN, ""),
        "Assert required FFI exports present in Windows DLL (AC-W4)": ("derive", OWN, ""),
        "Assert Windows DLL dependency closure": ("derive", OWN, ""),
        "Assert fused-orientation kernel signals present in Windows DLL (Task 11 / G-E)": ("derive", OWN, ""),
        "Assert full codec capability vector via probe (G1)": ("derive", OWN, ""),
        "Assert build capability vector via probe (S-E2)": ("derive", OWN, ""),
        "Compile + run functional capability probe (CI-T3)": ("derive", OWN, ""),
        "Stage native artifacts": ("derive", OWN, ""),
        "Assert every staged DLL is this leg's PE machine type (AC-C2)": ("derive", OWN, ""),
        "Measure and emit the minimum runtime floor (S-F1)": ("derive", OWN, ""),
        "Assert the Windows shipped-file group is complete (atomic group)": ("derive", OWN, ""),
        "Upload native artifact": (*CI, "actions/upload-artifact"),
        "Upload probe_results_windows_${{ matrix.arch_tag }}": (*CI, "actions/upload-artifact"),
    },
    "macos_build.yml:build": {
        "Checkout": (*CI, "the bootstrap clone is the checkout"),
        "Derive vcpkg baseline from vcpkg.json": ("derive", OWN, ""),
        "Set up Python": (*CI, "host interpreter"),
        "D6 layer 1 — argv equivalence (renderer vs golden vs legacy shell)": ("derive", ANY, "pure argv check"),
        "Install build prerequisites (Homebrew)": ("provision:brew", OWN, "host provisioning is checked, never performed"),
        "Cache vendored Halide v21 distribution": (*CI, "actions/cache"),
        "Fetch vendored Halide v21 distribution": ("seed:halide", OWN, "bootstrap seed"),
        "Cache vendored LibRaw distribution": (*CI, "actions/cache"),
        "Fetch vendored LibRaw distribution": ("derive", OWN, ""),
        "Cache vendored libjxl distribution": (*CI, "actions/cache"),
        "Fetch vendored libjxl distribution": ("derive", OWN, ""),
        "Bootstrap vcpkg at the pinned baseline (D5)": ("derive", OWN, ""),
        "vcpkg install libwebp + libde265 + aom (${{ matrix.vcpkg_triplet }})": ("derive", OWN, ""),
        "Assert the vcpkg artefacts (libwebp static, libde265 shared)": ("derive", OWN, ""),
        "Cache vendored HEIF (libheif + libde265) distribution": (*CI, "actions/cache"),
        "Fetch vendored HEIF distribution (Python carrier)": ("derive", OWN, ""),
        "Assert libde265's linkage in the produced dist (A5.2/A5.3)": ("impl:mac-de265-linkage", OWN, "otool + pattern-search body; ported"),
        "Compile + run functional capability probe (D4/R-7)": ("derive", OWN, ""),
        "Cache CMake build directory": (*CI, "actions/cache"),
        "Build dng_decoder_native via watchdog (native arm64)": ("derive", OWN, ""),
        "Build host Halide generators + x86_64 AOT (cross stage 1)": ("derive", OWN, ""),
        "Cross-compile dng_decoder_native for x86_64 (cross stage 2)": ("derive", OWN, ""),
        "Verify dylib was produced and has the expected architecture": ("derive", OWN, ""),
        "Build + run orientation capability probe (Task 11 / G-E)": ("derive", OWN, ""),
        "Assert full codec capability vector via probe (G1, native leg)": ("derive", OWN, ""),
        "Assert build capability vector via probe (S-E2, native leg)": ("derive", OWN, ""),
        "Assert codec capability vector via configure log (G1, cross leg)": ("derive", OWN, ""),
        "Assert build capability vector via probe (S-E2, cross leg)": ("derive", OWN, ""),
        "Assert required FFI exports present in dylib (G3)": ("derive", OWN, ""),
        "Stage native artifact": ("derive", OWN, ""),
        "Verify staged companion dylibs (arch + reachability + rpath)": ("derive", OWN, ""),
        "Measure and emit the minimum runtime floor (S-F1)": ("derive", OWN, ""),
        "Upload native artifact": (*CI, "actions/upload-artifact"),
        "Upload native build log (native arm64)": (*CI, "actions/upload-artifact"),
        "Upload probe_results_macos_arm64": (*CI, "actions/upload-artifact"),
    },
}


def load_jobs(workflows_dir: Path) -> dict:
    jobs = {}
    for path in sorted(workflows_dir.glob("*.yml")):
        for job in wf.parse_workflow(path):
            jobs[f"{job.file}:{job.job}"] = job
    return jobs


def scope_problems(workflows_dir: Path) -> list:
    """Every way the classification can disagree with the workflow files."""
    jobs = load_jobs(workflows_dir)
    problems = [f"workflow job {j} is not classified in prepush.JOB_SCOPE" for j in sorted(set(jobs) - set(JOB_SCOPE))]
    problems += [f"prepush.JOB_SCOPE classifies {j}, which no workflow defines" for j in sorted(set(JOB_SCOPE) - set(jobs))]
    for key, alias, rows in MIRRORED:
        job = jobs.get(key)
        if job is None:
            continue
        if not job.steps:
            problems.append(f"{key}: parsed zero steps (parser blind to this job?)")
        names = [s.name for s in job.steps]
        scope = STEP_SCOPE.get(key, {})
        problems += [f"{key}: step {n!r} is not classified in prepush.STEP_SCOPE" for n in names if n not in scope]
        problems += [f"{key}: prepush.STEP_SCOPE classifies step {n!r}, which the workflow no longer has"
                     for n in scope if n not in names]
        row_keys = {r.get("arch_tag", "") for r in (job.rows or [{}])}
        if row_keys != set(rows):
            problems.append(f"{key}: matrix rows {sorted(row_keys)} differ from prepush.MIRRORED {sorted(rows)}")
        for step in job.steps:
            kind = scope.get(step.name, ("",))[0]
            for row in job.rows or [{}]:
                try:
                    cond = wf.condition(step, row)
                except ValueError as exc:
                    problems.append(f"{key}: {exc}")
                    continue
                if kind == "derive" and cond == "run":
                    env = _probe_env(job, row)
                    env.update({k: wf.expand_expr(v, row, "W", env) for k, v in step.env.items()})
                    try:
                        cmds = wf.derive_commands(step, row, "W", env)
                    except (KeyError, ValueError) as exc:
                        cmds, why = None, str(exc)
                    else:
                        why = "body uses shell constructs beyond the derivable subset"
                    if not cmds:
                        problems.append(f"{key}: step {step.name!r} is classified derive but is not derivable ({why})")
    return problems


def _probe_env(job, row) -> dict:
    env = {k: k for k in ("GITHUB_WORKSPACE", "RUNNER_TEMP", "GITHUB_ENV", "GITHUB_PATH", "VULKAN_SDK",
                          "VCPKG_BASELINE")}
    env.update(wf.job_env(job, row, "W"))
    return env


def prepush_mentions(workflows_dir: Path) -> list:
    hits = []
    for path in sorted(workflows_dir.glob("*.yml")):
        for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            if "prepush" in line.lower():
                hits.append(f"{path.name}:{n}: {line.strip()}")
    return hits


# ---------------------------------------------------------------------------
# Gate-level steps.
# ---------------------------------------------------------------------------
def a_policy(ctx: Ctx):
    hits = prepush_mentions(ctx.clone / WORKFLOWS_DIR)
    for hit in hits:
        emit(f"::error::workflow references the local-only prepush gate: {hit}")
    emit(f"PREPUSH_POLICY_WORKFLOW_HITS={len(hits)}")
    return 1 if hits else 0


def a_scope(ctx: Ctx):
    for job in sorted(JOB_SCOPE):
        emit(f"PREPUSH_SCOPE {job} -> {JOB_SCOPE[job][0]}: {JOB_SCOPE[job][1]}")
    problems = scope_problems(ctx.clone / WORKFLOWS_DIR)
    for p in problems:
        emit(f"::error::{p}")
    steps = sum(len(v) for v in STEP_SCOPE.values())
    emit(f"PREPUSH_SCOPE_JOBS={len(JOB_SCOPE)} classified_steps={steps} problems={len(problems)}")
    return 1 if problems else 0


# ---------------------------------------------------------------------------
# Workflow-derived execution.
# ---------------------------------------------------------------------------
@dataclass
class JobRow:
    job: wf.WfJob
    row: dict
    workspace: Path
    env: dict
    github_env: Path
    github_path: Path


def jobrow(ctx: Ctx, key: str, alias: str, row: dict) -> JobRow:
    rk = (key, row.get("arch_tag", ""))
    if rk in ctx.jobrows:
        return ctx.jobrows[rk]
    job = load_jobs(ctx.clone / WORKFLOWS_DIR)[key]
    workspace = ctx.clone
    if rk in OWN_WORKSPACE:
        workspace = own_workspace(ctx, f"{alias}-{rk[1]}" if rk[1] else alias, OWN_WORKSPACE[rk])
    state = ctx.clone.parent / "prepush-runner" / f"{alias}-{rk[1] or 'job'}"
    (state / "temp").mkdir(parents=True, exist_ok=True)
    gh_env, gh_path = state / "github_env", state / "github_path"
    gh_env.write_text("", encoding="utf-8")
    gh_path.write_text("", encoding="utf-8")
    env = ctx.base_env()
    env.update({"GITHUB_WORKSPACE": str(workspace), "RUNNER_TEMP": str(state / "temp"),
                "GITHUB_ENV": str(gh_env), "GITHUB_PATH": str(gh_path)})
    env.update(wf.job_env(job, row, str(workspace)))
    ctx.jobrows[rk] = JobRow(job, row, workspace, env, gh_env, gh_path)
    return ctx.jobrows[rk]


def own_workspace(ctx: Ctx, alias: str, seeds: tuple = ()) -> Path:
    """A second checkout for a job row CI runs on its own machine (OWN_WORKSPACE)."""
    ws = ctx.clone.parent / f"ws-{alias}"
    if not ws.exists():
        stream(["git", "-c", "core.autocrlf=false", "clone", "--no-hardlinks", str(ctx.clone), str(ws)], ctx.clone.parent)
        for name in seeds:
            if name in SEEDS:
                seed_tree(name, ctx.clone / SEEDS[name], ws / SEEDS[name])
            elif (ctx.clone / OPTIONAL_SEEDS[name] / ".pins").is_file():
                seed_optional(name, ctx.clone, ws)
    return ws


# One vcpkg fetch per run (scratch-local, gone with the scratch): every
# `ci.py vcpkg-bootstrap` still runs `git clone <VCPKG_REPO_URL>`, which git's
# url.insteadOf rewrites to a mirror cloned from that URL once in this run.
_VCPKG_MIRROR_LOCK = threading.Lock()
_VCPKG_MIRROR: dict = {}


def vcpkg_mirror_env(scratch: Path) -> tuple:
    """(rc, env overlay). The first caller clones the mirror; the rest wait and reuse its rc."""
    from ci.provision import VCPKG_REPO_URL  # noqa: PLC0415

    mirror = scratch / "prepush-vcpkg-mirror.git"
    with _VCPKG_MIRROR_LOCK:
        if "rc" not in _VCPKG_MIRROR:
            _VCPKG_MIRROR["rc"] = stream(["git", "clone", "--mirror", VCPKG_REPO_URL, str(mirror)], scratch)
            emit(f"PREPUSH_VCPKG_MIRROR rc={_VCPKG_MIRROR['rc']} path={mirror}")
        else:
            emit(f"PREPUSH_VCPKG_MIRROR reused rc={_VCPKG_MIRROR['rc']} path={mirror}")
    return _VCPKG_MIRROR["rc"], {"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": f"url.{mirror}.insteadOf",
                                 "GIT_CONFIG_VALUE_0": VCPKG_REPO_URL}


def _absorb_github_files(jr: JobRow) -> None:
    for line in jr.github_env.read_text(encoding="utf-8").splitlines():
        key, sep, value = line.partition("=")
        if sep and re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", key):
            jr.env[key] = value
    for line in jr.github_path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            path_key = next((k for k in jr.env if k.upper() == "PATH"), "PATH")
            if line.strip() not in jr.env.get(path_key, "").split(os.pathsep):
                jr.env[path_key] = line.strip() + os.pathsep + jr.env.get(path_key, "")
    jr.github_env.write_text("", encoding="utf-8")
    jr.github_path.write_text("", encoding="utf-8")


def _resolve_exe(argv0: str, env: dict) -> str:
    if argv0 in ("python3", "python"):
        return py_exe()
    path = next((v for k, v in env.items() if k.upper() == "PATH"), None)
    return shutil.which(argv0, path=path) or argv0


def run_derived(jr: JobRow, step: wf.WfStep) -> int:
    env = dict(jr.env)
    for k, v in step.env.items():
        env[k] = wf.expand_expr(v, jr.row, str(jr.workspace), env)
    cmds = wf.derive_commands(step, jr.row, str(jr.workspace), env)
    if not cmds:
        emit(f"::error::step {step.name!r} is classified derive but its body is not derivable")
        return 2
    cwd = jr.workspace
    if "working-directory" in step.keys:
        cwd = Path(wf.expand_expr(step.keys["working-directory"], jr.row, str(jr.workspace), env))
        if not cwd.is_absolute():
            cwd = jr.workspace / cwd
    rc = 0
    if any("vcpkg-bootstrap" in cmd.argv for cmd in cmds):
        rc, overlay = vcpkg_mirror_env(jr.workspace.parent)
        if rc != 0:
            emit("::error::cloning the vcpkg repository failed")
            return rc
        env.update(overlay)
    for cmd in cmds:
        argv = [_resolve_exe(cmd.argv[0], env), *cmd.argv[1:]]
        tee = None
        if cmd.tee:
            tee = Path(cmd.tee) if Path(cmd.tee).is_absolute() else cwd / cmd.tee
        rc = stream(argv, cwd, env={**env, **cmd.env}, tee=tee)
        if rc != 0:
            break
    _absorb_github_files(jr)
    return rc


# --- implementations named by STEP_SCOPE `impl:` entries --------------------
def i_guards(ctx: Ctx, jr: JobRow, step) -> int:
    ok, detail = docker_available()
    if ok:
        emit(f"guards: docker-form (docker server {detail})")
        return stream(ctx.py("native/scripts/ci.py", "guards", "--docker"), jr.workspace, env=jr.env)
    emit(f"guards: host-form (no docker on host) -- docker said: {detail[-200:]}")
    return stream(ctx.py("native/scripts/ci.py", "guards"), jr.workspace, env=jr.env)


def docker_available() -> tuple:
    rc, out = capture(["docker", "info", "--format", "{{.ServerVersion}}"], cwd=REPO_ROOT)
    return rc == 0, out.strip()


def i_deps_pytest(ctx: Ctx, jr: JobRow, step) -> int:
    rc = stream(ctx.py("-m", "pytest", "native/scripts/deps/", "-q"), jr.workspace, env=jr.env)
    emit(f"DEPS_PYTEST_RC={rc}")
    return rc


def i_vcpkg_baseline(ctx: Ctx, jr: JobRow, step) -> int:
    rc = stream(ctx.py("native/scripts/ci.py", "vcpkg-baseline", "--github-env", str(jr.github_env)),
                jr.workspace, env=jr.env)
    _absorb_github_files(jr)
    return rc


WIN_BUILD_DIR = "native/build-windows"


def i_win_configure(ctx: Ctx, jr: JobRow, step) -> int:
    if jr.row.get("two_stage") == "true":
        emit("::error::the arm64 two-stage configure is not implemented (no ARM64 host)")
        return 3
    zlib = (jr.workspace / "zlib-install").as_posix()
    argv = ["cmake", "-S", "native", "-B", WIN_BUILD_DIR, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl",
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", "-DDNG_DIAGNOSTIC_BUILD=OFF", "-DDNG_VK_PIPELINE_CACHE=OFF",
            f"-DDNG_ZLIB_ROOT={zlib}", f"-DZLIB_ROOT={zlib}", "-DZLIB_USE_STATIC_LIBS=ON"]
    rc = stream(argv, jr.workspace, env=jr.env, tee=jr.workspace / "configure.log")
    emit(f"CONFIGURE_RC={rc}")
    return rc


def i_win_assert_jxl(ctx: Ctx, jr: JobRow, step) -> int:
    log = jr.workspace / "configure.log"
    found = log.is_file() and "JXL: static" in log.read_text(encoding="utf-8", errors="replace")
    rc = 0 if found else 1
    emit(f"ASSERT JXL static-link RC={rc}")
    if rc:
        emit("::error::configure.log does not show the expected 'JXL: static' line from cmake/jxl.cmake")
    return rc


def i_mac_de265_linkage(ctx: Ctx, jr: JobRow, step) -> int:
    """Port of macos_build.yml's libde265 linkage assertion. Gate verified on macOS host at 6b35c47f (2026-10-04, docs/logs/2026-10-04/ceyx-prepush-6b35c47f-090909.log)."""
    dist = jr.workspace / jr.row["heif_dist_dir"]
    lib = dist / "lib"
    if not (lib / "libde265.0.dylib").is_file():
        emit(f"FAIL: {lib}/libde265.0.dylib absent")
        return 1
    if not (lib / "libde265.dylib").exists():
        emit(f"FAIL: {lib}/libde265.dylib absent")
        return 1
    rc, out = capture(["otool", "-D", str(lib / "libde265.0.dylib")], jr.workspace, env=jr.env)
    emit(out)
    if rc != 0 or "@rpath/libde265.0.dylib" not in out.splitlines():
        emit("FAIL: libde265's install name is not @rpath/libde265.0.dylib")
        return 1
    rc, out = capture(["otool", "-L", str(lib / "libheif.1.dylib")], jr.workspace, env=jr.env)
    deps = "\n".join(out.splitlines()[1:])
    emit(deps)
    if rc != 0 or "@rpath/libde265" not in deps:
        emit("FAIL: libheif does not reference libde265 via @rpath")
        return 1
    if jr.env["RUNNER_TEMP"] in deps:
        emit("FAIL: libheif carries a load command into the build machine's vcpkg prefix")
        return 1
    return 0


IMPLS = {
    "guards": i_guards, "deps-pytest": i_deps_pytest, "vcpkg-baseline": i_vcpkg_baseline,
    "win-configure": i_win_configure, "win-assert-jxl": i_win_assert_jxl, "mac-de265-linkage": i_mac_de265_linkage,
}


# --- host provisioning checks (the gate never installs anything) -----------
def p_check(ctx: Ctx, jr: JobRow, step, what: str) -> int:
    path = next((v for k, v in jr.env.items() if k.upper() == "PATH"), None)
    if what == "pytest":
        ok = importlib.util.find_spec("pytest") is not None
        hint = "python -m pip install pytest"
    elif what == "ninja":
        ok = shutil.which("ninja", path=path) is not None
        hint = "python -m pip install ninja"
    elif what == "msvc-env":
        ok = bool(ctx.env)
        hint = "install Visual Studio Build Tools with the x64 VC tools"
    elif what == "vulkan-sdk":
        sdk = jr.env.get("VULKAN_SDK", "")
        ok = bool(sdk) and Path(sdk).is_dir()
        hint = "install the Vulkan SDK (sets VULKAN_SDK)"
    elif what == "flutter":
        ok = shutil.which("flutter", path=path) is not None and shutil.which("dart", path=path) is not None
        hint = "install Flutter (stable) and put flutter/dart on PATH"
    elif what == "brew":
        cmds = wf.derive_commands(step, jr.row, str(jr.workspace), jr.env) or []
        pkgs = next((c.argv[2:] for c in cmds if c.argv[:2] == ["brew", "install"]), [])
        rc, out = capture(["brew", "list", "--versions", *pkgs], jr.workspace, env=jr.env)
        ok = rc == 0 and bool(pkgs)
        hint = f"brew install {' '.join(pkgs)}"
        emit(out.strip())
    else:
        emit(f"::error::unknown provision check {what!r}")
        return 2
    emit(f"PREPUSH_PROVISION({what})={'present' if ok else 'MISSING'}")
    if not ok:
        emit(f"::error::host prerequisite {what} missing -- {hint}")
    return 0 if ok else 1


def workflow_steps(clone: Path, host: str) -> list:
    """Gate steps generated from the mirrored jobs' workflow steps."""
    jobs = load_jobs(clone / WORKFLOWS_DIR)
    steps: list = []
    any_seen: set = set()
    for key, alias, row_hosts in MIRRORED:
        job = jobs.get(key)
        if job is None:
            continue
        scope = STEP_SCOPE.get(key, {})
        for row in job.rows or [{}]:
            rk = row.get("arch_tag", "")
            owner = row_hosts.get(rk)
            label = f"{alias}[{rk}]" if rk else alias
            if owner != host:
                own = [s for s in job.steps if scope.get(s.name, ("", OWN))[1] == OWN and _cond(s, row) == "run"
                       and not scope.get(s.name, ("",))[0].startswith(("ci-only", "seed:", "covered:"))]
                why = (f"row runs on host {owner}" if owner else "no local host can run this row "
                       "(its LoadLibrary/target-arch probes need an ARM64 Windows host)")
                if own:
                    steps.append(Step(label, f"{key} row={rk or '-'} ({len(own)} own-host steps)",
                                      frozenset(), None, skip_reason=why))
            for s in job.steps:
                kind, where, note = scope.get(s.name, ("unclassified", OWN, ""))
                if where == ANY:
                    if (key, s.name) in any_seen:
                        continue
                    any_seen.add((key, s.name))
                elif owner != host:
                    continue
                cond = _cond(s, row)
                name = f"{label}:{_slug(s.name)}" if where == OWN else f"{alias}:{_slug(s.name)}"
                deriv = f"{key} row={rk or '-'} step {s.name!r} [{kind}]" + (f" -- {note}" if note else "")
                if cond == "skip-row":
                    continue
                if cond == "on-failure" or kind == "ci-only":
                    steps.append(Step(name, deriv, ALL_HOSTS, None, status=CIONLY))
                    continue
                if kind.startswith(("seed:", "covered:")):
                    steps.append(Step(name, deriv, ALL_HOSTS, None, status=COVERED))
                    continue
                steps.append(Step(name, deriv, ALL_HOSTS, _wf_action(key, alias, row, s, kind)))
    for key, (alias, owner) in UNPORTED_OWNER.items():
        reason = JOB_SCOPE[key][1]
        if owner == host:
            steps.append(Step(f"{alias}-leg", f"{key}: {reason}", frozenset({host}), None))
        else:
            steps.append(Step(f"{alias}-leg", f"{key}: {reason}", frozenset(), None,
                              skip_reason=f"owned by host {owner}" if owner else "no local host platform owns this leg"))
    return steps


def _cond(step, row) -> str:
    try:
        return wf.condition(step, row)
    except ValueError:
        return "run"  # scope-derivation reports it as a problem


def _slug(text: str) -> str:
    text = re.sub(r"\$\{\{[^}]*\}\}", "", text).lower()
    return re.sub(r"[^a-z0-9]+", "-", text).strip("-")[:60]


def _wf_action(key: str, alias: str, row: dict, step, kind: str):
    def action(ctx: Ctx):
        jr = jobrow(ctx, key, alias, row)
        if kind == "derive":
            return run_derived(jr, step)
        if kind.startswith("impl:"):
            return IMPLS[kind[5:]](ctx, jr, step)
        if kind.startswith("provision:"):
            return p_check(ctx, jr, step, kind[10:])
        emit(f"::error::step {step.name!r} has unknown classification {kind!r}")
        return 2
    return action


# ---------------------------------------------------------------------------
# Windows toolchain environment.
# ---------------------------------------------------------------------------
def _vswhere() -> Optional[Path]:
    base = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    p = Path(base) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    return p if p.is_file() else None


def windows_msvc_env() -> tuple:
    """The vcvars64 environment (what ilammy/msvc-dev-cmd exports in CI),
    plus the VS-bundled LLVM bin dir appended when clang-cl is not already on
    PATH. Returns (env or None, diagnostic)."""
    vswhere = _vswhere()
    if vswhere is None:
        return None, "vswhere.exe not found (Visual Studio Installer absent)"
    rc, out = capture([vswhere, "-latest", "-products", "*", "-requires",
                       "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                      cwd=REPO_ROOT)
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
    # `C:\...` as a remote host spec. CI's pwsh steps resolve System32's
    # bsdtar/curl first.
    msys = [p for p in parts if re.search(r"\\Git\\(usr|mingw64)\\bin", p, re.IGNORECASE)]
    parts = [p for p in parts if p not in msys] + msys
    if shutil.which("clang-cl", path=os.pathsep.join(parts)) is None:
        llvm = Path(install) / "VC" / "Tools" / "Llvm" / "x64" / "bin"
        if (llvm / "clang-cl.exe").is_file():
            parts.append(str(llvm))
    env[path_key] = os.pathsep.join(parts)
    env.update(CHILD_ENV_OVERRIDES)
    return env, f"vcvars64={vcvars}"


# ---------------------------------------------------------------------------
# Test layer: host-unsupported inventory with failure signatures.
# ---------------------------------------------------------------------------
# A failure line, in a build log or runner/test output.
# `PREPUSH_CHILD_EXIT=<nonzero>` is printed by the --decode-case shim for
# every child the runner's case function starts, so an exit code is a
# failure line a signature must account for.
_FAILURE_LINE = re.compile(r"error|FAILED|\bFAIL\b|undefined symbol|PREPUSH_CHILD_EXIT=[1-9]")


@dataclass
class Unsupported:
    cls: str
    evidence: str
    must: tuple  # every regex must match some line of the observed output
    allowed: tuple  # every failure line must match one of these
    # host: should be portable, not yet runnable here (skipped_host).
    # known-defect: a real defect quarantined to the campaign (known_defect).
    kind: str = "host"


def _obj_failed(target: str) -> str:
    return rf"FAILED: \[code=\d+\] CMakeFiles/{target}\.dir/tests/{target}\.cpp\.obj"


def _undefined(symbols: str) -> str:
    return rf"lld-link: error: undefined symbol: .*\b({symbols})\b"


_GENERATED = r"^\d+ errors? generated\.$"
_DLL_INTERNALS = ("links dng_decoder_native and calls non-FFI internals; a Windows DLL exports only "
                  "FFI_EXPORT symbols (macOS/Linux shared libs export all), so lld-link reports them undefined")
_MACOS_DYLIB_FIXTURE = (
    "loads the shipped Mach-O plugin/macos/Libraries/libdng_decoder_native.dylib "
    "(plugin/test/support/native_fixtures.dart:7 shippedDylibPath); it is gitignored (.gitignore:36 *.dylib) "
    "and a Mach-O dylib cannot be loaded on Windows")
_DYLIB_FAILURE = r"libdng_decoder_native\.dylib"


def _link_entry(target: str, symbols: str, detail: str) -> Unsupported:
    return Unsupported("link-error-dll-internals", f"{_DLL_INTERNALS}: {detail}",
                       must=(_undefined(symbols),),
                       allowed=(_undefined(symbols), rf"FAILED: \[code=\d+\] {target}\.exe"))


HOST_UNSUPPORTED: dict = {
    WINDOWS_X64: {
        "target:test_device_handoff": Unsupported(
            "compile-error-posix-header",
            "native/tests/test_device_handoff.cpp:49 #include <unistd.h> -> clang-cl: 'unistd.h' file not found",
            must=(r"test_device_handoff\.cpp\(49,10\): fatal error: 'unistd\.h' file not found",),
            allowed=(r"test_device_handoff\.cpp\(49,10\): fatal error: 'unistd\.h' file not found", _GENERATED,
                     _obj_failed("test_device_handoff"))),
        "target:test_raw_end_to_end": Unsupported(
            "compile-error-posix-api",
            "native/tests/test_raw_end_to_end.cpp:325 setenv/unsetenv undeclared (not in the MSVC CRT)",
            must=(r"test_raw_end_to_end\.cpp\(\d+,\d+\): error: use of undeclared identifier '(un)?setenv'",),
            allowed=(r"test_raw_end_to_end\.cpp\(\d+,\d+\): error: use of undeclared identifier '(un)?setenv'",
                     _GENERATED, _obj_failed("test_raw_end_to_end"))),
        "target:test_raw_hardening": Unsupported(
            "compile-error-posix-api",
            "native/tests/test_raw_hardening.cpp:327 setenv/unsetenv undeclared (not in the MSVC CRT)",
            must=(r"test_raw_hardening\.cpp\(\d+,\d+\): error: use of undeclared identifier '(un)?setenv'",),
            allowed=(r"test_raw_hardening\.cpp\(\d+,\d+\): error: use of undeclared identifier '(un)?setenv'",
                     _GENERATED, _obj_failed("test_raw_hardening"))),
        "target:test_libraw_adapter": _link_entry(
            "test_libraw_adapter",
            "raw_invert_3x3|raw_bayer_filters_check_2x2|raw_black_pattern_from_libraw|raw_camera_to_pcs_from_libraw"
            "|LibRawFrontendContext|LibRawGpuInputAdapter|raw_bayer_phase_from_pattern|raw_classify_layout"
            "|raw_contract_print|raw_layout_class_name|raw_pcs_white|raw_bayer_channel_index_at_plane"
            "|raw_component_black_from_libraw|raw_white_balance_from_libraw",
            "raw_invert_3x3, raw_bayer_filters_check_2x2, LibRawFrontendContext::* ... (19 symbols)"),
        "target:test_raw_sized_decode": _link_entry(
            "test_raw_sized_decode", "raw_pipeline_probe_output_size|raw_pipeline_decode_file_into",
            "raw_pipeline_probe_output_size, raw_pipeline_decode_file_into"),
        "target:test_raw_render_params": _link_entry(
            "test_raw_render_params",
            "dng_render_params_for_test|halide_stage2_ol2_dispatch_failed|halide_try_dispatch_opcode2"
            "|halide_try_dispatch_opcode2_batch|raw_build_render_params|runRenderStage4HalideAot"
            "|raw_camera_to_pcs_from_libraw|LibRawFrontendContext|LibRawGpuInputAdapter|raw_pcs_white"
            "|raw_srgb_to_pcs_matrix|toIdentityHueSatMap",
            "raw_build_render_params, dng_render_params_for_test, raw_pcs_white ... (15 symbols)"),
        "target:test_stage4_oriented": Unsupported(
            "metal-link",
            "references halide_metal_device_interface and the Metal-only AOT stage4 objects",
            must=(_undefined("halide_metal_device_interface"),),
            allowed=(_undefined("halide_metal_device_interface|dng_render_stage4_scaled_preavg|dng_render_stage4_split"
                                "|dng_render_stage4_split_yuv420"),
                     r"FAILED: \[code=\d+\] test_stage4_oriented\.exe")),
        "decode-main:native/tests/run_decode_matrix.py": Unsupported(
            "metal-pinned-baseline",
            "native/tests/kernel_regression_baselines.json SHA256 gates lossless_halide_stage3/stage4 pin Metal output "
            "bytes, and test_decode's own lossy Stage4 self-gate requires 999 dB (Metal-identical); Windows Vulkan "
            "differs on exactly those, every other main-case gate (fixture hashes, lossless PSNR) still gates",
            must=(r"^\[SHA256 GATE\] lossless_halide_stage3: FAIL$", r"^\[SHA256 GATE\] lossless_halide_stage4: FAIL$",
                  r"^\[PSNR GATE\] Stage4: [\d.]+ dB < 999\.00 dB  \[FAIL\]$"),
            allowed=(r"^\[SHA256 GATE\] lossless_halide_stage[34]: FAIL$",
                     r"^\[PSNR GATE\] Stage4: [\d.]+ dB < 999\.00 dB  \[FAIL\]$",
                     r"^\[PSNR GATE\] FAIL — one or more stages below threshold; exiting 1$",
                     r"^\s*ERROR: \[Lossy / Halide Metal\] exit=1$")),
        "decode-case:ffi-dng-lossy": Unsupported(
            "metal-pinned-baseline",
            "needs the lossy Halide test render that run_decode_matrix.py stages only after test_decode's lossy case "
            "passes its Metal-identical self-gate (decode-main entry above)",
            must=(r"\[FFI lossy\] Halide test render missing",),
            allowed=(r"\[FFI lossy\] Halide test render missing", r"^PREPUSH_CASE_RESULT ffi-dng-lossy FAIL")),
        "decode-case:sized-decode": Unsupported(
            "windows-sized-decode-psnr",
            "REAL Windows image-quality defect: Stage4 device handoff fails and the degraded host-copy fallback "
            "drops the sized device route to 36-42 dB vs the CPU reference (AC5-D threshold 55 dB). High-priority "
            "campaign item for the user's attention",
            must=(r"device handoff Stage4 failed; using finish\(\)\+Stage4 host-copy path",
                  r"^\s*FAIL AC5-D: [34]\d\.\d+ dB < 55\.00 dB threshold$"),
            allowed=(r"^\s*FAIL AC5-D: [34]\d\.\d+ dB < 55\.00 dB threshold$", r"^\s*\[FAIL\]$", r"^OVERALL=FAIL$",
                     r"^PREPUSH_CHILD_EXIT=1$",
                     r"^PREPUSH_CASE_RESULT sized-decode FAIL -- exit=1 overall_pass=False "
                     r"device_handoff_fell_back_to_host=True$"),
            kind="known-defect"),
        "decode-case:orient-symbol-absence": Unsupported(
            "macho-nm-instrument",
            "run_decode_matrix.py _run_orient_symbol_absence_case lists exports with `nm -gU` (macOS nm; -U is Mach-O "
            "'defined only') against the production dylib; Windows has no nm and a PE DLL keeps exports in its export "
            "table, not a symbol table. Windows' FFI export surface is gated by windows[x86_64]:assert-required-ffi-"
            "exports (positive set only; the oracle-only ABSENCE check has no Windows instrument)",
            must=(r"PREPUSH_CASE_RESULT orient-symbol-absence FAIL -- instrument `nm -gU`",),
            allowed=(r"PREPUSH_CASE_RESULT orient-symbol-absence FAIL -- instrument `nm -gU`",)),
        **{f"plugin-test:{name}": Unsupported(
            "macos-dylib-fixture", _MACOS_DYLIB_FIXTURE, must=(_DYLIB_FAILURE,), allowed=(_DYLIB_FAILURE,))
           for name in ("decode_failure_error_code_test.dart", "dng_image_native_address_test.dart",
                        "dng_sized_decode_active_test.dart", "dng_sized_decode_fallback_test.dart",
                        "encode_service_test.dart", "raw_decode_service_test.dart", "raw_symbol_absent_test.dart",
                        "retired_symbols_absent_test.dart", "wp10_decode_into_buffer_symbol_absent_test.dart")},
    },
}

# Single tests quarantined as flaky (lead ruling 2026-10-03, halcyon rules:
# never retried; a failure is counted under flaky_known with this evidence,
# any OTHER failing test in the suite stays red). (file, test-name prefix).
FLAKY_TESTS = {
    "plugin": {
        ("decode_pool_test.dart", "TC-942: past the respawn cap"): (
            "same-day pass+fail at the same commit content: passed in run2.log (head b09202f) and dev4.out, failed in "
            "run3.log (head 081efe7, no plugin/ change between them) -- timing-dependent respawn-cap test"),
    },
}

# Recorded as campaign input; printed on every host, never counted.
DEFECT_INVENTORY = (
    ("gitignored-test-fixture",
     "plugin tests depend on plugin/macos/Libraries/ (decoder + @rpath companions), which .gitignore:36 (*.dylib) keeps "
     "out of every fresh clone; the gate seeds it on macOS hosts only. Suites that SKIP (rather than fail) without a "
     "loadable dylib -- native_buffer_pool_alignment_test, native_rotation_bindings_test, service_pooled_arms_test, "
     "service_resize_retry_test, wp10_activation_proof_test -- lose coverage silently; each skipped test is listed per "
     "run as a PREPUSH_INNER_SKIP line"),
    ("fetch-halide-windows-layout",
     "native/scripts/deps/fetch_halide.py already_present() looked only for lib/Halide.lib; the Windows dist ships "
     "lib/Release/Halide.lib, so every Windows CI run re-downloaded the dist (fixed in its own commit)"),
)

REPROOF_DIR = Path("prepush-reproof")  # not under artifacts/: run_decode_matrix.py wipes that


def host_unsupported(host: str) -> dict:
    return HOST_UNSUPPORTED.get(host, {})


def match_signature(entry: Unsupported, text: str) -> list:
    """Problems with `text` as a re-proof of `entry` ([] = signature holds)."""
    lines = text.splitlines()
    problems = [f"declared signature not observed: /{rx}/" for rx in entry.must
                if not any(re.search(rx, ln) for ln in lines)]
    allowed = [re.compile(rx) for rx in entry.allowed]
    for ln in lines:
        if _FAILURE_LINE.search(ln) and not any(rx.search(ln) for rx in allowed):
            problems.append(f"undeclared failure: {ln.strip()[:240]}")
    return problems


def reprove(ctx: Ctx, item: str, rc: int, text: str, log: Path) -> object:
    """Judge one host-unsupported item's re-proof run: HOSTSKIP if it failed
    with exactly its declared signature, else a hard failure."""
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(text, encoding="utf-8")
    entry = host_unsupported(ctx.host)[item]
    if rc == 0:
        emit(f"::error::STALE host-unsupported entry {item}: it now PASSES on {ctx.host}; remove it from "
             "prepush.HOST_UNSUPPORTED so it is gated")
        return 1
    problems = match_signature(entry, text)
    for p in problems:
        emit(f"::error::{item}: {p}")
    if problems:
        emit(f"::error::{item} failed, but NOT with its declared {entry.cls} signature -- a real failure, gate RED "
             f"(log {log.relative_to(ctx.clone).as_posix()})")
        return 1
    status = KNOWNDEFECT if entry.kind == "known-defect" else HOSTSKIP
    emit(f"PREPUSH_{status}({item}): class={entry.cls} rc={rc} signature=matched -- {entry.evidence}")
    hits = [ln for ln in text.splitlines() if _FAILURE_LINE.search(ln)]
    for ln in hits[:6]:
        emit(f"PREPUSH_{status}_EVIDENCE({item}): {ln.strip()[:300]}")
    ctx.host_skips.append((item, entry.cls, entry.evidence, status))
    return (status, item)


# ---------------------------------------------------------------------------
# Test layer: steps.
# ---------------------------------------------------------------------------
def gate_runners(clone: Path) -> tuple:
    """(non-manual runner scripts -> executables, manual scripts) derived
    from native/tests/gates.py."""
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


def build_dir_for(host: str) -> str:
    return WIN_BUILD_DIR if host == WINDOWS_X64 else "native/build"


def _exe(name: str) -> str:
    return name + (".exe" if os.name == "nt" else "")


# `cmake --build --target help` output and keep-going flags are GENERATOR properties, not host properties.
_KEEP_GOING = {"Ninja": ["-k", "0"], "Unix Makefiles": ["-k", f"-j{os.cpu_count() or 1}"]}


def _generator(build_dir: Path) -> str:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return ""
    m = re.search(r"^CMAKE_GENERATOR:INTERNAL=(.*)$", cache.read_text(encoding="utf-8"), re.M)
    return m.group(1).strip() if m else ""


def _help_targets(out: str) -> set:
    """Target names from `cmake --build --target help`: Ninja `name: phony`, Makefiles `... name`."""
    return set(re.findall(r"^(?:\.\.\. )?([A-Za-z0-9_]+)(?=:| |$)", out, re.MULTILINE))


def t_build_tests(ctx: Ctx):
    if ctx.host == LINUX_X64:
        emit("::error::PREPUSH_UNIMPLEMENTED(test-build-targets): no native build leg is implemented for a Linux host")
        return 3
    runners, _ = gate_runners(ctx.clone)
    build_dir = build_dir_for(ctx.host)
    gen = _generator(ctx.clone / build_dir)
    if gen not in _KEEP_GOING:
        emit(f"::error::unsupported CMake generator {gen!r} in {build_dir}")
        return 1
    rc, out = capture(["cmake", "--build", build_dir, "--target", "help"], ctx.clone, env=ctx.base_env())
    available = _help_targets(out)
    wanted = sorted({exe for exes in runners.values() for exe in exes})
    unsupported = {t for t in wanted if f"target:{t}" in host_unsupported(ctx.host)}
    targets = [t for t in wanted if t in available and t not in unsupported]
    absent = [t for t in wanted if t not in available]
    for t in absent:
        emit(f"PREPUSH_TEST_TARGET_NOT_CONFIGURED({t}): this build dir's configure (generator {gen}) defines no such target")
    emit(f"PREPUSH_TEST_TARGETS wanted={len(wanted)} building={len(targets)} host_unsupported={len(unsupported)} "
         f"not_configured={len(absent)}")
    if rc != 0 or not targets:
        emit(f"::error::cannot enumerate test targets in {build_dir} (rc={rc})")
        return rc or 1
    rc = ctx.run(["cmake", "--build", build_dir, "--target", *targets, "--", *_KEEP_GOING[gen]])
    bad = 0
    for t in sorted(unsupported):
        probe_rc, probe_out = capture(["cmake", "--build", build_dir, "--target", t], ctx.clone, env=ctx.base_env())
        verdict = reprove(ctx, f"target:{t}", probe_rc, probe_out, ctx.clone / REPROOF_DIR / f"{t}.build.log")
        bad += verdict == 1
    return rc or (1 if bad else 0)


# gates.py kind `runner:native/scripts/prepush.py`: a bare test binary this gate
# builds (test-build-targets) and runs itself (test-bare-binaries). BARE_CORPUS
# maps an executable to the repo-relative corpus files passed as its arguments.
BARE_RUNNER = "native/scripts/prepush.py"
BARE_CORPUS: dict = {
    "test_idle_funnel": ("image_samples/raw_corpus/DXT50003.RAF",),
}


BARE_SUMMARY_RE = re.compile(r"\[[^\]]*SUMMARY\]\s+executed=(\d+)\s+skipped=(\d+)(?:\s+failed=(\d+))?(?:\s+skipped_cases=(\S*))?")
BARE_SKIP_RE = re.compile(r"\s(\w+) -> SKIP reason=(\S+)")


def run_bare_binaries(build_dir: Path, root: Path, names: list, corpus: dict, run: Callable,
                      inner_skips: Optional[list] = None) -> int:
    """Run each built binary (run(argv) -> (rc, output)); a missing binary, a missing
    corpus file, rc 2 (could not decode), any other nonzero rc, a missing SUMMARY
    line or executed=0 is a FAIL naming it. The binary's own skipped cases are
    appended to inner_skips and echoed with their reasons - declared, never hidden."""
    bad = 0
    for name in names:
        exe = build_dir / _exe(name)
        if not exe.is_file():
            emit(f"::error::PREPUSH_BARE_CASE {name} FAIL: binary not found: {exe}")
            bad += 1
            continue
        args = [root / rel for rel in corpus.get(name, ())]
        missing = [a for a in args if not a.is_file()]
        if missing:
            emit(f"::error::PREPUSH_BARE_CASE {name} FAIL: corpus file missing: {missing[0]}")
            bad += 1
            continue
        emit(f"PREPUSH_EXEC: {exe} {' '.join(map(str, args))}")
        rc, out = run([str(exe), *map(str, args)])
        sys.stdout.write(out)
        if rc != 0:
            why = "could not decode (rc=2)" if rc == 2 else f"rc={rc}"
            emit(f"::error::PREPUSH_BARE_CASE {name} FAIL: {why}")
            bad += 1
            continue
        m = BARE_SUMMARY_RE.search(out)
        if not m:
            emit(f"::error::PREPUSH_BARE_CASE {name} FAIL: no SUMMARY line (executed/skipped counts) in the binary's output")
            bad += 1
            continue
        executed, skipped = int(m.group(1)), int(m.group(2))
        if executed == 0:
            emit(f"::error::PREPUSH_BARE_CASE {name} FAIL: SUMMARY reports executed=0 (vacuous)")
            bad += 1
            continue
        reasons = dict(BARE_SKIP_RE.findall(out))
        cases = [c for c in (m.group(4) or "").split(",") if c]
        cases += [f"unnamed-{i + 1}" for i in range(skipped - len(cases))]
        for case in cases:
            reason = reasons.get(case, "no reason printed")
            emit(f"PREPUSH_BARE_INNER_SKIP {name}::{case} reason={reason}")
            if inner_skips is not None:
                inner_skips.append((f"bare-{name}", f"{case} (reason={reason})"))
        emit(f"PREPUSH_BARE_CASE {name} PASS executed={executed} skipped={skipped}")
    return 1 if bad else 0


def t_bare_binaries(ctx: Ctx):
    if ctx.host == LINUX_X64:
        emit("::error::PREPUSH_UNIMPLEMENTED(test-bare-binaries): no native build leg is implemented for a Linux host")
        return 3
    runners, _ = gate_runners(ctx.clone)
    unsupported = host_unsupported(ctx.host)
    names = [n for n in runners.get(BARE_RUNNER, []) if f"target:{n}" not in unsupported]
    if not names:
        return (SKIP, f"no gates.py entry of kind runner:{BARE_RUNNER} runs on this host")
    return run_bare_binaries(ctx.clone / build_dir_for(ctx.host), ctx.clone, names, BARE_CORPUS,
                             lambda argv: capture(argv, ctx.clone / build_dir_for(ctx.host), env=ctx.base_env()),
                             ctx.inner_skips)


# run_decode_matrix.py harness cases, run one at a time through the runner's
# OWN case functions (so pass/fail semantics stay the runner's) when its main
# cases cannot complete on this host. (case, binary or None).
DECODE_CASES = (
    ("cfa-phase", "test_cfa_phase"),
    ("cfa-color-bggr", "test_cfa_color"),
    ("sized-decode", "test_sized_decode"),
    ("stage4-oriented", "test_stage4_oriented"),
    ("abi-layout", "test_abi_layout"),
    ("encode-yuv420", "ceyx_encode_harness"),
    ("device-handoff", "test_device_handoff"),
    ("ffi-dng-lossless", "dng_ffi_harness"),
    ("ffi-dng-lossy", "dng_ffi_harness"),
    ("ffi-raw", "dng_ffi_harness"),
    ("orient-symbol-absence", None),
)
_MAIN_OPT_OUTS = ("--no-ffi-harness", "--no-device-handoff-harness", "--no-cfa-phase-harness", "--no-bggr-case",
                  "--no-sized-decode-harness", "--no-stage4-oriented-harness", "--no-abi-layout-harness",
                  "--no-orient-symbol-absence-gate", "--no-encode-yuv420-case", "--no-raw-ffi-case")
DECODE_SCRIPT = "native/tests/run_decode_matrix.py"


def t_decode_matrix(ctx: Ctx):
    if ctx.host == MACOS_ARM64:
        return ctx.run(ctx.py(DECODE_SCRIPT))
    if ctx.host == LINUX_X64:
        emit("::error::PREPUSH_UNIMPLEMENTED(test-decode-matrix): no native build leg is implemented for a Linux host")
        return 3
    b = build_dir_for(ctx.host)
    item = f"decode-main:{DECODE_SCRIPT}"
    argv = ctx.py(DECODE_SCRIPT, "--test-decode", f"{b}/{_exe('test_decode')}", *_MAIN_OPT_OUTS)
    emit(f"PREPUSH_EXEC: {' '.join(argv)}")
    rc, out = capture(argv, ctx.clone, env=ctx.base_env())
    sys.stdout.write(out)
    return reprove(ctx, item, rc, out, ctx.clone / REPROOF_DIR / "decode-main.log")


def t_decode_case(case: str, binary: Optional[str]):
    def action(ctx: Ctx):
        b = build_dir_for(ctx.host)
        item = f"decode-case:{case}"
        if binary and f"target:{binary}" in host_unsupported(ctx.host):
            emit(f"PREPUSH_CASE_DEPENDS({case}): binary target:{binary} is host-unsupported (counted there)")
            return (SKIP, f"depends on host-unsupported target:{binary}")
        argv = ctx.py("native/scripts/ci.py", "prepush", "--decode-case", case, "--build-dir", b)
        if case == "encode-yuv420":
            py = pillow_python(ctx)
            if py is None:
                return 1
            argv[0] = py
        emit(f"PREPUSH_EXEC: {' '.join(argv)}")
        rc, out = capture(argv, ctx.clone, env=ctx.base_env())
        sys.stdout.write(out)
        if item in host_unsupported(ctx.host):
            return reprove(ctx, item, rc, out, ctx.clone / REPROOF_DIR / f"decode-case-{case}.log")
        return rc
    return action


def pillow_python(ctx: Ctx) -> Optional[str]:
    """run_decode_matrix.py's yuv420 encode gate needs Pillow + numpy
    (verify_yuv420_encode.py); no repo manifest declares them. Use the host
    interpreter when it has them, else a scratch venv (system site-packages
    + the two packages from PyPI) -- never install into the host."""
    if all(importlib.util.find_spec(m) is not None for m in ("PIL", "numpy")):
        return py_exe()
    venv = ctx.clone.parent / "prepush-pyenv"
    py = venv / ("Scripts/pythonw.exe" if os.name == "nt" else "bin/python")
    if not py.is_file():
        if stream([py_exe(), "-m", "venv", "--system-site-packages", str(venv)], ctx.clone) != 0:
            return None
        if stream([str(py), "-m", "pip", "install", "--disable-pip-version-check", "Pillow", "numpy"], ctx.clone) != 0:
            emit("::error::could not provision Pillow + numpy into the scratch venv")
            return None
    rc, out = capture([str(py), "-m", "pip", "list", "--format=freeze"], ctx.clone)
    emit("PREPUSH_PYENV: " + " ".join(ln for ln in out.splitlines() if ln.lower().startswith(("pillow", "numpy"))))
    return str(py)


def decode_case_main(case: str, build_dir: str) -> int:
    """`prepush --decode-case CASE`: run ONE run_decode_matrix.py case through
    the runner's own case function, in the current checkout."""
    root = REPO_ROOT
    spec = importlib.util.spec_from_file_location("run_decode_matrix", root / DECODE_SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(root / "native/tests"))
    spec.loader.exec_module(mod)
    lossless = str(root / "image_samples/lossless_dng_sample.dng")
    lossy = str(root / "image_samples/lossy_dng_sample.dng")
    artifact_dir = root / "artifacts"
    matrix_current = artifact_dir / "matrix-current"

    def binary(name: str) -> Path:
        return root / build_dir / _exe(name)

    # Record every child's exit code: the runner's case functions fold it
    # into PASS/FAIL, and a quarantine signature must see the code itself.
    original_run = subprocess.run

    def recording_run(*a, **k):
        proc = original_run(*a, **k)
        print(f"PREPUSH_CHILD_EXIT={proc.returncode}", flush=True)
        return proc

    mod.subprocess.run = recording_run
    source = (root / DECODE_SCRIPT).read_text(encoding="utf-8")
    m = re.search(r'"--bggr-min-b-minus-r",\s*type=float,\s*default=([\d.]+)', source)
    min_b_minus_r = float(m.group(1)) if m else None
    ffi_env = {"DNG_STAGE1_TIMING": "1", "DNG_MAP_POLY_TIMING": "1", "DNG_STAGE2_SDK_TIMING": "1"}
    try:
        if case == "cfa-phase":
            result = mod._run_cfa_phase_case(root, binary("test_cfa_phase"))
        elif case == "cfa-color-bggr":
            if min_b_minus_r is None:
                raise RuntimeError("cannot read --bggr-min-b-minus-r default from run_decode_matrix.py")
            result = mod._run_cfa_color_case(root, binary("test_cfa_color"), root / mod._DEFAULT_BGGR_SAMPLE,
                                             min_b_minus_r)
        elif case == "sized-decode":
            result = mod._run_sized_decode_case(root, binary("test_sized_decode"), lossless)
        elif case == "stage4-oriented":
            result = mod._run_stage4_oriented_case(root, binary("test_stage4_oriented"), lossless, 1)
        elif case == "abi-layout":
            result = mod._run_abi_layout_case(root, binary("test_abi_layout"))
        elif case == "encode-yuv420":
            result = mod._run_encode_yuv420_case(root, binary("ceyx_encode_harness"), lossless,
                                                 matrix_current / "encode_yuv420")
        elif case == "orient-symbol-absence":
            lib = root / build_dir / ("dng_decoder_native.dll" if os.name == "nt" else "libdng_decoder_native.dylib")
            if shutil.which("nm") is None:
                print(f"PREPUSH_CASE_RESULT {case} FAIL -- instrument `nm -gU` (Mach-O export listing) not on PATH")
                return 1
            result = mod._run_orient_symbol_absence_case(root, lib, artifact_dir / "orient_symbol_nm.txt")
        elif case in ("ffi-dng-lossless", "ffi-dng-lossy"):
            fixture = case.rsplit("-", 1)[1]
            dng = lossless if fixture == "lossless" else lossy
            staged = mod._stage_ffi_test_render(artifact_dir, matrix_current, fixture, dng)
            run = mod._run_ffi_case(root, str(binary("dng_ffi_harness")), f"{fixture.title()} / FFI", dng,
                                    ffi_env, staged)
            result = mod.CfaCheckResult(name=case, status="PASS", detail=f"ok={run.ok} contract={run.contract_pass}")
        elif case == "ffi-raw":
            raw_dir = matrix_current / "raw_ffi"
            raw_dir.mkdir(parents=True, exist_ok=True)
            run = mod._run_ffi_case(root, str(binary("dng_ffi_harness")), "Generic RAW / FFI",
                                    str(root / mod._DEFAULT_RAW_FFI_SAMPLE),
                                    {**ffi_env, "CEYX_RAW_TIMING_LOG": "1"}, raw_dir, require_rgb_match=False)
            result = mod.CfaCheckResult(name=case, status="PASS", detail=f"ok={run.ok} contract={run.contract_pass}")
        else:
            print(f"::error::unknown decode case {case!r}")
            return 2
    except RuntimeError as exc:
        print(str(exc))
        print(f"PREPUSH_CASE_RESULT {case} FAIL")
        return 1
    print(f"PREPUSH_CASE_RESULT {case} {result.status} -- {result.detail}")
    return 0 if result.status == "PASS" else 1


RAW_SCRIPT = "native/tests/run_raw_matrix.py"
_RAW_CASE = re.compile(r"^\[RawMatrix\] (\S+)\s+(?:rc=-?\d+\s+[\d.]+s -> (PASS|FAIL|SKIP)|-> (FAIL) \(binary missing: "
                       r"[^)]*\)|-> (SKIP) reason=(\S+))")


def t_raw_matrix(ctx: Ctx):
    if ctx.host == MACOS_ARM64:
        return ctx.run(ctx.py(RAW_SCRIPT))
    if ctx.host == LINUX_X64:
        emit("::error::PREPUSH_UNIMPLEMENTED(test-raw-matrix): no native build leg is implemented for a Linux host")
        return 3
    # Every binary case is judged here; the runner's mandatory dng-regression
    # case IS run_decode_matrix.py, which steps test-decode-* cover case by
    # case, so it is skipped with the runner's own flag.
    argv = ctx.py(RAW_SCRIPT, "--build-dir", build_dir_for(ctx.host), "--skip-dng")
    emit(f"PREPUSH_EXEC: {' '.join(argv)}")
    rc, out = capture(argv, ctx.clone, env=ctx.base_env())
    sys.stdout.write(out)
    spec = importlib.util.spec_from_file_location("run_raw_matrix", ctx.clone / RAW_SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(ctx.clone / "native/tests"))
    spec.loader.exec_module(mod)
    expected = {"provenance", "corpus", "architecture-gates", *mod.TEST_BINARIES, "raw-sized-decode", "dng-regression"}
    seen, bad = {}, 0
    for line in out.splitlines():
        m = _RAW_CASE.match(line)
        if m:
            seen[m.group(1)] = (m.group(2) or m.group(3) or m.group(4), m.group(5), "binary missing" in line)
    for name in sorted(expected - set(seen)):
        emit(f"::error::raw matrix case {name} produced no result line -- the instrument did not run it")
        bad += 1
    for name, (status, reason, missing) in sorted(seen.items()):
        target = "test_raw_sized_decode" if name == "raw-sized-decode" else name
        if status == "PASS":
            emit(f"PREPUSH_RAW_CASE {name} PASS")
        elif missing and f"target:{target}" in host_unsupported(ctx.host):
            emit(f"PREPUSH_RAW_CASE {name} not-built -- target:{target} is host-unsupported (counted there)")
        elif name == "dng-regression" and status == "SKIP" and reason == "skip-dng-flag":
            emit("PREPUSH_RAW_CASE dng-regression delegated -- covered case by case by test-decode-* steps")
        else:
            emit(f"::error::raw matrix case {name}: {status}" + (f" reason={reason}" if reason else ""))
            bad += 1
    m = re.search(r"^\[RawMatrix DECLARED\] count=\d+ cases=(\S+)", out, re.MULTILINE)
    for entry in (m.group(1).split(",") if m else []):
        ctx.inner_skips.append(("raw-matrix", f"{entry} (runner-declared per-sample skip)"))
    emit(f"PREPUSH_RAW_MATRIX runner_rc={rc} judged_cases={len(seen)} bad={bad}")
    return 1 if bad else 0


# The shell-built macOS HEIF dist (untracked, so absent from every clone) is the L2 baseline the
# carrier-built dist is compared against; the outer run seeds it next to the clone.
HEIF_BASELINE_SRC = Path("native/third_party/heif-dist")
HEIF_BASELINE_DIR = "heif-baseline-dist"
HEIF_CONSUMER_OUT = "heif-consumer-out"


def dist_equivalence_argv(ctx: Ctx) -> list:
    """Layer-2/3 inputs derived from what the gate itself builds: the carrier dist is the macOS arm64
    row's `heif_dist_dir` in macos_build.yml; the consumer is the HEIF codec test binary the
    build-tests step produced (its link resolves to that dist)."""
    job = load_jobs(ctx.clone / WORKFLOWS_DIR)["macos_build.yml:build"]
    row = next(r for r in job.rows if r.get("arch_tag") == "arm64")
    out = ctx.clone.parent / HEIF_CONSUMER_OUT
    out.mkdir(parents=True, exist_ok=True)
    binary = ctx.clone / build_dir_for(ctx.host) / "test_codec_heif"
    return ["--platform", "macos", "--arch", "arm64",
            "--baseline-dist", str(ctx.clone.parent / HEIF_BASELINE_DIR),
            "--carrier-dist", str(ctx.clone / row["heif_dist_dir"]),
            "--consumer-profile", "heif-codec",
            "--consumer-command", f"{binary} {out}"]


def t_dist_equivalence(ctx: Ctx):
    if ctx.host != MACOS_ARM64:
        return (SKIP, "layers 2-3 compare the macOS carrier-built HEIF dist against the committed macOS dist "
                      "(`--platform` accepts macos or linux only); layer 1 runs as macos:d6-layer-1 on every host")
    return ctx.run(ctx.py("native/tests/run_dist_equivalence.py", *dist_equivalence_argv(ctx)))


def t_flutter(subdir: str):
    def action(ctx: Ctx):
        where = ctx.clone / subdir
        flutter = _resolve_exe("flutter", ctx.base_env())
        rc = stream([flutter, "pub", "get"], where, env=ctx.base_env())
        if rc != 0:
            return rc
        report = ctx.clone / REPROOF_DIR / f"flutter-{subdir}.json"
        report.parent.mkdir(parents=True, exist_ok=True)
        env = ctx.base_env()
        if subdir == "app":
            # The app resolves the dylib via DngDecoderService's generic search, whose script-relative
            # candidates all miss under `flutter test`; the gate's own build is the one under test.
            env["DNG_NATIVE_BUILD_DIR"] = str(ctx.clone / build_dir_for(ctx.host))
        rc = stream([flutter, "test", "--file-reporter", f"json:{report}"], where, env=env)
        tests = parse_dart_json(report)
        for t in tests.values():
            if t["skipped"]:
                ctx.inner_skips.append((f"flutter-{subdir}", f"{t['suite']} :: {t['name']}"))
        failing = [t for t in tests.values() if t["result"] not in ("success", None) and not t["hidden"]]
        prefix = f"{subdir}-test:"
        declared = {k[len(prefix):] for k in host_unsupported(ctx.host) if k.startswith(prefix)}
        emit(f"PREPUSH_DART({subdir}) tests={sum(not t['hidden'] for t in tests.values())} "
             f"failed={len(failing)} skipped={sum(t['skipped'] for t in tests.values())}")
        bad = 0
        for t in failing:
            if Path(t["suite"]).name in declared:
                continue
            flaky = next((ev for (f, prefix), ev in FLAKY_TESTS.get(subdir, {}).items()
                          if Path(t["suite"]).name == f and t["name"].startswith(prefix)), None)
            if flaky:
                item = f"{subdir}-test:{Path(t['suite']).name}::{t['name'][:60]}"
                emit(f"PREPUSH_{FLAKY}({item}): quarantined, NOT retried -- {flaky}")
                ctx.host_skips.append((item, "flaky-quarantine", flaky, FLAKY))
                continue
            emit(f"::error::{subdir} test failed: {t['suite']} :: {t['name']}")
            bad += 1
        by_file: dict = {f: [] for f in sorted(declared)}
        for t in tests.values():
            fname = Path(t["suite"] or "").name
            if fname in by_file and (t["result"] not in ("success", None) or t["errors"]):
                by_file[fname].append(t)
        verdicts = []
        for fname, fails in by_file.items():
            # One line per failing test (name + its whole error text): the
            # signature is judged per failing TEST, not per wrapped line.
            text = "\n".join(f"{t['name']} [{t['result']}] :: " + " | ".join(
                " ".join(e.split()) for e in t["errors"]) for t in fails)
            frc = 1 if fails else 0
            verdicts.append(reprove(ctx, f"{prefix}{fname}", frc, text, ctx.clone / REPROOF_DIR / f"{subdir}-{fname}.log"))
        bad += sum(v == 1 for v in verdicts)
        if rc != 0 and not failing:
            emit(f"::error::flutter test exited {rc} with no failing test in its report (load error?)")
            bad += 1
        return 1 if bad else 0
    return action


def parse_dart_json(path: Path) -> dict:
    """testID -> {name, suite, result, skipped, hidden, errors} from the
    `--file-reporter json:` event stream (dart test JSON reporter protocol)."""
    suites, tests = {}, {}
    if not path.is_file():
        return tests
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            ev = json.loads(line)
        except ValueError:
            continue
        kind = ev.get("type")
        if kind == "suite":
            suites[ev["suite"]["id"]] = ev["suite"].get("path") or ""
        elif kind == "testStart":
            t = ev["test"]
            tests[t["id"]] = {"name": t.get("name", ""), "suite": suites.get(t.get("suiteID"), ""), "result": None,
                              "skipped": False, "hidden": False, "errors": []}
        elif kind == "error" and ev.get("testID") in tests:
            tests[ev["testID"]]["errors"].append(str(ev.get("error", "")))
        elif kind == "testDone" and ev.get("testID") in tests:
            tests[ev["testID"]].update(result=ev.get("result"), skipped=bool(ev.get("skipped")),
                                       hidden=bool(ev.get("hidden")))
    return tests


# ---------------------------------------------------------------------------
# Roster.
# ---------------------------------------------------------------------------
def build_steps(clone: Path, host: str) -> list:
    steps = [
        Step("policy-no-prepush-in-workflows", "gate policy: prepush is local-only", ALL_HOSTS, a_policy),
        Step("scope-derivation", "every workflow job and mirrored step classified; derive steps derivable", ALL_HOSTS, a_scope),
    ]
    steps += workflow_steps(clone, host)
    steps += [
        Step("test-manifest", "native/tests/check_test_manifest.py", ALL_HOSTS,
             lambda ctx: ctx.run(ctx.py("native/tests/check_test_manifest.py")), group="test"),
        Step("test-matrix-parsers", "native/tests/test_decode_matrix_parsers.py", ALL_HOSTS,
             lambda ctx: ctx.run(ctx.py("native/tests/test_decode_matrix_parsers.py")), group="test"),
        Step("test-build-targets", "build every non-manual gates.py runner executable this host configures",
             ALL_HOSTS, t_build_tests, group="test"),
        Step("test-bare-binaries", f"gates.py runners of kind {BARE_RUNNER}: bare test binaries, built then run",
             ALL_HOSTS, t_bare_binaries, group="test"),
        Step("test-decode-matrix", f"gates.py runner {DECODE_SCRIPT} (main cases; full runner on macOS)",
             ALL_HOSTS, t_decode_matrix, group="test"),
    ]
    if host not in (MACOS_ARM64, LINUX_X64):
        for case, binary in DECODE_CASES:
            steps.append(Step(f"test-decode-{case}", f"{DECODE_SCRIPT} case {case} via the runner's own case function",
                              ALL_HOSTS, t_decode_case(case, binary), group="test"))
    steps += [
        Step("test-raw-matrix", f"gates.py runner {RAW_SCRIPT} (every case judged)", ALL_HOSTS, t_raw_matrix, group="test"),
        Step("test-dist-equivalence", "gates.py runner native/tests/run_dist_equivalence.py", ALL_HOSTS,
             t_dist_equivalence, group="test"),
    ]
    try:
        _, manual = gate_runners(clone)
    except (OSError, KeyError, SyntaxError) as exc:
        manual = []
        emit(f"::error::cannot read native/tests/gates.py: {exc}")
    for script in manual:
        steps.append(Step(f"test-{Path(script).stem.replace('_', '-')}", f"gates.py SCRIPTS {script}", frozenset(),
                          None, skip_reason="declared manual: in native/tests/gates.py SCRIPTS", group="test"))
    steps += [
        Step("test-flutter-plugin", "plugin/ `flutter test`", ALL_HOSTS, t_flutter("plugin"), group="test"),
        Step("test-flutter-app", "app/ `flutter test`", ALL_HOSTS, t_flutter("app"), group="test"),
    ]
    return steps


# ---------------------------------------------------------------------------
# Inner run.
# ---------------------------------------------------------------------------
def run_inner(args) -> int:
    clone, host = REPO_ROOT, host_key()
    steps = build_steps(clone, host)
    names = [s.name for s in steps]
    dupes = sorted({n for n in names if names.count(n) > 1})
    if dupes:
        emit(f"::error::duplicate gate step names {dupes}")
        return 2
    unknown = [n for n in (args.step or []) + (args.skip_step or []) if n not in names]
    if unknown:
        emit(f"::error::unknown step name(s) {unknown}; see --list")
        return 2
    ctx = Ctx(clone=clone, host=host)
    results: list = []
    for item in args.bootstrap_result or []:
        name, _, rc = item.partition("=")
        results.append((name, "PASS" if rc == "0" else "FAIL", int(rc)))
    for name in args.skip_step or []:
        emit(f"PREPUSH_SKIP({name}): skipped on request (--skip-step); this run is NOT a full gate")
        results.append((name, SKIP, None))
    selected = [s for s in steps if (not args.step or s.name in args.step) and s.name not in (args.skip_step or [])]
    partial = bool(args.step or args.skip_step)

    if host == WINDOWS_X64:
        env, diag = windows_msvc_env()
        emit(f"PREPUSH_MSVC_ENV: {diag}")
        if env is None:
            emit(f"::error::MSVC developer environment unavailable: {diag}")
        else:
            ctx.env = env

    results += run_selected(ctx, selected, host)

    emit_inventory(ctx)
    return summarize(results, host, args.head or "unknown", partial, len(ctx.inner_skips))


def run_selected(ctx: Ctx, selected: list, host: str) -> list:
    """Run the selected steps (concurrent lanes, see step_lanes) and return their result rows in roster order."""
    lanes = step_lanes(selected, host)
    rows: dict = {}  # index in `selected` -> result rows

    def run_lane(lane: str, items: list) -> None:
        seen: list = []  # this lane's rows so far: a quarantined item is counted once
        for idx, step in items:
            rows[idx] = run_step(ctx, step, host, collect_skips=lane == MAIN_LANE, seen=seen)
            seen += rows[idx]

    if len(lanes) == 1:
        run_lane(MAIN_LANE, lanes[MAIN_LANE])
    else:
        run_lanes(ctx, lanes, run_lane)
    for idx, step in enumerate(selected):
        if idx not in rows:  # a lane died outside a step's own try: red, never silently absent
            emit(f"::error::PREPUSH_LANE_LOST({step.name}): its lane ended without running it")
            emit(f"PREPUSH_STEP_RC({step.name})=4")
            rows[idx] = [(step.name, "FAIL", 4)]
    return [row for idx in range(len(selected)) for row in rows[idx]]


MAIN_LANE = "main"


def step_lanes(selected: list, host: str) -> dict:
    """lane -> [(index, step)]. Steps of an OWN_WORKSPACE row run in a lane of
    their own (own checkout = own CI machine); everything else keeps its
    order in the main lane, so test-* follow the arm64 row there."""
    aliases = {key: alias for key, alias, _ in MIRRORED}
    labels = {f"{aliases[key]}[{rk}]:" if rk else f"{aliases[key]}:": f"{aliases[key]}-{rk}" if rk else aliases[key]
              for key, rk in OWN_WORKSPACE}
    lanes: dict = {MAIN_LANE: []}
    for idx, step in enumerate(selected):
        lane = next((ln for prefix, ln in labels.items() if step.name.startswith(prefix)), MAIN_LANE)
        lanes.setdefault(lane, []).append((idx, step))
    return lanes


class _LaneOut:
    """sys.stdout while lanes run: each thread writes to its own sink (the
    main lane to the real stdout, live; side lanes to a file replayed whole
    afterwards), so no step's lines interleave with another's."""

    def __init__(self, real):
        self.real = real
        self.local = threading.local()

    def _sink(self):
        return getattr(self.local, "sink", None) or self.real

    def write(self, data):
        return self._sink().write(data)

    def flush(self):
        self._sink().flush()


def run_lanes(ctx: Ctx, lanes: dict, run_lane: Callable) -> None:
    out_dir = ctx.clone.parent / "prepush-lanes"
    out_dir.mkdir(parents=True, exist_ok=True)
    router = _LaneOut(sys.stdout)
    side = [ln for ln in lanes if ln != MAIN_LANE]
    emit(f"PREPUSH_LANES main={len(lanes[MAIN_LANE])} " + " ".join(f"{ln}={len(lanes[ln])}" for ln in side)
         + f" (side-lane output replayed after the main lane; live copies in {out_dir})")

    def side_lane(lane: str) -> None:
        with (out_dir / f"{lane}.log").open("w", encoding="utf-8") as fh:
            router.local.sink = fh
            try:
                run_lane(lane, lanes[lane])
            except BaseException as exc:  # noqa: BLE001 - reported; missing rows turn red
                emit(f"::error::lane {lane} raised {type(exc).__name__}: {exc}")

    sys.stdout = router
    try:
        threads = [threading.Thread(target=side_lane, args=(ln,), name=f"lane-{ln}") for ln in side]
        for t in threads:
            t.start()
        try:
            run_lane(MAIN_LANE, lanes[MAIN_LANE])
        finally:
            for t in threads:
                t.join()
    finally:
        sys.stdout = router.real
    for lane in side:
        emit("")
        emit(f"==== PREPUSH LANE {lane} (replay) ====")
        sys.stdout.write((out_dir / f"{lane}.log").read_text(encoding="utf-8"))
        emit(f"==== END LANE {lane} ====")


def run_step(ctx: Ctx, step, host: str, collect_skips: bool, seen: list) -> list:
    out: list = []
    emit("")
    emit(f"==== PREPUSH STEP {step.name} [{step.group}] ====")
    emit(f"PREPUSH_STEP_DERIVATION({step.name}): {step.derivation}")
    if step.status in (CIONLY, COVERED):
        emit(f"PREPUSH_{step.status}({step.name})")
        out.append((step.name, step.status, None))
        return out
    if host not in step.hosts:
        emit(f"PREPUSH_SKIP({step.name}): host={host} -- {step.skip_reason}")
        out.append((step.name, SKIP, None))
        return out
    if step.action is None:
        emit(f"::error::PREPUSH_UNIMPLEMENTED({step.name}): this is host {host}'s own leg and the gate has no "
             "implementation for it")
        emit(f"PREPUSH_STEP_RC({step.name})=3")
        out.append((step.name, "FAIL", 3))
        return out
    emit(f"PREPUSH_STEP_BEGIN({step.name})")
    started = time.monotonic()
    try:
        outcome = step.action(ctx)
    except Exception as exc:  # a crash in the gate is a red step, never a pass
        emit(f"::error::step {step.name} raised {type(exc).__name__}: {exc}")
        outcome = 4
    elapsed = time.monotonic() - started
    if isinstance(outcome, tuple) and outcome[0] == SKIP:
        emit(f"PREPUSH_SKIP({step.name}): host={host} -- {outcome[1]}")
        out.append((step.name, SKIP, None))
        return out
    if isinstance(outcome, tuple) and outcome[0] in ROW_PREFIX:
        emit(f"PREPUSH_STEP_{outcome[0]}({step.name}): {outcome[1]}")
        out.append((f"{ROW_PREFIX[outcome[0]]}:{outcome[1]}", outcome[0], None))
    else:
        rc = int(outcome)
        emit(f"PREPUSH_STEP_RC({step.name})={rc}")
        emit(f"PREPUSH_STEP_SECONDS({step.name})={elapsed:.1f}")
        out.append((step.name, "PASS" if rc == 0 else "FAIL", rc))
    # Items a step quarantined internally (targets, plugin files, flaky
    # tests) each get their own counted row (main lane only: the only lane
    # whose steps quarantine; the others must not read a list it appends to).
    if collect_skips:
        for item, _, _, status in ctx.host_skips:
            row = (f"{ROW_PREFIX[status]}:{item}", status, None)
            if row not in seen and row not in out:
                out.append(row)
    return out


def emit_inventory(ctx: Ctx) -> None:
    emit("")
    for where, name in ctx.inner_skips:
        emit(f"PREPUSH_INNER_SKIP\t{where}\t{name}")
    emit(f"==== PREPUSH QUARANTINE INVENTORY host={ctx.host} count={len(ctx.host_skips)} ====")
    for item, cls, evidence, status in ctx.host_skips:
        tag = {HOSTSKIP: "PREPUSH_HOST_UNSUPPORTED", KNOWNDEFECT: "PREPUSH_KNOWN_DEFECT", FLAKY: "PREPUSH_FLAKY_KNOWN"}
        emit(f"{tag[status]}\t{ctx.host}\t{item}\t{cls}\t{evidence}")
    for cls, evidence in DEFECT_INVENTORY:
        emit(f"PREPUSH_DEFECT_INVENTORY\t{ctx.host}\t-\t{cls}\t{evidence}")
    emit("==== END INVENTORY ====")


def summarize(results: list, host: str, head: str, partial: bool = False, inner_skips: int = 0) -> int:
    emit("")
    emit("==== PREPUSH RESULTS ====")
    for name, status, rc in results:
        emit(f"PREPUSH_RESULT {status:8} {name}" + ("" if rc is None else f" rc={rc}"))

    def count(status):
        return sum(1 for _, s, _ in results if s == status)

    failed = [n for n, s, _ in results if s == "FAIL"]
    emit(f"PREPUSH-SUMMARY host={host} head={head} total={len(results)} passed={count('PASS')} "
         f"failed={len(failed)} skipped={count(SKIP)} skipped_host={count(HOSTSKIP)} "
         f"known_defect={count(KNOWNDEFECT)} flaky_known={count(FLAKY)} skipped_inner={inner_skips} "
         f"ci_only={count(CIONLY)} covered={count(COVERED)} partial={int(partial)}"
         + (f" failed_steps={','.join(failed)}" if failed else ""))
    return 1 if failed else 0


# ---------------------------------------------------------------------------
# Outer run (in the working tree): clone, seed, hand off.
# ---------------------------------------------------------------------------
def tree_manifest(root: Path) -> dict:
    out = {}
    for p in ([root] if root.is_file() else sorted(root.rglob("*"))):
        if p.is_file():
            h = hashlib.sha256()
            with p.open("rb") as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
            out[p.name if p == root else p.relative_to(root).as_posix()] = (p.stat().st_size, h.hexdigest())
    return out


_SEED_HINTS = {
    "halide": "python3 native/scripts/build_deps.py fetch halide",
    "samples": "owner-supplied fixtures",
    "heif-baseline": "the shell-built macOS HEIF dist (see native/third_party/heif-dist/PROVENANCE.md)",
    "macos-dylib": "build or stage the shipped macOS dylib and its companions in plugin/macos/Libraries",
}


def seed_tree(name: str, src: Path, dst: Path) -> int:
    emit(f"PREPUSH_SEED({name}): {src} -> {dst}")
    if not src.exists():
        emit(f"::error::seed source {src} does not exist in the working tree -- provide it there first "
             f"({_SEED_HINTS.get(name, '')})")
        return 1
    if dst.is_dir():
        shutil.rmtree(dst)
    if src.is_file():
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    else:
        shutil.copytree(src, dst, symlinks=False)
    a, b = tree_manifest(src), tree_manifest(dst)
    files, size = len(b), sum(s for s, _ in b.values())
    mismatched = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
    emit(f"PREPUSH_SEED_VERIFY({name}): files={files} bytes={size} sha256_mismatches={len(mismatched)}")
    for k in mismatched[:20]:
        emit(f"::error::seed {name}: {k} differs after copy")
    return 1 if mismatched or files == 0 else 0


def seed_optional(name: str, src_root: Path, clone_root: Path) -> int:
    """seed_tree for an OPTIONAL_SEEDS dist, then the dist dir's TRACKED files
    (PROVENANCE.md) are put back to the clone's HEAD: the seed supplies only
    gitignored build output, never an uncommitted edit."""
    rel = OPTIONAL_SEEDS[name]
    rc = seed_tree(name, src_root / rel, clone_root / rel)
    _, tracked = capture(["git", "ls-files", "--", rel.as_posix()], cwd=clone_root)
    if rc == 0 and tracked.strip():
        rc = stream(["git", "checkout", "HEAD", "--", rel.as_posix()], cwd=clone_root)
    return rc


# Samples the test layer reads by default path, on top of the sha256-locked
# fixtures in kernel_regression_baselines.json. A missing one turns the gate red.
_DEFAULT_SAMPLES = ("image_samples/raw_sample.arw", "image_samples/bayer_conc_a.dng",
                    "image_samples/lossless_dng_sample.dng")


def samples_ok(clone: Path) -> int:
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
        emit(f"::error::{bad} required test sample(s) missing or hash-mismatched in the clone")
    return 1 if bad else 0


def halide_version_ok(dst: Path) -> int:
    sys.path.insert(0, str(REPO_ROOT / "native" / "scripts" / "deps"))
    import fetch_halide  # type: ignore

    machine = "x86_64" if host_key() == WINDOWS_X64 else platform_module.machine()
    _, _, asset = fetch_halide.resolve_asset(platform_module.system(), machine)
    text = (dst / "VERSION").read_text(encoding="utf-8") if (dst / "VERSION").is_file() else ""
    ok = f"asset: {asset}" in text and fetch_halide.already_present(dst)
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
    emit(f"PREPUSH_PYTHON={sys.executable} {platform_module.python_version()} children={py_exe()}")
    emit("PREPUSH_CHILD_ENV: " + " ".join(f"{k}={v}" for k, v in CHILD_ENV_OVERRIDES.items())
         + " (CI runners use a UTF-8 locale; set for every child)")
    if host == MACOS_ARM64:
        emit("PREPUSH_STATUS: the macOS leg, macOS dylib seed and macOS test layer are verified on macOS host at 6b35c47f (2026-10-04, docs/logs/2026-10-04/ceyx-prepush-6b35c47f-090909.log)")
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
        _, chead = capture(["git", "rev-parse", "HEAD"], cwd=clone)
        if chead.strip() != head:
            emit(f"::error::clone HEAD {chead.strip()} != working tree HEAD {head}")
            rc = 1
        rc = rc or stream(["git", "config", "core.autocrlf", "false"], cwd=clone)
    emit(f"PREPUSH_STEP_RC(bootstrap-clone)={rc}")
    results.append(("bootstrap-clone", "PASS" if rc == 0 else "FAIL", rc))
    if rc != 0:
        return summarize(results, host, head)
    if "macos-dylib" not in HOST_SEEDS.get(host, {}):
        emit(f"PREPUSH_SEED_STATUS(macos-dylib): not seeded on host={host} (macOS hosts only); "
             "that path is not exercised on this host")
    for name, rel in {**SEEDS, **HOST_SEEDS.get(host, {})}.items():
        rc = seed_tree(name, REPO_ROOT / rel, clone / rel)
        if name == "halide" and rc == 0:
            rc = halide_version_ok(clone / rel)
        if name == "samples":
            rc = samples_ok(clone) or rc
        emit(f"PREPUSH_STEP_RC(bootstrap-seed-{name})={rc}")
        results.append((f"bootstrap-seed-{name}", "PASS" if rc == 0 else "FAIL", rc))
    for name, rel in OPTIONAL_SEEDS.items():
        if not (REPO_ROOT / rel / ".pins").is_file():
            emit(f"PREPUSH_SEED_STATUS({name}): no built dist (.pins) at {rel} in the working tree -- not seeded; "
                 "its fetch step builds it from source")
            continue
        rc = seed_optional(name, REPO_ROOT, clone)
        emit(f"PREPUSH_STEP_RC(bootstrap-seed-{name})={rc}")
        results.append((f"bootstrap-seed-{name}", "PASS" if rc == 0 else "FAIL", rc))
    if host == MACOS_ARM64:
        rc = seed_tree("heif-baseline", REPO_ROOT / HEIF_BASELINE_SRC, scratch / HEIF_BASELINE_DIR)
        emit(f"PREPUSH_STEP_RC(bootstrap-seed-heif-baseline)={rc}")
        results.append(("bootstrap-seed-heif-baseline", "PASS" if rc == 0 else "FAIL", rc))
    inner = [py_exe(), str(clone / "native/scripts/ci.py"), "prepush", "--inner", "--head", head]
    for name, _, rc in results:
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
    for step in build_steps(REPO_ROOT, host_key()):
        hosts = ",".join(sorted(step.hosts)) or "none"
        emit(f"{step.name:58} [{step.group}] {step.status or ''} hosts={hosts} :: {step.derivation}")
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
    p.add_argument("--decode-case", default=None, help=argparse.SUPPRESS)
    p.add_argument("--build-dir", default=None, help=argparse.SUPPRESS)


def default_log_path(head: str, now: Optional[time.struct_time] = None) -> Path:
    now = now or time.localtime()
    return (REPO_ROOT / "docs" / "logs" / time.strftime("%Y-%m-%d", now)
            / f"ceyx-prepush-{head}-{time.strftime('%H%M%S', now)}.log")


def main(args) -> int:
    # Child output is UTF-8; a legacy-code-page console or redirect (cp950)
    # cannot encode all of it and would kill the gate mid-run.
    # Under pythonw with no redirect there is no stdout at all; --log is then
    # the only output (artifact-first), so write the console copy to devnull.
    if sys.stdout is None:
        sys.stdout = open(os.devnull, "w", encoding="utf-8")
    if sys.stderr is None:
        sys.stderr = sys.stdout
    for s in (sys.stdout, sys.stderr):
        if hasattr(s, "reconfigure"):
            s.reconfigure(encoding="utf-8", errors="replace")
    if args.decode_case:
        return decode_case_main(args.decode_case, args.build_dir or build_dir_for(host_key()))
    if args.list:
        return list_steps()
    if args.inner:
        return run_inner(args)
    # Byte-stable invocation (user decree): the artifact path is chosen here, never on the command line.
    if not args.log:
        rc, head = capture(["git", "rev-parse", "--short", "HEAD"], cwd=REPO_ROOT)
        args.log = str(default_log_path(head.strip() if rc == 0 else "unknown"))
    log_path = Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as fh:
        real = sys.stdout
        sys.stdout = _Tee(real, fh)
        try:
            emit(f"PREPUSH_LOG={log_path}")
            rc = run_outer(args)
            emit(f"PREPUSH_EXIT_RC={rc}")
        finally:
            sys.stdout = real
    return rc
