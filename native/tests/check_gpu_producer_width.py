#!/usr/bin/env python3
r"""Mechanical gate: every materialised GPU producer must be >= 32 bits wide.

WHY THIS EXISTS
---------------
DngRenderGenerator.cpp:525-556 records a MEASURED defect: a Halide Func
materialised at a GPU loop level becomes an array in the Workgroup storage
class, and the Adreno 750 Vulkan driver miscompiles that array when its element
type is narrower than 32 bits. Two schedules identical character-for-character
apart from the staged type, same binary, same run, 6000x4000: uint8 gives
G/B = 24,000,000/24,000,000 wrong; uint32 gives 0. The failure is SILENT --
kernel return code 0, no crash, just wrong pixels.

That file's own conclusion: "any future materialised producer here needs a
mechanical element-width check, not a reviewer's memory." Until this script
existed there was no such check anywhere in the tree -- the rule lived only as
prose in RawBayerDemosaicGenerator.cpp:55-72 and in that comment. This is the
enforcer.

WHAT IT READS, AND WHY THAT INSTRUMENT
--------------------------------------
It reads the LOWERED HALIDE STATEMENT (`-e stmt`), never the generator source.
"I wrote compute_at" is not evidence about what materialises; the plan's global
constraint is explicit that materialisation claims come from the lowered
statement or the artifact.

The specific quantity is the shared-memory argument of the dispatch call:

  halide_metal_run (metal[0],  "_kernel_...", t276, t275, 1, 18, 18, 1, 0 + (324*4), ...)
  halide_vulkan_run(vulkan[0], "_kernel_...", t276, t275, 1, 18, 18, 1, 0 + (324*4), ...)
                                              \_ grid _/  \_ block _/  \_ shared _/

`(324*4)` is `<element count> * <element width in bytes>`. Narrowing the staged
producer to uint16_t changes it to `(324*2)`, which this script rejects.

This quantity was chosen over parsing the embedded kernel source because it is
the ONLY form that is textually identical on both backend families: the Metal
arm embeds readable Metal C++ (`threadgroup int4* _normalized_0`), but the
Vulkan arm embeds SPIR-V, where the type is an OpTypeArray id and is not
greppable. A check that only worked on Metal would be exactly the
backend-divergent verification the campaign's no-divergence rule forbids --
it would leave the defect's ONLY known host platform family unchecked.

KNOWN LIMITS, stated rather than buried
---------------------------------------
* This gate proves a width, not correctness. It cannot detect a wrong value
  that is 32 bits wide.
* `0 + (N*W)` is the form Halide v21 emits for a single staged producer. If a
  future schedule stages two producers the arithmetic may nest differently;
  --strict makes an unparseable shared-memory expression a FAILURE rather than
  a skip, and --strict is what the acceptance gate runs with. An unrecognised
  form must be read by a human, not silently passed.
* The device-allocation half (`--max-dispatches`, `--forbid-device-malloc`) is
  reported honestly: on every archive inspected so far the kernel never calls
  device_malloc at all, because the buffers are caller-supplied. So
  --forbid-device-malloc is a WEAK assertion here and is labelled as such in the
  output; the load-bearing allocation evidence is the dispatch count plus the
  host-side arena counters, not this grep.
"""

import argparse
import re
import sys

DISPATCH_RE = re.compile(
    r"halide_(?P<backend>metal|vulkan)_run\s*\(", re.MULTILINE)

# The shared-memory argument, as emitted by Halide v21 for a staged producer.
# Anchored on the three block-dim integers that immediately precede it so a
# stray "N*W" elsewhere in the line cannot be mistaken for it.
SHARED_RE = re.compile(
    r",\s*(?P<bx>\d+|[A-Za-z_][\w.]*)"     # blocks x
    r",\s*(?P<by>\d+|[A-Za-z_][\w.]*)"     # blocks y
    r",\s*(?P<bz>\d+)"                     # blocks z
    r",\s*(?P<tx>\d+)"                     # threads x
    r",\s*(?P<ty>\d+)"                     # threads y
    r",\s*(?P<tz>\d+)"                     # threads z
    r",\s*(?P<shared>[^,]+?)\s*,")         # shared memory bytes
SHARED_STAGED_RE = re.compile(
    r"^\s*(?P<base>\d+)\s*\+\s*\(\s*(?P<elems>\d+)\s*\*\s*(?P<width>\d+)\s*\)\s*$")
SHARED_ZERO_RE = re.compile(r"^\s*0\s*$")

MIN_WIDTH_BYTES = 4  # 32 bits -- the measured threshold, not a preference


