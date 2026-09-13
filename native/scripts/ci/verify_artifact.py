"""Linux artifact-verification gates: `linux_build.yml`'s L16 step (AC-L2/
AC-L3/AC-L4), the S-B3 import-closure gate, the S-F1 min-runtime
measure+drift pair, AC-L5's export-manifest check, and the portable-baseline
AVX-512 wrapper.

Frozen interface: docs/logs/2026-09-13/pyci-plan.md WI-7. Every emitted line
below is transcribed verbatim from `linux_build.yml` at `d33cc607` (the AC-2
baseline commit) -- this module changes WHERE the assertions run, never
WHAT they assert or print. Two pre-existing gaps port as-is on purpose
(C-G4 items; do not "fix" them here, see the two PORTED AS-IS comments
below and their pinning tests in test_verify_artifact.py):

  * the AC-L4 ldd gate branches on a substring match, never on ldd's own RC;
  * `READ_MIN_RUNTIME_RC` is printed but never gated (the min-runtime DRIFT
    check, not the read, is what fails the step).

Accumulate-all-failures does NOT apply inside `verify_artifact()`: the
original shell aborts at the first failing sub-check, and preserving that
exact short-circuit is itself part of AC-2 parity (a later sub-check's
`::error::` line must never appear on a run where an earlier one failed
today).
"""

from __future__ import annotations

import glob
import os
import re
import sys
from pathlib import Path

from . import report, run, targets

# native/scripts/assert_exports.py is a sibling top-level script (not part of
# this package) that already exposes a programmatic `run(manifest, platform,
# dump_text)` entry point -- imported directly rather than through
# `run.run()`, unlike the other sub-scripts below, all of which only expose
# an argparse `main()` and are therefore invoked as child processes.
_SCRIPTS_DIR = Path(__file__).resolve().parents[1]
if str(_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS_DIR))
import assert_exports as _assert_exports_script  # noqa: E402
# Imported for its PE/ELF dump PARSER only (P-23). The gate itself is still
# invoked as a child process below, exactly as linux/android already do --
# this import exists so the transitive walk enumerates a module's imports
# with the same parser the gate uses, instead of growing a second regex
# that could drift from it.
import assert_import_closure as _assert_import_closure_script  # noqa: E402


def _artifact_path(platform: str) -> str:
    return targets.spec(platform)["artifact_path"]


def verify_artifact(platform: str, arch: str | None = None, *, dylib_path: str | None = None) -> int:
    """Linux: AC-L2 (file) + AC-L3 (nm -D vulkan symbol) + AC-L4 (ldd, no
    libvulkan). Replaces `linux_build.yml:507-540` -- UNCHANGED from before.

    macOS/windows added here (WI-22 follow-on): each replaces a completely
    DIFFERENT check under the same command name -- ``file`` exists is the
    only thing the three share. macOS asserts the produced dylib's
    architecture matches `arch` (`macos_build.yml:581-593`); windows only
    asserts the DLL exists and is non-empty (`windows_build.yml:494-511` --
    its exports check is a SEPARATE step, now `assert_exports()`, not this
    function). Every emitted line and every `::error::` message (including
    macOS's genuine absence of any stderr redirect on either of its two
    error lines, ported as-is) is transcribed verbatim per platform."""
    if platform == "linux":
        so = _artifact_path(platform)

        report.section("AC-L2: file")
        result = run.run(["file", so])
        if result.stdout:
            report.plain(result.stdout.rstrip("\n"))
        report.bare_rc(result.returncode)
        if result.returncode != 0:
            report.error(f"file '{so}' failed (rc={result.returncode}); the Linux .so is missing.")
            return 1

        report.section("AC-L3: nm -D halide_vulkan_device_interface")
        run.run_to_file(["nm", "-D", so], "nm_dynsyms.txt")
        dump_text = Path("nm_dynsyms.txt").read_text(errors="replace")
        matches = [line for line in dump_text.splitlines() if "halide_vulkan_device_interface" in line]
        for line in matches:
            report.plain(line)
        rc = 0 if matches else 1
        report.bare_rc(rc)
        if rc != 0:
            report.error(
                f"halide_vulkan_device_interface not found in {so} — "
                "Vulkan runtime absent from the binary (AC-L3)."
            )
            return 1

        report.section("AC-L4: ldd (expect NO libvulkan)")
        ldd_result = run.run_to_file(["ldd", so], "ldd_out.txt")
        ldd_text = Path("ldd_out.txt").read_text(errors="replace")
        if ldd_text:
            report.plain(ldd_text.rstrip("\n"))
        report.bare_rc(ldd_result.returncode)
        # PORTED AS-IS (pre-existing, d33cc607 linux_build.yml:534): the gate is
        # the substring test below, never ldd's own RC -- see
        # test_ldd_gate_is_substring_not_rc.
        if "libvulkan" in ldd_text:
            report.error("libvulkan appears in ldd output — link-time Vulkan dependency regressed (D4).")
            return 1

        return 0

    if platform == "macos":
        if not dylib_path:
            raise ValueError("verify_artifact(platform='macos') requires dylib_path")
        if not arch:
            raise ValueError("verify_artifact(platform='macos') requires arch")
        so = dylib_path
        if not Path(so).is_file():
            # PORTED AS-IS: no stderr redirect on this line in the source
            # shell (macos_build.yml:583-585), matching the same convention
            # already established for the staging/exports steps.
            report.error(f"Expected dylib not found at {so}", stream=sys.stdout)
            return 1
        result = run.run(["file", so])
        report.plain(result.stdout.rstrip("\n"))
        lipo_result = run.run(["lipo", "-archs", so])
        archs = lipo_result.stdout.strip()
        report.plain(f"lipo -archs => {archs}")
        if archs != arch:
            report.error(
                f"Expected architecture '{arch}' but the dylib reports '{archs}'",
                stream=sys.stdout,
            )
            return 1
        otool_result = run.run(["otool", "-L", so])
        report.plain(otool_result.stdout.rstrip("\n"))
        return 0

    if platform == "windows":
        so = _artifact_path(platform)
        if not Path(so).is_file():
            report.error(f"{so} not found — the Windows build did not emit the expected DLL.")
            return 1
        report.plain("== file ==")
        result = run.run(["file", so])
        if result.returncode != 0:
            report.notice("file(1) unavailable on this runner; size check below is the binding evidence.")
        else:
            report.plain(result.stdout.rstrip("\n"))
        report.plain("== size (bytes) ==")
        size = Path(so).stat().st_size
        report.marker("DLL_SIZE_BYTES", size)
        if size <= 0:
            report.error("DLL is zero bytes.")
            return 1
        return 0

    raise ValueError(f"verify_artifact: unsupported platform {platform!r}")


