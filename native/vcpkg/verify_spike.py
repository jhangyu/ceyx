#!/usr/bin/env python3
"""Mechanical acceptance gate for the OD-2 overlay-port spike.

Checks the ARTEFACT and the CONFIGURE LOG, never the intent (pitfall N21:
"the variable being set and the policy honouring it are different facts").

Usage:
    python3 verify_spike.py --vcpkg-root <dir> --install-root <dir> --triplet <t>

Exit code 0 = all assertions pass. Every assertion prints one line beginning
with "PASS " or "FAIL " so the result is greppable and countable, and the final
line is "RESULT ok=<n> failed=<n> RC=<code>".

No shell is used anywhere in this file (R-5).
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# --- libheif's own configure-time verdict on the kvazaar backend -------------
# libheif/plugins/CMakeLists.txt:28 prints this when a codec is compiled INTO
# libheif; :44 prints the negative form. Both are checked, because asserting
# only the positive cannot distinguish "enabled" from "log truncated".
KVZ_BUILTIN = "Compiling 'Kvazaar HEVC encoder' as built-in backend"
KVZ_ABSENT = "Not compiling 'Kvazaar HEVC encoder' backend"
DE265_BUILTIN = "Compiling 'libde265 HEVC decoder' as built-in backend"

# Dynamic-CRT import names. A /MT-linked binary imports none of these.
DYNAMIC_CRT_MARKERS = (
    "vcruntime140",
    "msvcp140",
    "ucrtbase",
    "api-ms-win-crt-",
    "msvcr",
)


class Results:
    def __init__(self) -> None:
        self.ok = 0
        self.failed = 0

    def check(self, cond: bool, msg: str) -> bool:
        if cond:
            self.ok += 1
            print("PASS " + msg)
        else:
            self.failed += 1
            print("FAIL " + msg)
        return cond

    def skip(self, msg: str) -> None:
        print("SKIP " + msg)


# --------------------------------------------------------------------------
# Minimal PE import-table reader. Only the DLL names are needed.
# --------------------------------------------------------------------------
def pe_imported_dlls(path: Path) -> list[str]:
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE image (no MZ)")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        raise ValueError(f"{path}: bad PE signature")
    coff = pe_off + 4
    n_sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x20B:      # PE32+
        dd = opt + 112
    elif magic == 0x10B:    # PE32
        dd = opt + 96
    else:
        raise ValueError(f"{path}: unknown optional-header magic {magic:#x}")
    import_rva, import_size = struct.unpack_from("<II", data, dd + 8)
    if import_rva == 0 or import_size == 0:
        return []

    sections = []
    sec_off = opt + opt_size
    for i in range(n_sections):
        base = sec_off + i * 40
        vaddr, = struct.unpack_from("<I", data, base + 12)
        vsize, = struct.unpack_from("<I", data, base + 8)
        raw_size, = struct.unpack_from("<I", data, base + 16)
        raw_ptr, = struct.unpack_from("<I", data, base + 20)
        sections.append((vaddr, max(vsize, raw_size), raw_ptr))

    def rva_to_off(rva: int) -> int | None:
        for vaddr, vsize, raw_ptr in sections:
            if vaddr <= rva < vaddr + vsize:
                return raw_ptr + (rva - vaddr)
        return None

    names: list[str] = []
    off = rva_to_off(import_rva)
    if off is None:
        return []
    while True:
        entry = data[off:off + 20]
        if len(entry) < 20 or entry == b"\0" * 20:
            break
        name_rva = struct.unpack_from("<I", entry, 12)[0]
        if name_rva == 0:
            break
        n_off = rva_to_off(name_rva)
        if n_off is None:
            break
        end = data.index(b"\0", n_off)
        names.append(data[n_off:end].decode("ascii", "replace"))
        off += 20
    return names


# --------------------------------------------------------------------------
def find_config_log(vcpkg_root: Path, port: str, triplet: str) -> Path | None:
    bt = vcpkg_root / "buildtrees" / port
    if not bt.is_dir():
        return None
    candidates = sorted(bt.glob(f"config-{triplet}-*-out.log"))
    candidates += sorted(bt.glob("config-*-out.log"))
    return candidates[0] if candidates else None


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--vcpkg-root", required=True, type=Path)
    ap.add_argument("--install-root", required=True, type=Path)
    ap.add_argument("--triplet", required=True)
    args = ap.parse_args()

    r = Results()
    triplet = args.triplet
    is_windows = "windows" in triplet
    prefix = args.install_root / triplet

    r.check(prefix.is_dir(), f"install prefix exists: {prefix}")

    # --- AC3: kvazaar encoder actually enabled in libheif ------------------
    heif_log = find_config_log(args.vcpkg_root, "libheif", triplet)
    if heif_log is None:
        r.check(False, "libheif configure log found")
    else:
        text = read_text(heif_log)
        r.check(KVZ_BUILTIN in text,
                f"libheif configure log contains {KVZ_BUILTIN!r} ({heif_log})")
        r.check(KVZ_ABSENT not in text,
                f"libheif configure log does NOT contain {KVZ_ABSENT!r}")
        r.check(DE265_BUILTIN in text,
                f"libheif configure log contains {DE265_BUILTIN!r} (N27: decoder really linked)")
        # K3: the GPL encoder must not have been found or enabled.
        r.check("Compiling 'x265 HEVC encoder' as built-in backend" not in text,
                "libheif configure log shows NO x265 backend (K3, GPL-2.0 excluded)")

    # --- linkage asymmetry (LGPL vs permissive) ---------------------------
    lib = prefix / "lib"
    binroot = prefix / "bin" if is_windows else prefix / "lib"
    if is_windows:
        shared = {p.name.lower() for p in binroot.glob("*.dll")} if binroot.is_dir() else set()
        r.check(any("heif" in n for n in shared), f"libheif shipped as a DLL (found: {sorted(shared)})")
        r.check(any("de265" in n for n in shared), f"libde265 shipped as a DLL (found: {sorted(shared)})")
        statics = {p.name.lower() for p in lib.glob("*.lib")} if lib.is_dir() else set()
        r.check(any("kvazaar" in n for n in statics), f"kvazaar static lib present (found: {sorted(statics)})")
        r.check(any(n.startswith("aom") or n == "libaom.lib" for n in statics),
                f"aom static lib present (found: {sorted(statics)})")
    else:
        shared = {p.name for p in lib.glob("*.dylib")} | {p.name for p in lib.glob("*.so*")}
        r.check(any("heif" in n for n in shared), f"libheif shipped as a shared library (found: {sorted(shared)})")
        r.check(any("de265" in n for n in shared), f"libde265 shipped as a shared library (found: {sorted(shared)})")
        statics = {p.name for p in lib.glob("*.a")}
        r.check("libkvazaar.a" in statics, f"kvazaar static archive present (found: {sorted(statics)})")
        r.check("libaom.a" in statics, f"aom static archive present (found: {sorted(statics)})")

    # --- Windows blocker (1): the dec265 tool must not be shipped ---------
    if is_windows:
        r.check(not (prefix / "tools" / "libde265").exists(),
                "libde265 dec265 tool NOT installed (Spec §3.2 Windows blocker 1)")

        # --- N21 / layer 6: prove the STATIC CRT from the ARTEFACT --------
        dlls = sorted((prefix / "bin").glob("*.dll")) if (prefix / "bin").is_dir() else []
        r.check(bool(dlls), "at least one DLL produced for the CRT assertion")
        for dll in dlls:
            try:
                imports = [n.lower() for n in pe_imported_dlls(dll)]
            except Exception as exc:  # noqa: BLE001
                r.check(False, f"{dll.name}: PE import table unreadable ({exc})")
                continue
            bad = [n for n in imports if any(m in n for m in DYNAMIC_CRT_MARKERS)]
            r.check(not bad,
                    f"{dll.name}: static CRT — no dynamic-CRT imports "
                    f"(imports={imports}, offending={bad})")

        # Windows blocker (3) corollary: libheif must import libde265's DLL.
        heif_dlls = [d for d in dlls if "heif" in d.name.lower()]
        for d in heif_dlls:
            imports = [n.lower() for n in pe_imported_dlls(d)]
            r.check(any("de265" in n for n in imports),
                    f"{d.name} imports the libde265 DLL (imports={imports})")

    rc = 0 if r.failed == 0 else 1
    print(f"RESULT ok={r.ok} failed={r.failed} RC={rc}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
