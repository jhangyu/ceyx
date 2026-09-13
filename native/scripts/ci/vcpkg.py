"""The five Windows HEIF-dist vcpkg steps (`.github/workflows/heif_dist_windows.yml`).

WI-30, push 8b. Separate module from `provision.py` deliberately: `provision.py`
already owns Linux/macOS's `vcpkg-baseline`/`vcpkg-bootstrap`/`vcpkg-install`/
`assert-vcpkg-artefacts` (push 8, WI-22), but their shell bodies are NOT
identical to the Windows dist leg's -- Windows bootstraps with
`bootstrap-vcpkg.bat` + `vcpkg.exe` (not `.sh` + extension-less `vcpkg`), and
the dist leg's artefact assertion checks a single library (aom, static) plus
a licence file, not `provision.py`'s webp/de265/aom triple. Folding this in
would put Windows-only branches into a module the plan wants branch-free
(motivation: "`dist_build.py` carries no Windows-only logic"), so it is its
own module instead.

C9 rule (already enforced for `provision.py`): no vcpkg baseline literal
(the 40-hex-digit `builtin-baseline` SHA) may live in Python -- `vcpkg.json`
is read at runtime, every time, because `ci_conventions_check` only scans
YAML for that literal and would never see one hiding in this file.
"""

from __future__ import annotations

from pathlib import Path

from . import report, run


def baseline(manifest_path: str, github_env_path: str) -> int:
    """Replaces heif_dist_windows.yml:86-90 ("Derive vcpkg baseline from
    vcpkg.json"). Reads `builtin-baseline` out of `vcpkg.json` and appends
    `VCPKG_BASELINE=<sha>` to `$GITHUB_ENV` -- never truncates (see
    `report.github_env_append`)."""
    import json

    data = json.loads(Path(manifest_path).read_text())
    sha = data["builtin-baseline"]
    report.github_env_append(github_env_path, "VCPKG_BASELINE", sha)
    return 0


def bootstrap(root: str, sha: str) -> int:
    """Replaces heif_dist_windows.yml:130-139 ("Bootstrap vcpkg at the
    pinned baseline (D1-a)"): clone, checkout the pinned baseline, run
    `bootstrap-vcpkg.bat` (Windows-specific -- `.sh` is Linux/macOS's,
    `provision.vcpkg_bootstrap`'s job, not this one), print `vcpkg.exe
    version`. Short-circuits on the first failing step, same as the shell's
    unguarded command sequence under an implicit errexit."""
    vcpkg_dir = str(Path(root) / "vcpkg")

    result = run.run(["git", "clone", "https://github.com/microsoft/vcpkg.git", vcpkg_dir])
    if result.stdout:
        report.plain(result.stdout.rstrip("\n"))
    if result.returncode != 0:
        report.error(f"git clone of vcpkg failed (rc={result.returncode}).")
        return result.returncode

    result = run.run(["git", "-C", vcpkg_dir, "checkout", sha])
    if result.stdout:
        report.plain(result.stdout.rstrip("\n"))
    if result.returncode != 0:
        report.error(f"git checkout {sha} failed in {vcpkg_dir} (rc={result.returncode}).")
        return result.returncode

    result = run.run([str(Path(vcpkg_dir) / "bootstrap-vcpkg.bat"), "-disableMetrics"])
    if result.stdout:
        report.plain(result.stdout.rstrip("\n"))
    if result.returncode != 0:
        report.error(f"bootstrap-vcpkg.bat failed (rc={result.returncode}).")
        return result.returncode

    result = run.run([str(Path(vcpkg_dir) / "vcpkg.exe"), "version"])
    if result.stdout:
        report.plain(result.stdout.rstrip("\n"))
    return result.returncode


def install(manifest_root: str, install_root: str, triplet: str, features: list[str], rc_marker: str) -> int:
    """Replaces heif_dist_windows.yml:156-171 ("vcpkg install de265 + aom
    (x64-windows-heif)"). Argv shape transcribed verbatim from the YAML's
    `set +e ... rc=$?; echo "VCPKG_INSTALL_RC=${rc}"; exit $rc`: RC is read
    off `RunResult.returncode`, adjacent to the `run.run` call, never off a
    shell variable. `rc_marker` is caller-supplied (the WI-30 spec's
    `--rc-marker VCPKG_INSTALL_RC`), not hardcoded here -- same posture as
    `dist_build.py`'s `--rc-marker`."""
    # vcpkg.exe lives under <root>/vcpkg, which the caller does not repeat
    # separately -- WI-30's argv only carries --install-root (the
    # vcpkg-installed dest), so the binary path is derived the same way
    # bootstrap() derives it: sibling "vcpkg" dir under the same root as
    # install_root's parent.
    root = Path(install_root).parent
    vcpkg_bin = str(root / "vcpkg" / "vcpkg.exe")
    argv = [
        vcpkg_bin, "install",
        f"--x-manifest-root={manifest_root}",
        f"--x-install-root={install_root}",
        f"--triplet={triplet}",
        "--x-no-default-features",
        *[f"--x-feature={f}" for f in features],
        "--no-print-usage",
    ]
    result = run.run(argv)
    if result.stdout:
        report.plain(result.stdout.rstrip("\n"))
    if result.stderr:
        report.plain(result.stderr.rstrip("\n"))
    report.marker(rc_marker, result.returncode)
    return result.returncode


def assert_aom_artifact(prefix: str) -> int:
    """Replaces heif_dist_windows.yml:173-183 ("Assert the vcpkg aom
    artefact (static)"). Both failure messages are byte-identical to the
    YAML's -- a migration preserves semantics, it does not tighten wording.
    Order preserved: `ls -la lib/` unconditionally, then the two `test -f`
    checks, first-failure-wins (the YAML's `||` short-circuits identically:
    the second `test -f` never runs once the first has already failed and
    exited)."""
    prefix_path = Path(prefix)
    lib_dir = prefix_path / "lib"

    ls_result = run.run(["ls", "-la", str(lib_dir)])
    if ls_result.stdout:
        report.plain(ls_result.stdout.rstrip("\n"))

    if not (lib_dir / "aom.lib").is_file():
        report.plain(f"FAIL: aom.lib absent (expected a static archive) under {prefix}/lib")
        return 1

    if not (prefix_path / "share" / "aom" / "copyright").is_file():
        report.plain(
            "FAIL: share/aom/copyright absent -- vendor_licences() needs it for the PATENTS grant"
        )
        return 1

    return 0


def export_prefix(prefix: str, github_env_path: str) -> int:
    """Replaces heif_dist_windows.yml:192-198 ("Export CEYX_VCPKG_PREFIX
    for the carrier"). Appends, never truncates (`report.github_env_append`
    opens the file in `"a"` mode)."""
    report.github_env_append(github_env_path, "CEYX_VCPKG_PREFIX", prefix)
    return 0