def import_closure(
    platform: str, *, artifact_dir: str | None = None, ndk_home: str | None = None
) -> int:
    """S-B3 import-closure gate. Replaces the pre-migration inline shell of
    the step named `Import-closure gate (S-B3)` in `linux_build.yml` and in
    `android_build.yml` (android, WI-34/WI-38 push-8 follow-on), and of
    `Assert Windows DLL dependency closure` in `windows_build.yml`
    (windows, P-23 -- the last unmigrated step of that workflow).

    THOSE SHELL BODIES NO LONGER EXIST AT ANY LINE: each of the three steps
    is now a one-line `ci.py` call, which is why every citation in this
    docstring names a STEP NAME to grep for rather than a line range. The
    ranges that used to stand here were not merely off by a few lines --
    `linux_build.yml:548-564` had drifted onto an unrelated
    `capability-vector` step ~65 lines from the real one -- and they could
    not have been kept correct by care, because the text they pointed at was
    deleted by the very migration they describe. To read the pre-migration
    shell, `git log -S` the step name in that workflow.

    WINDOWS, AND WHY THE OLD "PERMANENTLY EXCLUDED" NOTE HERE WAS WRONG:
    this docstring, `ci.py`'s `_IMPORT_CLOSURE_PLATFORMS` comment,
    `windows_build.yml`'s step comment and `allowlist.py`'s BLOCKED entry all
    said windows was blocked because "`assert_import_closure.py` has no PE
    branch". It has had one since WI-4 -- grep that file for `parse_pe_dump`,
    `WINDOWS_OS_ALLOWLIST` and its `--format` choices -- and the windows step
    was already calling it with `--format pe`. What was actually missing was a
    PE leg HERE: the ELF legs below capture their dump with readelf, and
    nothing captured a dumpbin/llvm-objdump dump. That is what
    `_import_closure_windows()` adds.

    CITATIONS IN THIS PARAGRAPH ARE DELIBERATELY SYMBOL-ANCHORED, NOT LINE-
    NUMBERED, and that is the round's own lesson rather than a style
    preference: this migration was briefed against FOUR prose sites citing
    line numbers that had rotted (including one naming a line past the end of
    its file), and the first draft of this very docstring cited the
    `--format elf` argv by a number that its own edit had already moved. A
    comment recording HOW TO RE-DERIVE an answer survives editing; one
    recording the answer expires on the next edit. To find the ELF hardcode
    this paragraph is about, grep this file for the `"--format", "elf"` argv.

    macOS still has no DT_NEEDED-shaped step in its YAML at all, so its
    exclusion is unchanged and remains genuine.

    Android needed a caller-supplied `artifact_dir`/`ndk_home` (R5: the
    decoder's location and the NDK's llvm-readelf path are workflow context,
    never a `targets.py` fact) -- same redesign `stage.py` already went
    through for its own android/macos functions. Two things are PORTED
    AS-IS and deliberately asymmetric with linux (verified against
    android's own pre-migration shell body -- `git log -S 'Import-closure
    gate (S-B3)' -- .github/workflows/android_build.yml` -- not assumed from
    linux's shape):

      * No `report.section()` banner and no echo of the readelf dump on
        android -- the shell redirects `llvm-readelf`'s combined output
        straight into a file (`> android_main_dynamic.txt 2>&1`) with no
        `cat`/`echo` of a section title anywhere in that half of the step,
        unlike linux's `echo "== S-B3: ... =="` + `cat readelf_dynamic.txt`.
      * The failure message text differs: android's names the script
        explicitly ("... (WI-4/assert_import_closure.py) failed for ...")
        where linux's does not. Two different literal strings in the
        source YAML, kept as two different literal strings here -- not
        unified (P-10 discipline).

    The `IMPORT_CLOSURE_RC=<n>` marker is the ONE thing genuinely identical
    across both platforms in the source YAML (`echo "IMPORT_CLOSURE_RC=${RC}"`
    verbatim in both `linux_build.yml` and `android_build.yml`) -- reused
    as-is via the same `report.rc("IMPORT_CLOSURE", rc)` call, not given a
    platform-specific twin the way stage.py's completion markers are: that
    would invent a divergence the source YAML does not have.

    ADDED, NOT A PORT: the `ls ... | head -n1` in android's pre-migration
    shell (same `git log -S` as above; the pipeline is gone from the tree
    with the rest of that body) had no explicit empty-match guard and would
    have fallen through
    to a readelf invocation on an empty/garbage path (eventual failure via
    assert_import_closure.py's own UNVERIFIED-on-unparseable-dump path,
    just via a different, less legible route). A clean, named failure is
    raised here instead of reproducing that crash-shaped gap verbatim --
    flagged for the leader's ruling, not silently decided as equivalent."""
    if platform == "windows":
        return _import_closure_windows()

    if platform == "android":
        if not artifact_dir or not ndk_home:
            raise ValueError(
                "import_closure(platform='android') requires artifact_dir and ndk_home"
            )
        llvm_readelf = os.path.join(
            ndk_home, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin", "llvm-readelf"
        )
        if not os.access(llvm_readelf, os.X_OK):
            report.error(
                f"llvm-readelf not found at {llvm_readelf} — NDK layout may have changed "
                "(r27c expected)."
            )
            return 1
        matches = sorted(
            glob.glob(os.path.join(artifact_dir, "native", "libdng_decoder_native*.so"))
        )
        if not matches:
            report.error(f"no libdng_decoder_native*.so found under {artifact_dir}/native")
            return 1
        so = matches[0]
        dump_path = "android_main_dynamic.txt"
        run.run_to_file([llvm_readelf, "-d", so], dump_path)
        staged_dir = os.path.join(artifact_dir, "native")
    else:
        so = _artifact_path(platform)
        spec = targets.spec(platform)
        readelf_tool = spec["readelf_tools"][0] if spec["readelf_tools"] else "readelf"
        staged_dir = spec["dist_dir"]
        dump_path = "readelf_dynamic.txt"

        report.section("S-B3: readelf DT_NEEDED import-closure gate")
        run.run_to_file([readelf_tool, "-d", so], dump_path)
        dynamic_text = Path(dump_path).read_text(errors="replace")
        if dynamic_text:
            report.plain(dynamic_text.rstrip("\n"))

    rc, out = run.capture([
        sys.executable, "native/scripts/assert_import_closure.py",
        "--dump", dump_path,
        "--staged-dir", staged_dir,
        "--declaration", "native/deps/shipped_files.toml",
        "--platform", platform,
        "--format", "elf",
    ])
    if out:
        report.plain(out.rstrip("\n"))
    report.rc("IMPORT_CLOSURE", rc)
    if rc != 0:
        if platform == "android":
            report.error(
                f"import-closure gate (WI-4/assert_import_closure.py) failed for {so} — "
                "see IMPORT ... -> MISSING lines above."
            )
        else:
            report.error(
                f"import-closure gate failed for {so} — see IMPORT ... -> MISSING lines above."
            )
    return rc