def dispatch_lines(text):
    """Yield (backend, full dispatch call text) for each GPU dispatch."""
    for match in DISPATCH_RE.finditer(text):
        start = match.start()
        line_end = text.find("\n", start)
        if line_end == -1:
            line_end = len(text)
        yield match.group("backend"), text[start:line_end]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("stmt", help="path to a lowered statement file (-e stmt)")
    parser.add_argument("--require-staged-producer", action="store_true",
                        help="fail unless at least one dispatch stages a producer "
                             "in shared memory (the fusion property)")
    parser.add_argument("--forbid-staged-producer", action="store_true",
                        help="inverse; used to prove this gate can go red")
    parser.add_argument("--max-dispatches", type=int, default=None,
                        help="fail if the archive issues more GPU dispatches than this")
    parser.add_argument("--forbid-device-malloc", action="store_true",
                        help="fail on any device_malloc in the lowered statement "
                             "(weak: see the module docstring)")
    parser.add_argument("--strict", action="store_true",
                        help="treat an unparseable shared-memory expression as a "
                             "FAILURE instead of a skip")
    args = parser.parse_args()

    try:
        with open(args.stmt, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    except OSError as error:
        print("[GpuProducerWidth] cannot read %s: %s" % (args.stmt, error))
        return 2

    failures = []
    dispatches = list(dispatch_lines(text))
    staged = 0
    print("[GpuProducerWidth] stmt=%s dispatches=%d" % (args.stmt, len(dispatches)))

    if not dispatches:
        # A statement with no GPU dispatch cannot be evidence about GPU
        # producers. Refusing is the point: silently passing here is how a
        # gate becomes an assertion that cannot fail.
        failures.append("no GPU dispatch found -- this statement cannot answer "
                        "the question being asked (wrong target? CPU-only build?)")

    for index, (backend, call) in enumerate(dispatches):
        shared_match = SHARED_RE.search(call)
        if not shared_match:
            message = ("dispatch %d (%s): could not locate the shared-memory "
                       "argument" % (index, backend))
            if args.strict:
                failures.append(message)
            else:
                print("[GpuProducerWidth] SKIP %s" % message)
            continue
        shared = shared_match.group("shared")
        if SHARED_ZERO_RE.match(shared):
            print("[GpuProducerWidth] dispatch %d (%s): shared=0 -- no staged "
                  "producer (fully inlined)" % (index, backend))
            continue
        staged_match = SHARED_STAGED_RE.match(shared)
        if not staged_match:
            message = ("dispatch %d (%s): unrecognised shared-memory expression "
                       "%r -- a human must read this" % (index, backend, shared))
            if args.strict:
                failures.append(message)
            else:
                print("[GpuProducerWidth] SKIP %s" % message)
            continue
        elems = int(staged_match.group("elems"))
        width = int(staged_match.group("width"))
        staged += 1
        verdict = "PASS" if width >= MIN_WIDTH_BYTES else "FAIL"
        print("[GpuProducerWidth] dispatch %d (%s): staged producer "
              "elements=%d element_width_bytes=%d (min %d) -> %s"
              % (index, backend, elems, width, MIN_WIDTH_BYTES, verdict))
        if width < MIN_WIDTH_BYTES:
            failures.append(
                "dispatch %d (%s): staged producer element width %d bytes < %d. "
                "A sub-32-bit array in the Workgroup storage class is miscompiled "
                "by the Adreno Vulkan driver, SILENTLY (see "
                "DngRenderGenerator.cpp:525-556)."
                % (index, backend, width, MIN_WIDTH_BYTES))

    if args.require_staged_producer and staged == 0:
        failures.append("no dispatch stages a producer in shared memory; the "
                        "fused schedule is expected to stage the demosaic "
                        "producer on-chip")
    if args.forbid_staged_producer and staged != 0:
        failures.append("%d dispatch(es) stage a producer, but none was expected"
                        % staged)
    if args.max_dispatches is not None and len(dispatches) > args.max_dispatches:
        failures.append("%d GPU dispatches > --max-dispatches %d"
                        % (len(dispatches), args.max_dispatches))
    if args.forbid_device_malloc:
        # Counted on the whole text, then reported. `grep -c` counts LINES, not
        # occurrences (lesson 2026-08-17), so occurrences are counted directly.
        occurrences = len(re.findall(r"device_malloc", text))
        print("[GpuProducerWidth] device_malloc occurrences=%d "
              "(WEAK assertion -- see module docstring)" % occurrences)
        if occurrences:
            failures.append("device_malloc occurs %d time(s)" % occurrences)

    for failure in failures:
        print("[GpuProducerWidth] FAIL: %s" % failure)
    print("[GpuProducerWidth] staged_producers=%d failures=%d -> %s"
          % (staged, len(failures), "PASS" if not failures else "FAIL"))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
