#!/usr/bin/env python3
"""Codec capability gate: dlopen the built library and ask it what it supports.

Generalises the original single-format JXL-only capability probe (task #9,
per docs/logs/2026-08-31/r5-jxl-diagnosis.md; that script was retired in
round 3 once every call site was rewired to this one -- see
native/deps/codec_expectations.toml and render_expectations.py) from one
format to all five, because spec-windows-codec-full-green.md AC1 names five
formats in two directions.

WHY NOT A SYMBOL GREP -- the reasoning is unchanged and load-bearing on every
platform, so it is repeated here rather than referenced:

  A third-party codec's public symbols are hidden by construction in a static
  build (libjxl: JXL_STATIC_DEFINE makes JXL_EXPORT expand to nothing;
  libheif: WITH_REDUCED_VISIBILITY=ON), so they can NEVER appear in any export
  table of the linked library no matter how correctly the codec is linked in.
  On Windows nothing is exported by default at all. An export-table check
  therefore returns the same answer for a correct build and a soft-degraded
  one: it has zero discriminating power.

  The correct instrument is our OWN exported capability surface.
  ceyx_still_decode_supports / ceyx_encode_supports are compiled from the same
  preprocessor flags that gate the codec routes, and a green link with a codec
  enabled entails the codec's archive was actually linked (a mismatched link
  fails with undefined symbols, not with a wrong answer).

Only meaningful when loaded on the library's OWN target architecture: a
cross-compiled artifact cannot be dlopen'd from a foreign-arch host process.
The macOS x86_64 cross leg therefore keeps its configure-log assertion instead
of calling this script.
"""
import argparse
import ctypes
import sys

# native/include/ceyx_encode_api.h:84-90. Append-only; values never reused.
FORMATS = {
    "jpeg": 1,
    "webp": 2,
    "heic": 3,
    "avif": 4,
    "jxl": 5,
}

DIRECTIONS = ("encode", "decode")

# WI-5 (docs/logs/2026-09-12/platform-parity-plan.md step 5.3): the build
# capability surface (broader than codec formats -- "what is compiled into
# this artifact", not "which codec direction works"), queried via ceyx's own
# ceyx_build_capabilities/ceyx_build_capability_names exports (step 5.1).
# Kept as a fixed, known set here (rather than trusting the library's runtime
# name list blindly) so a typo'd --expect-cap name fails loudly as "unknown"
# instead of silently asking the library about a name it happens to answer
# -1 for anyway; the two end up equivalent for an unknown name, but this list
# is also what parse_capability_expectation validates argv against before
# ever touching the library.
CAPABILITY_NAMES = ("ICC", "OPENMP", "HEIF", "WEBP", "JXL", "RAW")


class ProbeError(Exception):
    """Raised when the instrument itself could not run (not a capability verdict)."""


def parse_expectation(text):
    """'heic:encode=1' -> ('heic', 'encode', 1). Raises ValueError on anything else."""
    try:
        lhs, rhs = text.split("=", 1)
        fmt, direction = lhs.split(":", 1)
        value = int(rhs)
    except ValueError:
        raise ValueError(f"malformed expectation {text!r}; want <format>:<direction>=<0|1>")
    if fmt not in FORMATS:
        raise ValueError(f"unknown format {fmt!r}; known: {', '.join(sorted(FORMATS))}")
    if direction not in DIRECTIONS:
        raise ValueError(f"unknown direction {direction!r}; want encode or decode")
    if value not in (0, 1):
        raise ValueError(f"expectation value must be 0 or 1, got {value}")
    return fmt, direction, value


def parse_capability_expectation(text):
    """'ICC=0' -> ('ICC', 0). Raises ValueError on anything else."""
    try:
        name, rhs = text.split("=", 1)
        value = int(rhs)
    except ValueError:
        raise ValueError(f"malformed capability expectation {text!r}; want <NAME>=<0|1>")
    if name not in CAPABILITY_NAMES:
        raise ValueError(f"unknown capability {name!r}; known: {', '.join(CAPABILITY_NAMES)}")
    if value not in (0, 1):
        raise ValueError(f"capability expectation value must be 0 or 1, got {value}")
    return name, value


def load_library(path):
    """dlopen the artifact and bind both capability entry points.

    Raises ProbeError with a diagnosable message on dlopen failure or a
    missing capability export (distinct from a capability mismatch: this
    means the instrument itself could not run).
    """
    try:
        lib = ctypes.CDLL(path)
    except OSError as exc:
        raise ProbeError(f"dlopen failed for {path}: {exc}") from exc
    for name in ("ceyx_still_decode_supports", "ceyx_encode_supports"):
        try:
            fn = getattr(lib, name)
        except AttributeError as exc:
            raise ProbeError(
                f"{path} does not export the capability surface: {exc}"
            ) from exc
        fn.argtypes = [ctypes.c_int32]
        fn.restype = ctypes.c_int32
    return lib