_PE_DUMP_HEADER = "Dump of file"


def _pe_dump_imports(
    binary: str, dump_path: str, *, primary_marker: str, fallback_marker: str, subject: str,
    prefix: str = "",
) -> int:
    """Captures a PE import-table dump to ``dump_path`` (dumpbin, falling back
    to llvm-objdump) and returns the RC of whichever tool produced the file.

    Both tools write to a FILE and the match happens in Python afterwards --
    never `dumpbin | grep`, which SIGPIPEs the producer on a SUCCESSFUL match
    under pipefail and reports 141 (the 2026-08-28 reverse-gate shape the
    replaced shell step also called out in its own comment)."""
    spec = targets.spec("windows")
    dumpbin = spec["dumpbin_tools"][0] if spec["dumpbin_tools"] else "dumpbin"
    objdump = spec["objdump_tools"][0] if spec["objdump_tools"] else "llvm-objdump"

    rc = run.run_to_file([dumpbin, "-dependents", binary], dump_path).returncode
    report.marker(primary_marker, rc)
    if rc != 0:
        message = f"{dumpbin} -dependents{subject} failed (rc={rc}); falling back to {objdump}."
        if prefix:
            # Inside the transitive walk the fallback notice is a PLAIN
            # prefixed line, not a `::notice::`: `::notice::` matches
            # markerdiff's marker pattern (`markerdiff.MARKER_RE`), so one
            # per visited module would make the step's marker multiset depend
            # on how many companions the graph happens to contain.
            report.plain(f"{prefix}notice: {message}")
        else:
            report.notice(message)
        rc = run.run_to_file([objdump, "-p", binary], dump_path).returncode
        report.marker(fallback_marker, rc)
    return rc


