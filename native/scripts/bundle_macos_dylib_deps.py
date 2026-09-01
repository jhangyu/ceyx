#!/usr/bin/env python3
"""Make a macOS shared library self-contained by vendoring its Homebrew deps.

dng_decoder_native links liblcms2/libjpeg-turbo (LibRaw's colour management +
JPEG deps) via find_package(), which on this host resolves to absolute
/opt/homebrew paths. That's fine for local test binaries (run on a dev
machine that has Homebrew), but the shared library ships inside distributed
app bundles -- a machine without Homebrew installed at those exact paths
can't load it.

Copies any Homebrew-path dependency next to the target dylib, rewrites its
own install name to @rpath/<name>, and repoints the target's load command at
@rpath/<name>. Recurses into copied deps in case of further Homebrew-path
dependencies. Idempotent: an already-@rpath dependency is left alone.
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

HOMEBREW_PREFIXES = ("/opt/homebrew/", "/usr/local/")

# Dependency forms this script already knows are fine as-is: relative-path
# conventions this project's own build already uses (@rpath, and the two
# lower-level loader tokens in case something ever emits them directly), and
# genuine OS-provided absolute paths that exist identically on every Mac and
# need no vendoring.
KNOWN_SAFE_PREFIXES = ("@rpath/", "@loader_path/", "@executable_path/", "/usr/lib/", "/System/")


def otool_deps(path: Path) -> list[str]:
    out = subprocess.run(["otool", "-L", str(path)], capture_output=True, text=True, check=True).stdout
    deps = []
    for line in out.splitlines()[1:]:  # first line is the file path itself
        m = re.match(r"^\s*(\S+)\s+\(compatibility", line)
        if m:
            deps.append(m.group(1))
    return deps


def resign(path: Path) -> None:
    # install_name_tool invalidates any existing signature; ad-hoc re-sign so
    # the file loads under Gatekeeper/hardened runtime after this rewrite.
    subprocess.run(["codesign", "--force", "--sign", "-", str(path)], check=True)


def existing_rpaths(path: Path) -> set[str]:
    out = subprocess.run(["otool", "-l", str(path)], capture_output=True, text=True, check=True).stdout
    return set(re.findall(r"path (\S+) \(offset", out))


def ensure_loader_path_rpath(path: Path) -> None:
    # @rpath/<dep> only resolves if something in the load chain contributes a
    # matching LC_RPATH. A host app bundle sets one up automatically, but a
    # bare dlopen() of this file straight out of its build/vendor directory
    # (e.g. Halcyon's `flutter test`, run from source, not from inside a
    # built .app) does not -- so the dylib must carry its own, pointing at
    # the directory it and its vendored siblings live in.
    if "@loader_path" not in existing_rpaths(path):
        subprocess.run(["install_name_tool", "-add_rpath", "@loader_path", str(path)], check=True)


def bundle(dylib: Path, dest_dir: Path) -> None:
    queue = [dylib]
    seen = set()
    while queue:
        current = queue.pop()
        rewrote = False
        for dep in otool_deps(current):
            if dep.startswith(KNOWN_SAFE_PREFIXES):
                continue
            if not dep.startswith(HOMEBREW_PREFIXES):
                # BUNDLER-BLIND-SPOT fix (2026-09-01): this branch used to be
                # a silent `continue`. That silence is exactly how a broken
                # release artifact reached a green CI run: a raw Homebrew
                # bottle's own install name is an UNSUBSTITUTED relocation
                # placeholder token of the literal form
                # "@@HOMEBREW_PREFIX@@/opt/<formula>/lib/<name>.dylib" (only
                # `brew install` itself rewrites that token to a real path;
                # extracting a bottle without running `brew` leaves it as-is)
                # -- which does not start with either Homebrew prefix above,
                # so this check always let it straight through unexamined,
                # and it got linked into the decoder and shipped verbatim.
                # A dependency this script cannot classify is a shape nobody
                # anticipated, which is precisely the failure this project
                # cannot silently ignore in a release-asset-producing step:
                # hard error, not a warning, because a warning is exactly
                # the kind of output this same defect already proved can
                # sit unnoticed in a green run's log.
                print(
                    f"[bundle] ERROR: {current} depends on {dep!r}, which this "
                    "script cannot classify (not @rpath/@loader_path/"
                    "@executable_path, not /usr/lib or /System, not a "
                    "Homebrew-prefixed absolute path). This is exactly the "
                    "shape of an unsubstituted package-manager relocation "
                    "token (e.g. a raw Homebrew bottle's own "
                    "'@@HOMEBREW_PREFIX@@/...' install name) and would ship "
                    "an unresolvable dependency reference in the release "
                    "asset. Refusing to continue -- fix the dependency's "
                    "recorded install name (e.g. vendor-and-rewrite it "
                    "before linking) rather than relaxing this check.",
                    file=sys.stderr,
                )
                sys.exit(1)
            name = Path(dep).name
            dest = dest_dir / name
            if name not in seen:
                seen.add(name)
                if not dest.exists():
                    shutil.copy2(dep, dest)
                    dest.chmod(0o755)
                    subprocess.run(["install_name_tool", "-id", f"@rpath/{name}", str(dest)], check=True)
                    resign(dest)
                    print(f"[bundle] vendored {dep} -> {dest}")
                queue.append(dest)
            subprocess.run(
                ["install_name_tool", "-change", dep, f"@rpath/{name}", str(current)], check=True
            )
            rewrote = True
        if rewrote:
            ensure_loader_path_rpath(current)
            resign(current)
            print(f"[bundle] repointed Homebrew deps in {current}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: bundle_macos_dylib_deps.py <dylib>", file=sys.stderr)
        return 2
    dylib = Path(sys.argv[1]).resolve()
    if not dylib.exists():
        print(f"[bundle] ERROR: {dylib} not found", file=sys.stderr)
        return 1
    bundle(dylib, dylib.parent)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