def load_build_capabilities(lib, path):
    """Bind ceyx_build_capabilities on an already-loaded lib (WI-5, step 5.1).

    A separate bind step from load_library() because this surface has no
    Dart lookupFunction site and is asserted only by this probe (WI-5 plan,
    "Open items for lead" (b)) -- callers that never ask a --expect-cap
    question should not pay for (or fail on) a missing build-capability
    export. Raises ProbeError, same instrument-vs-verdict distinction as the
    codec surface.
    """
    try:
        fn = lib.ceyx_build_capabilities
    except AttributeError as exc:
        raise ProbeError(
            f"{path} does not export ceyx_build_capabilities: {exc}"
        ) from exc
    fn.argtypes = [ctypes.c_char_p]
    fn.restype = ctypes.c_int32
    return fn


def probe(lib, fmt, direction):
    """Return the raw reported value for (fmt, direction) from an already-loaded lib."""
    fn = lib.ceyx_encode_supports if direction == "encode" else lib.ceyx_still_decode_supports
    return fn(FORMATS[fmt])


def probe_capability(cap_fn, name):
    """Return the raw reported value (1/0/-1) for a build capability name."""
    return cap_fn(name.encode("ascii"))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lib_path", help="Path to the built dng_decoder_native shared library")
    ap.add_argument("--expect", action="append", default=[], metavar="FMT:DIR=VAL",
                    help="Expected codec value, repeatable. Example: --expect heic:encode=1")
    ap.add_argument("--expect-cap", action="append", default=[], metavar="NAME=VAL",
                    help="Expected build capability value (WI-5), repeatable. "
                         "Example: --expect-cap ICC=0")
    ap.add_argument("--json-out", metavar="PATH",
                    help="Write a machine-readable result file, even on failure")
    args = ap.parse_args(argv)

    if not args.expect and not args.expect_cap:
        ap.error("at least one of --expect or --expect-cap is required")

    try:
        expectations = [parse_expectation(e) for e in args.expect]
        cap_expectations = [parse_capability_expectation(e) for e in args.expect_cap]
    except ValueError as exc:
        print(f"::error::{exc}", file=sys.stderr)
        return 2

    try:
        lib = load_library(args.lib_path) if expectations else ctypes.CDLL(args.lib_path)
        cap_fn = load_build_capabilities(lib, args.lib_path) if cap_expectations else None
    except OSError as exc:
        exc = ProbeError(f"dlopen failed for {args.lib_path}: {exc}")
        print(f"::error::{exc}", file=sys.stderr)
        if args.json_out:
            import json
            with open(args.json_out, "w") as f:
                json.dump({
                    "library": args.lib_path,
                    "error": str(exc),
                    "results": [],
                    "ok": False,
                }, f, indent=2)
        return 1
    except ProbeError as exc:
        print(f"::error::{exc}", file=sys.stderr)
        if args.json_out:
            import json
            with open(args.json_out, "w") as f:
                json.dump({
                    "library": args.lib_path,
                    "error": str(exc),
                    "results": [],
                    "ok": False,
                }, f, indent=2)
        return 1

    # Report EVERY mismatch, never just the first: one run should answer every
    # hypothesis. Stopping at the first turns a single diagnosis into N rounds.
    results = []
    failures = []
    for fmt, direction, want in expectations:
        got = probe(lib, fmt, direction)
        ok = got == want
        status = "OK" if ok else "MISMATCH"
        print(f"{status:9s} {fmt}:{direction} want={want} got={got}")
        results.append({
            "format": fmt,
            "direction": direction,
            "got": got,
            "expected": want,
            "ok": ok,
        })
        if not ok:
            failures.append(f"{fmt}:{direction} want={want} got={got}")

    # A capability name the ledger asserts but the library does not know
    # (-1) is UNVERIFIED and exits non-zero -- not the same as a confirmed 0
    # (WI-5 plan step 5.3): "the instrument could not answer" must never read
    # as "capability absent".
    cap_results = []
    for name, want in cap_expectations:
        got = probe_capability(cap_fn, name)
        if got == -1:
            status = "UNVERIFIED"
            ok = False
        else:
            ok = got == want
            status = "OK" if ok else "MISMATCH"
        print(f"{status:9s} capability:{name} want={want} got={got}")
        cap_results.append({
            "capability": name,
            "got": got,
            "expected": want,
            "ok": ok,
        })
        if not ok:
            failures.append(f"capability:{name} want={want} got={got} status={status}")

    overall_ok = not failures

    if args.json_out:
        import json
        with open(args.json_out, "w") as f:
            json.dump({
                "library": args.lib_path,
                "results": results,
                "capability_results": cap_results,
                "ok": overall_ok,
            }, f, indent=2)

    if failures:
        for f in failures:
            print(f"::error::capability mismatch: {f}", file=sys.stderr)
        print("::error::Do NOT relax an expectation to go green. A 0 where 1 was "
              "expected means the codec is genuinely absent from this artifact: "
              "fix the dist. A 1 where 0 was expected means the capability arm is "
              "lying about a codec it does not have. An UNVERIFIED build "
              "capability means the instrument itself could not answer -- that "
              "is not the same as a confirmed 0.", file=sys.stderr)
        return 1

    print(f"all {len(expectations) + len(cap_expectations)} capability expectations hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