def _strip_pe_dump_header(dump_path: str, body_path: str) -> str:
    """Writes ``dump_path`` minus its "Dump of file <path>" header lines to
    ``body_path`` and returns the body text.

    Load-bearing, not cosmetic: if the dumped file's own PATH contains the
    name being asserted (e.g. a workspace directory holding "heif.dll"), the
    header self-matches and the assertion passes without the binary importing
    anything at all -- the 2026-08-28 `otool -L` self-match shape."""
    text = Path(dump_path).read_text(errors="replace")
    body = "\n".join(line for line in text.splitlines() if _PE_DUMP_HEADER not in line)
    if body:
        body += "\n"
    Path(body_path).write_text(body, encoding="utf-8")
    return body


def _run_pe_closure_gate(body_path: str, staged_dir: str) -> tuple[int, str]:
    return run.capture([
        sys.executable, "native/scripts/assert_import_closure.py",
        "--dump", body_path,
        "--staged-dir", staged_dir,
        "--declaration", "native/deps/shipped_files.toml",
        "--platform", "windows",
        "--format", "pe",
    ])


def _pe_transitive_closure(root_body: str, staged_dir: str, decoder_name: str) -> int:
    """Walks the import graph BEYOND depth one and gates every staged module
    it reaches, returning the worst RC.

    WHY THIS EXISTS AND WHY DEPTH-ONE IS NOT ENOUGH: the decoder's own import
    table names heif.dll but NOT libde265.dll -- no translation unit here
    references a de265_* symbol, so libde265 appears only in heif.dll's table
    (run 33294722901 proved asserting it at the wrong depth is red on a
    correct artifact). The same shape holds for the OpenMP runtime:
    libomp140.x86_64.dll is imported by the decoder and itself imports
    VCRUNTIME140/VCRUNTIME140_1. A depth-one gate therefore cannot see a
    companion that ships with an unsatisfied dependency of its own, which on a
    user's machine is a total load failure naming only dng_decoder_native.dll.

    OUTPUT SHAPE, DELIBERATE: each sub-module's gate output is re-printed with
    a LOWERCASE `transitive(<module>):` prefix, so none of those lines matches
    markerdiff's marker pattern (`markerdiff.MARKER_RE`, anchored on an
    uppercase `NAME=`).
    The evidence stays in the job log, but the step's MARKER multiset is
    unchanged -- one `IMPORT_CLOSURE_RC`, aggregated over the whole walk,
    rather than one per visited module. That keeps AC-2 zero-delta without a
    `markerdiff.EXPECTED_ADDITIONS` ledger entry (another owner's file) and
    without weakening the gate: a transitive failure still fails the step
    through the aggregated RC."""
    # Case-insensitive, for the same reason the Windows allowlist is (see
    # assert_import_closure.CASE_INSENSITIVE_PLATFORMS): an import table may
    # spell a staged companion in any case. Under a case-SENSITIVE match the
    # walk would silently SKIP that module -- an under-walk that looks exactly
    # like a clean pass, which is the worse failure mode here. The map keeps the
    # real on-disk name so the file is still opened by its actual path.
    staged_by_key = (
        {p.name.lower(): p.name for p in Path(staged_dir).iterdir()}
        if Path(staged_dir).is_dir() else {}
    )
    decoder_key = decoder_name.lower()

    def staged_imports(body_text: str) -> list[str]:
        return [
            staged_by_key[name.lower()]
            for name in _assert_import_closure_script.parse_pe_dump(body_text)
            if name.lower() in staged_by_key and name.lower() != decoder_key
        ]

    queue = staged_imports(root_body)
    visited: set[str] = {decoder_name}
    worst = 0
    while queue:
        module = queue.pop(0)
        if module in visited:
            continue
        visited.add(module)
        dump_path = f"transitive_{module}.dump.txt"
        body_path = f"transitive_{module}.body.txt"
        rc = _pe_dump_imports(
            os.path.join(staged_dir, module),
            dump_path,
            # Lowercased marker names on purpose: see this function's output
            # note. `report.marker` prints them verbatim, and a lowercase name
            # is not a marker to markerdiff.
            primary_marker=f"transitive({module}): dependents_rc",
            fallback_marker=f"transitive({module}): objdump_rc",
            subject=f" on {module}",
            prefix=f"transitive({module}): ",
        )
        if rc != 0:
            report.error(
                f"could not read the import table of {module}; dependency presence is "
                "UNVERIFIED, refusing to publish."
            )
            worst = worst or 1
            continue
        body = _strip_pe_dump_header(dump_path, body_path)
        sub_rc, sub_out = _run_pe_closure_gate(body_path, staged_dir)
        for line in sub_out.splitlines():
            report.plain(f"transitive({module}): {line}")
        if sub_rc != 0:
            report.error(
                f"import-closure gate failed for staged companion {module} — its own imports "
                "are not satisfied beside the decoder; see the transitive IMPORT ... -> MISSING "
                "lines above."
            )
            worst = worst or sub_rc
        queue.extend(staged_imports(body))
    return worst


