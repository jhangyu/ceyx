#!/usr/bin/env python3
"""Fail if an x86-64 binary contains AVX-512 (EVEX-encoded) instructions.

WHY THIS EXISTS
---------------
2026-09-08: the published v0.1.19 Linux libdng_decoder_native.so SIGILL'd at
dlopen() on machines without AVX-512. Root cause: RawSpeed3's CpuMarch.cmake
compiled with `-march=native` on native (non-cross) builds, so an AVX-512
GitHub runner baked EVEX instructions into the shipped artifact -- some of them
on paths reached during ELF static initialisation, i.e. before any of our own
code can run, so no runtime feature check could have saved it.

The compile-side fix is in native/cmake/tests.cmake ("PORTABLE-BASELINE") and
native/cmake/halide_aot.cmake. This script is the mechanical gate that stops
the same class of defect from reaching a release again: it looks at the
BINARY, not at the build flags, so any future path that reintroduces
host-derived codegen (a new subproject, a new -march, a new Halide target)
is caught by the artifact itself.

INSTRUMENT NOTES
----------------
* The disassembly is written to a FILE and then scanned, never piped into
  grep: under `set -o pipefail`, `objdump | grep -q` returns 141 when grep
  exits early on a match and objdump takes SIGPIPE -- an INVERTED verdict
  (found => "failed"). Doing the scan in Python removes the hazard entirely.
* Detection is by operand syntax, which covers all three EVEX tells:
    - %zmm0-%zmm31          512-bit registers
    - {%k0}-{%k7}           opmask operands (AVX-512 VL forms on xmm/ymm)
    - %xmm16-31/%ymm16-31   high register file, EVEX-only encoding
  The last two matter: AVX-512VL code operating on ymm/xmm registers is just
  as illegal on a non-AVX-512 CPU as a zmm instruction, and a zmm-only check
  would have missed 148 of the 254 hits in the v0.1.19 artifact.
* SCOPE: point this at libdng_decoder_native.so ONLY, never at the companion
  libraries. It cannot distinguish "unconditional AVX-512" from "AVX-512
  behind a runtime cpuid dispatch", and the latter is legitimate: the shipped
  libde265.so.0 has 859 EVEX lines, all inside `transform_32x32_add_8_avx512`
  and its callee, reached only through libde265's own CPU-feature dispatch.
  libheif.so.1 from the same tarball has 0 and is the natural PASS control.
  Our own decoder has no such dispatch layer, so for it "any EVEX" is exactly
  the right predicate.
* Anything that prevents the check from running (no objdump, disassembly
  failure, wrong architecture) is a FAILURE, not a skip: an unverified
  artifact must not be publishable.

USAGE
    python3 native/scripts/assert_no_avx512.py <binary> [--dump <path>]
Exit codes: 0 = clean, 1 = AVX-512 found, 2 = could not verify.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from typing import NoReturn

# %zmmNN | {%kN} | %xmm/%ymm 16..31
EVEX_RE = re.compile(r"%zmm\d+|\{%k[0-7]\}|%[xy]mm(?:1[6-9]|2\d|3[01])\b")

CANDIDATE_TOOLS = ("llvm-objdump", "objdump", "gobjdump")


def fail(msg: str, code: int = 2) -> NoReturn:
    print(f"AVX512-GATE: UNVERIFIED -- {msg}", file=sys.stderr)
    sys.exit(code)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--dump", help="where to write the disassembly (default: <binary>.objdump.txt)")
    ap.add_argument("--objdump", help="disassembler to use (default: first of %s on PATH)"
                                      % ", ".join(CANDIDATE_TOOLS))
    ap.add_argument("--max-examples", type=int, default=10)
    args = ap.parse_args()

    binary = args.binary
    if not os.path.isfile(binary):
        fail(f"no such file: {binary}")

    tool = args.objdump
    if tool is None:
        for cand in CANDIDATE_TOOLS:
            if shutil.which(cand):
                tool = cand
                break
    if tool is None:
        fail("no disassembler on PATH (tried: %s)" % ", ".join(CANDIDATE_TOOLS))

    dump_path = args.dump or (binary + ".objdump.txt")

    # Architecture guard: this gate only means anything for x86-64. A non-x86
    # binary is NOT silently passed -- it is refused, so a platform change can
    # never turn this gate into a no-op that still prints green.
    hdr = subprocess.run([tool, "-f", binary], capture_output=True, text=True)
    if hdr.returncode != 0:
        fail(f"{tool} -f failed (rc={hdr.returncode}): {hdr.stderr.strip()[:400]}")
    arch_line = hdr.stdout
    if not re.search(r"x86[-_]?64|i386:x86-64", arch_line, re.IGNORECASE):
        fail(f"{binary} is not an x86-64 object per `{tool} -f`; this gate is x86-64 only.\n{arch_line}")

    with open(dump_path, "w") as fh:
        proc = subprocess.run([tool, "-d", binary], stdout=fh, stderr=subprocess.PIPE, text=True)
    rc = proc.returncode
    print(f"AVX512-GATE: tool={tool} disassemble_rc={rc} dump={dump_path}")
    if rc != 0:
        fail(f"{tool} -d failed (rc={rc}): {proc.stderr.strip()[:400]}")

    hits = []
    current_symbol = "<unknown>"
    sym_re = re.compile(r"^[0-9a-f]+\s+<(.+)>:")
    with open(dump_path, "r", errors="replace") as fh:
        for line in fh:
            m = sym_re.match(line)
            if m:
                current_symbol = m.group(1)
                continue
            if EVEX_RE.search(line):
                hits.append((current_symbol, line.rstrip()))

    symbols = sorted({s for s, _ in hits})
    print(f"AVX512-GATE: file={binary} evex_lines={len(hits)} distinct_symbols={len(symbols)}")
    if not hits:
        print("AVX512-GATE: VERDICT=PASS (no AVX-512/EVEX instructions found)")
        return 0

    for sym, line in hits[: args.max_examples]:
        print(f"  {sym}: {line.strip()}")
    for sym in symbols[: args.max_examples]:
        print(f"  symbol: {sym}")
    sys.stdout.flush()
    print(
        "AVX512-GATE: VERDICT=FAIL -- this artifact requires AVX-512 and will "
        "SIGILL on CPUs without it. Something in the build is compiling for the "
        "build machine (see PORTABLE-BASELINE in native/cmake/tests.cmake).",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
