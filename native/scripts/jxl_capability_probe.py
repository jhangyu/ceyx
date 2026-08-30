#!/usr/bin/env python3
"""JXL FFI capability gate (task #9 fix, per docs/logs/2026-08-31/r5-jxl-diagnosis.md).

Replaces the earlier `nm`-grep assertion, which tested an unsatisfiable claim:
JxlEncoderInitBasicInfo is compiled into the vendored static libjxl.a with
hidden visibility (JXL_STATIC_DEFINE -> JXL_EXPORT expands to nothing), so it
can NEVER appear in any export table regardless of whether the codec is
correctly linked in. See the diagnosis doc for the full mechanical evidence
(nm -m on the archive vs. the linked artifact, a disabled-build control, and
the source-level cause in jxl_export.h).

The correct instrument is our OWN exported FFI capability surface:
ceyx_still_decode_supports(kCeyxFormatJxl) and ceyx_encode_supports(kCeyxFormatJxl)
both return CEYX_ENABLE_JXL ? 1 : 0 at the point they were compiled — a green
link with CEYX_ENABLE_JXL=1 entails libjxl.a was actually linked (a mismatched
link would fail with undefined Jxl* symbols, not succeed with the wrong
answer). This script dlopen()s the built shared library, calls both exports,
and asserts they equal --expect for this leg (1 where JXL was fetched and
enabled, 0 where the leg took the explicit-OFF branch).

Only meaningful when loaded on the library's OWN target architecture (a
same-arch, same-OS process) — see the workflow comments at each call site for
which legs that excludes (cross-compiled outputs cannot be dlopen()'d from a
foreign-arch host process).
"""
import argparse
import ctypes
import sys

# native/include/ceyx_encode_api.h:89
K_CEYX_FORMAT_JXL = 5


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("lib_path", help="Path to the built dng_decoder_native shared library")
    ap.add_argument("--expect", type=int, required=True, choices=[0, 1],
                     help="Expected return value of both capability queries for this leg")
    args = ap.parse_args()

    try:
        lib = ctypes.CDLL(args.lib_path)
    except OSError as exc:
        print(f"::error::dlopen failed for {args.lib_path}: {exc}", file=sys.stderr)
        return 1

    lib.ceyx_still_decode_supports.argtypes = [ctypes.c_int32]
    lib.ceyx_still_decode_supports.restype = ctypes.c_int32
    lib.ceyx_encode_supports.argtypes = [ctypes.c_int32]
    lib.ceyx_encode_supports.restype = ctypes.c_int32

    decode_supports = lib.ceyx_still_decode_supports(K_CEYX_FORMAT_JXL)
    encode_supports = lib.ceyx_encode_supports(K_CEYX_FORMAT_JXL)

    print(f"ceyx_still_decode_supports(kCeyxFormatJxl)={decode_supports}")
    print(f"ceyx_encode_supports(kCeyxFormatJxl)={encode_supports}")

    decode_ok = decode_supports == args.expect
    encode_ok = encode_supports == args.expect
    print(f"EXPECT={args.expect} STILL_MATCH={decode_ok} ENCODE_MATCH={encode_ok}")

    if not (decode_ok and encode_ok):
        print(
            f"::error::JXL capability mismatch in {args.lib_path}: "
            f"expected both queries == {args.expect}, got "
            f"still={decode_supports} encode={encode_supports}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