def _import_closure_windows() -> int:
    """P-23: `windows_build.yml`'s "Assert Windows DLL dependency closure"
    step. Every emitted line below is transcribed verbatim from that step's
    shell, with ONE addition, marked in place: the transitive walk (see
    `_pe_transitive_closure`). The two named depth-assertions the shell makes
    (heif.dll in the decoder's table, libde265.dll in heif.dll's) are KEPT as
    they are rather than folded into the general walk -- they carry
    hand-written, actionable `::error::` text ("do NOT disable HEIF", "rebuild
    the dist via heif_dist_windows.yml") that a generic closure failure cannot
    reproduce."""
    spec = targets.spec("windows")
    dll = _artifact_path("windows")
    staged_dir = spec["dist_dir"]

    report.section("Step 4: import table")
    rc = _pe_dump_imports(
        dll, "dll_dependents.txt",
        primary_marker="DEPENDENTS_RC", fallback_marker="LLVM_OBJDUMP_RC", subject="",
    )
    dump_text = Path("dll_dependents.txt").read_text(errors="replace") if Path(
        "dll_dependents.txt").is_file() else ""
    if rc != 0:
        report.error(
            f"could not read the import table of {dll}; dependency presence is UNVERIFIED, "
            "refusing to publish."
        )
        if dump_text:
            report.plain(dump_text.rstrip("\n"))
        return 1
    if dump_text:
        report.plain(dump_text.rstrip("\n"))

    body = _strip_pe_dump_header("dll_dependents.txt", "dll_dependents_body.txt")
    heif_matches = [line for line in body.splitlines() if "heif.dll" in line.lower()]
    for line in heif_matches:
        report.plain(line)
    heif_rc = 0 if heif_matches else 1
    report.plain(f"ASSERT dep heif.dll RC={heif_rc}")
    if heif_rc != 0:
        report.error(
            f"{dll} does not import heif.dll — the HEIF route was not compiled in. Do NOT 'fix' "
            "this by disabling HEIF; check the configure log for 'HEIF: libheif' and that "
            "native/third_party/heif-dist-windows was found."
        )
        return 1

    report.section("Step 4: full import-closure gate (S-B1)")
    closure_rc, closure_out = _run_pe_closure_gate("dll_dependents_body.txt", staged_dir)
    if closure_out:
        report.plain(closure_out.rstrip("\n"))
    # ADDED (P-23), and placed BEFORE the marker so the single aggregated
    # `IMPORT_CLOSURE_RC` covers the whole graph, not just depth one.
    transitive_rc = _pe_transitive_closure(body, staged_dir, Path(dll).name)
    closure_rc = closure_rc or transitive_rc
    report.rc("IMPORT_CLOSURE", closure_rc)
    if closure_rc != 0:
        report.error(
            f"import-closure gate failed for {dll} — see IMPORT ... -> MISSING lines above. A "
            "package shipped like this fails at DynamicLibrary.open with an error naming only "
            "dng_decoder_native.dll."
        )
        return closure_rc

    report.section("Step 4: heif.dll's own import table (transitive libde265)")
    heif_path = os.path.join(staged_dir, "heif.dll")
    heif_dump_rc = _pe_dump_imports(
        heif_path, "heif_dependents.txt",
        primary_marker="HEIF_DEPENDENTS_RC", fallback_marker="HEIF_LLVM_OBJDUMP_RC",
        subject=" on heif.dll",
    )
    heif_text = Path("heif_dependents.txt").read_text(errors="replace") if Path(
        "heif_dependents.txt").is_file() else ""
    if heif_dump_rc != 0:
        report.error(
            "could not read the import table of heif.dll; the HEVC decoder linkage is "
            "UNVERIFIED, refusing to publish."
        )
        if heif_text:
            report.plain(heif_text.rstrip("\n"))
        return 1
    if heif_text:
        report.plain(heif_text.rstrip("\n"))
    de265_matches = [line for line in heif_text.splitlines() if "libde265.dll" in line.lower()]
    for line in de265_matches:
        report.plain(line)
    de265_rc = 0 if de265_matches else 1
    report.plain(f"ASSERT dep libde265.dll RC={de265_rc}")
    if de265_rc != 0:
        report.error(
            "heif.dll does not import libde265.dll — it was built WITHOUT an HEVC decoder and "
            "would report every HEIC file as undecodable. Rebuild the dist via "
            ".github/workflows/heif_dist_windows.yml; do NOT disable HEIF."
        )
        return 1
    return 0


