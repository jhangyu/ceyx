#!/bin/bash
# Fetch the vendored Halide v21.0.0 binary distribution into
# native/third_party/halide/.
#
# Provenance (was tracked in third_party/halide/VERSION before the 2026-07-05
# history rewrite removed the ~540MB binary payload from git):
#   halide/Halide@v21.0.0
#   https://github.com/halide/Halide/releases/tag/v21.0.0
#   abi_notes: schedule changes break AOT artifacts; bump requires full regen.
#
# Run once after a fresh clone (CMake configure fails without it).
# The Vulkan runtime FORK sources are a separate concern — see
# halide_runtime_fork/scripts/fetch_halide_v21_runtime.sh.
set -eu
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEST="$NATIVE_DIR/third_party/halide"

# W1 (2026-08-21, Windows port): the asset used to be hard-coded to
# arm-64-osx. Select it from `uname` instead so the same script works on the
# Windows build host (Git Bash / MSYS2) and on Intel macOS. Every name below
# was verified against the live release listing
# (https://api.github.com/repos/halide/Halide/releases/tags/v21.0.0) — all
# share the same b629c80... build commit, so the ABI matches the pinned
# version in third_party/halide/VERSION.
HALIDE_COMMIT="b629c80de18f1534ec71fddd8b567aa7027a0876"
OS_NAME="$(uname -s)"
ARCH_NAME="$(uname -m)"

case "$OS_NAME" in
    Darwin)
        case "$ARCH_NAME" in
            arm64|aarch64) PLATFORM="arm-64-osx" ;;
            x86_64)        PLATFORM="x86-64-osx" ;;
            *) echo "Unsupported macOS arch: $ARCH_NAME" >&2; exit 1 ;;
        esac
        ARCHIVE_EXT="tar.gz"
        ;;
    Linux)
        case "$ARCH_NAME" in
            aarch64|arm64) PLATFORM="arm-64-linux" ;;
            x86_64)        PLATFORM="x86-64-linux" ;;
            *) echo "Unsupported Linux arch: $ARCH_NAME" >&2; exit 1 ;;
        esac
        ARCHIVE_EXT="tar.gz"
        ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        # MSYS/Git-Bash report x86_64 on 64-bit Windows.
        case "$ARCH_NAME" in
            x86_64|amd64|AMD64) PLATFORM="x86-64-windows" ;;
            i686|i386)          PLATFORM="x86-32-windows" ;;
            *) echo "Unsupported Windows arch: $ARCH_NAME" >&2; exit 1 ;;
        esac
        ARCHIVE_EXT="zip"
        ;;
    *)
        echo "Unsupported host OS: $OS_NAME" >&2
        exit 1
        ;;
esac

ASSET="Halide-21.0.0-${PLATFORM}-${HALIDE_COMMIT}.${ARCHIVE_EXT}"
URL="https://github.com/halide/Halide/releases/download/v21.0.0/$ASSET"

# Windows ships an import library (Halide.lib), POSIX hosts a static archive.
if [ -f "$DEST/lib/libHalide.a" ] || [ -f "$DEST/lib/Halide.lib" ]; then
    echo "third_party/halide already present ($DEST) — nothing to do."
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
echo "Downloading $ASSET (host: $OS_NAME/$ARCH_NAME) ..."
curl -fL -o "$TMP/$ASSET" "$URL"
mkdir -p "$DEST"
if [ "$ARCHIVE_EXT" = "zip" ]; then
    # No tar --strip-components equivalent for unzip: extract to a staging dir
    # and hoist the single top-level directory's contents into $DEST.
    mkdir -p "$TMP/x"
    if command -v unzip >/dev/null 2>&1; then
        unzip -q "$TMP/$ASSET" -d "$TMP/x"
    else
        powershell.exe -NoProfile -Command \
            "Expand-Archive -LiteralPath '$TMP/$ASSET' -DestinationPath '$TMP/x' -Force"
    fi
    TOP="$(find "$TMP/x" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
    if [ -z "$TOP" ]; then
        echo "Unexpected archive layout in $ASSET" >&2
        exit 1
    fi
    # `mv "$TOP"/* ` misses dotfiles; use cp -R on the directory contents.
    (cd "$TOP" && tar -cf - .) | (cd "$DEST" && tar -xf -)
else
    tar -xzf "$TMP/$ASSET" -C "$DEST" --strip-components=1
fi
printf 'halide/Halide@v21.0.0\nbinary_provenance: vendored from https://github.com/halide/Halide/releases/tag/v21.0.0\nasset: %s\nabi_notes: schedule changes break AOT artifacts; bump requires full regen.\n' "$ASSET" > "$DEST/VERSION"
echo "OK — $(du -sh "$DEST" | cut -f1) at $DEST"