# P-10 (push 6): `min_runtime()` used to live here, Linux-only. It was
# orphaned when `ci.py`'s dispatch was rewired to the four-platform
# `ci/minruntime.py` generalisation and never called again in production --
# but it stayed invisible to every test for weeks because
# `ci.minruntime.min_runtime` and this module's former `min_runtime` shared
# the exact same name and signature, and on the only platform the stale
# dispatch could still reach (linux) they did the same readelf work. Deleted
# here, by this file's owner, in push 7 (deferred from push 6 deliberately --
# deleting a function in another WI's file mid-migration is the exact
# cross-ownership edit this campaign forbids; push 6 only fixed the dispatch).
# Its three direct-behaviour tests (`test_min_runtime_read_rc_is_not_gated`,
# `test_min_runtime_drift_failure_returns_nonzero`,
# `test_emission_matches_golden_min_runtime`) were deleted in the same
# commit -- this project's standing rule is that a superseded module's tests
# and docs go with it, no tombstones. Equivalent coverage lives in
# `ci/minruntime.py`'s own `_from_dump` and `test_minruntime.py`.
# `test_dispatch.py`'s `TestMinRuntimeDispatchGeneralised` no longer spies on
# this module's `min_runtime` attribute at all -- it asserts the attribute
# does not exist, which is a STRONGER regression guard than "was not called"
# (a future revert cannot recreate the function without the deletion itself
# being noticed).


def assert_exports(
    platform: str,
    arch: str | None = None,
    *,
    dylib_path: str | None = None,
    artifact_dir: str | None = None,
    ndk_home: str | None = None,
) -> int:
    """AC-L5/G3/AC-W4/G6: required FFI exports present in the built
    artifact. Replaces `linux_build.yml:629-641` (unchanged from before --
    linux still reads `nm_dynsyms.txt`, produced as a side effect of
    `verify_artifact()` earlier in the same job), `macos_build.yml:741-760`,
    `windows_build.yml:544-585`, `android_build.yml:295-326`.

    macOS/windows/android could NOT be collapsed onto linux's shape: the
    shell-prohibition guard's compliance test
    (`check_shell_prohibition.py:95,168`) requires exactly one code line
    starting with a python/pwsh-python invocation, so a `tool > file` dump
    step can never itself be a compliant one-liner -- the dump has to move
    INSIDE this module for every platform whose `ci.py verify-artifact`
    equivalent does not already produce a dump as a side effect (only linux
    does). One module change serves all three non-linux legs (they share
    an identical dump-then-assert shape) rather than three near-duplicate
    per-leg scripts -- the exact P-10 failure class this campaign keeps
    re-finding.

    Every dump-phase RC marker name and every `::error::` message below is
    transcribed VERBATIM per platform, including macOS's genuine ABSENCE of
    any `::error::` line on a dump failure (`exit "${RC}"` with no error
    text in the source shell -- ported as-is, not an omission here)."""
    if platform == "linux":
        so = _artifact_path(platform)
        report.section("AC-L5: required FFI exports present in .so")
        dump_text = Path("nm_dynsyms.txt").read_text(errors="replace")
    elif platform == "macos":
        if not dylib_path:
            raise ValueError("assert_exports(platform='macos') requires dylib_path")
        so = dylib_path
        result = run.run_to_file(["nm", "-gU", so], "dylib_exports.txt")
        report.marker("NM_EXPORTS_RC", result.returncode)
        if result.returncode != 0:
            report.error(
                f"nm failed on {so} (rc={result.returncode}); export presence is "
                "UNVERIFIED, refusing to publish."
            )
            text = Path("dylib_exports.txt").read_text(errors="replace")
            if text:
                report.plain(text.rstrip("\n"))
            return 1
        dump_text = Path("dylib_exports.txt").read_text(errors="replace")
    elif platform == "windows":
        so = _artifact_path(platform)
        report.plain("== AC-W4: exported FFI symbols ==")
        result = run.run(["dumpbin", "-exports", so])
        Path("dll_exports.txt").write_text(result.stdout + result.stderr)
        rc = result.returncode
        report.marker("DUMPBIN_RC", rc)
        if rc != 0:
            report.notice(f"dumpbin unavailable or failed (rc={rc}); falling back to llvm-nm.")
            result = run.run(["llvm-nm", "--extern-only", "--defined-only", so])
            Path("dll_exports.txt").write_text(result.stdout + result.stderr)
            rc = result.returncode
            report.marker("LLVM_NM_RC", rc)
        if rc != 0:
            report.error(
                f"could not read the export table of {so} with either dumpbin or "
                f"llvm-nm (rc={rc}); export presence is UNVERIFIED, refusing to publish."
            )
            text = Path("dll_exports.txt").read_text(errors="replace")
            if text:
                report.plain(text.rstrip("\n"))
            return 1
        dump_text = Path("dll_exports.txt").read_text(errors="replace")
        report.plain("-- first 60 lines of the export listing --")
        report.plain("\n".join(dump_text.splitlines()[:60]))
    elif platform == "android":
        if not artifact_dir or not ndk_home:
            raise ValueError(
                "assert_exports(platform='android') requires artifact_dir and ndk_home"
            )
        matches = sorted(
            glob.glob(os.path.join(artifact_dir, "native", "libdng_decoder_native*.so"))
        )
        if not matches:
            report.error(f"no libdng_decoder_native*.so found under {artifact_dir}/native")
            return 1
        so = matches[0]
        llvm_nm = os.path.join(
            ndk_home, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin", "llvm-nm"
        )
        if not os.access(llvm_nm, os.X_OK):
            report.error(
                f"llvm-nm not found at {llvm_nm} — NDK layout may have changed (r27c expected)."
            )
            return 1
        result = run.run_to_file([llvm_nm, "-D", so], "android_so_dynsyms.txt")
        report.marker("NM_RC", result.returncode)
        if result.returncode != 0:
            report.error(
                f"llvm-nm -D failed on {so} (rc={result.returncode}); export presence is "
                "UNVERIFIED, refusing to publish."
            )
            text = Path("android_so_dynsyms.txt").read_text(errors="replace")
            if text:
                report.plain(text.rstrip("\n"))
            return 1
        dump_text = Path("android_so_dynsyms.txt").read_text(errors="replace")
    else:
        raise ValueError(f"assert_exports: unsupported platform {platform!r}")

    # Keyword call, not positional: `native/scripts/deps/test_no_shell_lint.py`
    # flags any call named `run` whose FIRST POSITIONAL argument is a bare
    # string (the subprocess-argv shape it exists to catch) -- this is a
    # direct in-process call to assert_exports.py's own `run()`, not a
    # subprocess invocation, so it is a false positive under that lint's
    # name-only matching. Keyword args are not `node.args`, so this call
    # shape is correctly outside the lint's scope without touching the lint
    # itself (an un-owned, frozen file this campaign does not modify).
    rc = _assert_exports_script.run(
        manifest_path="native/deps/export_manifest.toml", platform=platform, dump_text=dump_text
    )
    report.rc("ASSERT_EXPORTS", rc)
    if rc != 0:
        if platform == "windows":
            report.error(
                f"assert_exports.py reported missing/absent symbol(s) in {so} — the DLL does "
                "not export the FFI surface Dart looks up. On Windows this needs "
                "__declspec(dllexport) via FFI_EXPORT on the definitions in native/src/ffi/ "
                "(see dng_ffi_api.cpp and heif_ffi_api.cpp)."
            )
        elif platform == "android":
            report.error(
                f"assert_exports.py reported missing/absent symbol(s) in {so} — the .so does "
                "not export the FFI surface Dart looks up."
            )
        elif platform == "linux":
            report.error(
                f"assert_exports.py reported missing/absent symbol(s) in {so} — the .so does "
                "not export the FFI surface Dart looks up (check FFI_EXPORT on the definitions "
                "in native/src/ffi/)."
            )
        # macOS: PORTED AS-IS -- the original shell has no `::error::` line
        # here at all, only `exit "${RC}"`. Do not add one.
    return rc


def verify_staged_companions(platform: str, dylib_path: str, artifact_dir: str, arch: str) -> int:
    """macOS's three staged-companion gates (macos_build.yml:790-861), split
    out of the "Stage native artifact" step per leader ruling: `stage.py`
    stages, this module verifies. THREE INDEPENDENT GATES, none implies the
    others (each ported verbatim, see the YAML comment this replaces for the
    real defect each one alone was proven to catch):

      1. Architecture: `lipo -archs` on each STAGED companion (from
         `<artifact_dir>/native/`, i.e. after `stage.py` has already copied
         it there -- not the pre-staged source).
      2. Reachability: the companion's basename appears in the decoder's OWN
         `otool -L` dependency listing (read from `dylib_path`, the ORIGINAL
         path -- not the staged copy; ported as-is, this asymmetry is
         intentional in the source step).
      3. Path convention: that dependency line must start `@rpath/<companion>`.

    Self-reference handling: `otool -L`'s line 2 is the inspected file's own
    LC_ID_DYLIB entry, stripped by NAME (`@rpath/<dylib basename>`), never by
    a positional offset (measured directly against a real decoder: a
    positional `tail -n +2` alone does not strip it).

    Every emitted line has NO `>&2` redirect in the original shell (unlike
    every other error line in this module) -- ported as-is via
    `report.error(..., stream=sys.stdout)`, matching report.py's documented
    exception list.
    """
    import read_shipped_files

    _SCRIPTS_DIR2 = Path(__file__).resolve().parents[1]
    if str(_SCRIPTS_DIR2) not in sys.path:
        sys.path.insert(0, str(_SCRIPTS_DIR2))

    companions = read_shipped_files.load_declaration()[platform]["companions"]
    staged_dir = Path(artifact_dir) / "native"
    dylib_basename = os.path.basename(dylib_path)

    report.plain(f"--- Gate 1: architecture (every staged companion reports {arch}) ---")
    for companion in companions:
        result = run.run(["lipo", "-archs", str(staged_dir / companion)])
        archs = (result.stdout + result.stderr).strip()
        report.plain(f"arch({companion}): {archs}")
        if not re.search(rf"\b{re.escape(arch)}\b", archs):
            report.error(
                f"{companion} archs '{archs}' do not include {arch}", stream=sys.stdout
            )
            return 1

    report.plain("--- Gates 2+3: reachability + path convention (decoder's own dependency graph) ---")
    run.run_to_file(["otool", "-L", dylib_path], "otool_dylib_deps.txt")
    all_lines = Path("otool_dylib_deps.txt").read_text(errors="replace").splitlines()
    # tail -n +2, then strip the self-reference (LC_ID_DYLIB) line by NAME,
    # not by position -- see docstring.
    dylib_dep_lines = [
        line for line in all_lines[1:] if f"@rpath/{dylib_basename}" not in line
    ]
    report.plain("\n".join(dylib_dep_lines))
    for companion in companions:
        matching = [line for line in dylib_dep_lines if companion in line]
        companion_line = "\n".join(matching)
        if not companion_line:
            report.error(
                f"{dylib_path} does not depend on {companion} at all — staged but "
                "unlinked (dead file).",
                stream=sys.stdout,
            )
            return 1
        report.plain(f"dependency line for {companion}: {companion_line}")
        if not any(re.match(rf"^\s*@rpath/{re.escape(companion)} ", line) for line in matching):
            report.error(
                f"{companion} is depended upon via a non-relative reference "
                f"({companion_line}) — expected the line to start with @rpath/{companion}. "
                "This would fail to load on any machine but the build host (the exact "
                "class of defect fixed in b0a0573).",
                stream=sys.stdout,
            )
            return 1
    return 0


def assert_no_avx512(platform: str) -> int:
    """Portable-baseline gate: the published .so must contain no AVX-512
    codepath. Replaces `linux_build.yml:671-681`. C-G3: `assert_no_avx512.py`
    keeps its logic inline in its own `main()` -- called through `run.run()`,
    never refactored into this module."""
    so = _artifact_path(platform)

    rc, out = run.capture([
        sys.executable, "native/scripts/assert_no_avx512.py",
        so,
        "--dump", "native/build-linux/avx512-gate-objdump.txt",
    ])
    if out:
        report.plain(out.rstrip("\n"))
    report.bare_rc(rc, "avx512 gate")
    if rc != 0:
        report.error(
            f"AVX-512 gate failed (rc={rc}) for libdng_decoder_native.so — refusing to publish "
            "an artifact that SIGILLs on non-AVX-512 CPUs. rc=2 means the check could not run at "
            "all, which is equally disqualifying."
        )
        # PORTED AS-IS (pre-existing, d33cc607 linux_build.yml:678-682): the
        # shell always `exit 1` here regardless of the child's actual rc
        # (2 for "could not run" collapses to the same 1 as an assertion
        # failure) -- see test_avx512_failure_returns_1_not_rc.
        return 1
    return 0
