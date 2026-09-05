#!/usr/bin/env python3
"""Standing guard: fail the build if Halide's JIT module hooks stop being inert.

WHY THIS EXISTS
---------------
`native/src/pipeline/dng_copy_lock.cpp` replaces Halide's process-wide
`device_copy_mutex` with address-striped mutexes. It carries exactly one
declared deviation from upstream: upstream's `UseModule` helper is not
replicated, because it dereferences `halide_device_interface_impl_t`, which the
public header forward-declares only. That deviation is inert ONLY because
`halide_use_jit_module` / `halide_release_jit_module` are empty stubs in an AOT
build. The source says so itself, at dng_copy_lock.cpp:252-253:

    If ceyx ever links the JIT runtime instead, that pair becomes a real
    refcount and this deviation stops being inert; that is why it is written
    down here.

R4 item 6 probed the whole tree and established that the trigger has NOT fired
(docs/logs/2026-09-05/r2-jit-closure.md, verdict JIT_LINKED_BUT_INERT). This
guard converts that point-in-time finding into a standing one: the day the pair
stops being inert, a build fails and says what is owed, instead of the deviation
becoming live in silence.

WHAT IT DOES
------------
For each image given, it disassembles the DEFINITION the image resolves for the
two JIT hook symbols and classifies the body. It deliberately does NOT scan for
references: reference scanning is blind to table-indirect access, and that blind
spot is what nearly closed item 6 on a false zero.

Exit codes:
  0  every image: both hooks INERT, and the positive control fired.
  1  TRIGGER ARMED: a hook body is non-inert. The crop/slice fork is now owed.
  2  INSTRUMENT UNUSABLE: no image, symbol absent, or the control did not fire.
     NEVER 0. A guard that passes when it measured nothing manufactures
     confidence, which is worse than the absence it is covering for.

The positive control runs on EVERY invocation, not once at install: it
classifies a symbol with a provably non-trivial body (`_halide_copy_to_host`)
using THE SAME classifier, and the guard refuses to report on the JIT pair
unless that control returns NON_INERT. Without it, a classifier that has rotted
into a constant "inert" — a toolchain change, a disassembler format change —
would report a reassuring green forever.

This module is the SINGLE HOME of the classifier. The item-6 probe imports
`classify_body` from here rather than carrying its own copy: two implementations
of one method are one instrument, and their agreement is not corroboration.

Usage:
  check_jit_inert.py <mach-o-image> [<mach-o-image> ...]
"""

import os
import re
import subprocess
import sys

JIT_SYMS = ("_halide_use_jit_module", "_halide_release_jit_module")

# Positive-control symbol: strongly defined by dng_copy_lock.cpp, measured
# NON_INERT (52 instructions) on all 12 subjects of the item-6 probe.
CONTROL_SYM = "_halide_copy_to_host"

# A real refcount must reach a global, so it must contain a load/store/address
# formation or a call. Frame housekeeping (stp/ldp/mov/sub/add/ret/nop/pac*) is
# deliberately excluded, and INERT additionally requires a short body, so a long
# frame-only function cannot be called inert.
NONTRIVIAL_MNEMONICS = {
    "bl", "blr", "b", "br", "cbz", "cbnz", "ldr", "ldrb", "ldrh", "str",
    "strb", "strh", "ldxr", "stxr", "ldadd", "cas", "adrp", "adr",
    "call", "jmp",
}
INERT_MAX_INSNS = 8


def _run(cmd):
    """Run a command with captured output. Never a pipeline.

    Under `set -o pipefail`, `nm ... | grep -q PAT` makes nm take SIGPIPE when
    the pattern IS found, so a found symbol reports as failure. Capture first,
    match afterwards, always.
    """
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        return p.stdout.decode("utf-8", "replace"), p.returncode
    except FileNotFoundError:
        return "", 127
    except OSError:
        return "", 126


def _defines_symbol(nm_out, sym):
    for line in nm_out.splitlines():
        parts = line.split()
        if not parts or parts[-1] != sym:
            continue
        if len(parts) >= 2 and parts[-2].upper() != "U":
            return True
    return False


def classify_body(image, sym):
    """Classify the body of the definition `image` resolves for `sym`.

    Returns (state, detail) where state is INERT | NON_INERT | ABSENT.
    ABSENT is an instrument failure, never a reassuring answer.
    """
    nm_out, nm_rc = _run(["nm", "-a", image])
    if nm_rc != 0 and not nm_out:
        return "ABSENT", "nm_rc=%d" % nm_rc
    if not _defines_symbol(nm_out, sym):
        return "ABSENT", "symbol_not_defined"

    dis, drc = _run(["otool", "-tvV", "-p", sym, image])
    if not dis.strip():
        return "ABSENT", "empty_disasm otool_rc=%d" % drc

    started = False
    insns = 0
    nontrivial = []
    for line in dis.splitlines():
        stripped = line.strip()
        if not started:
            if stripped.endswith(":") and stripped.rstrip(":") == sym:
                started = True
            continue
        if re.match(r"^[A-Za-z_.$][\w.$]*:$", stripped):
            break  # next symbol label ends the body
        m = re.match(r"^[0-9a-fA-F]{6,16}\s+(\S+)", stripped)
        if not m:
            continue
        mnem = m.group(1).lower()
        insns += 1
        if mnem in NONTRIVIAL_MNEMONICS:
            nontrivial.append(mnem)
        if mnem in ("ret", "retab"):
            break

    if insns == 0:
        return "ABSENT", "no_instructions_parsed otool_rc=%d" % drc
    if not nontrivial and insns <= INERT_MAX_INSNS:
        return "INERT", "insns=%d nontrivial=0" % insns
    return "NON_INERT", "insns=%d nontrivial=%d:%s" % (
        insns, len(nontrivial), ",".join(sorted(set(nontrivial))[:4]))


def _uuid(image):
    out, _ = _run(["dwarfdump", "--uuid", image])
    m = re.search(r"UUID:\s*([0-9A-Fa-f-]+)", out)
    if m:
        return m.group(1)
    out, _ = _run(["shasum", "-a", "256", image])
    parts = out.split()
    return "sha256:" + parts[0] if parts else "UNIDENTIFIED"


ARMED_MESSAGE = """
!!! HALIDE JIT MODULE HOOKS ARE NO LONGER INERT !!!

  %s
  resolves a NON-EMPTY body for %s (%s).

WHAT THIS MEANS: native/src/pipeline/dng_copy_lock.cpp:240-253 declares one
deviation from upstream Halide -- upstream's UseModule helper is not replicated
-- and that deviation is safe ONLY while this symbol pair is an empty AOT stub.
It is no longer a stub, so the deviation is now LIVE: the striped copy lock no
longer takes the module refcount upstream takes.

WHAT IS OWED: the crop/slice fork for halide_device_crop / halide_device_slice /
halide_device_release_crop, parked as CONTINGENT-not-scheduled in R4 item 6.
Its trigger has now fired. See docs/logs/2026-09-05/r2-jit-closure.md.

This guard exists precisely so that this change could not happen silently.
Do NOT silence it to get a green build.
"""


def main(argv):
    images = [a for a in argv[1:] if not a.startswith("-")]
    if not images:
        print("JITGUARD|FAIL|no image argument given", file=sys.stderr)
        print("JITGUARD|usage: check_jit_inert.py <mach-o-image> [...]",
              file=sys.stderr)
        return 2

    missing = [p for p in images if not os.path.isfile(p)]
    if missing:
        # A guard whose subject does not exist must be LOUD. Silently passing
        # here is how a check stops existing without anyone noticing.
        for p in missing:
            print("JITGUARD|FAIL|no such image: %s" % p, file=sys.stderr)
        return 2

    armed = False
    unusable = False
    for image in images:
        ident = _uuid(image)
        print("JITGUARD|image=%s|id=%s" % (image, ident))

        # Positive control FIRST and on EVERY invocation: if the classifier
        # cannot emit NON_INERT on a body that provably has one, then every
        # INERT it would print about the JIT pair is worthless.
        cstate, cdetail = classify_body(image, CONTROL_SYM)
        print("JITGUARD|control|%s|%s|%s" % (CONTROL_SYM, cstate, cdetail))
        if cstate != "NON_INERT":
            print("JITGUARD|FAIL|control did not fire on %s (got %s): the "
                  "classifier cannot be trusted to detect a live body, so no "
                  "verdict about the JIT hooks is drawn"
                  % (CONTROL_SYM, cstate), file=sys.stderr)
            unusable = True
            continue

        for sym in JIT_SYMS:
            state, detail = classify_body(image, sym)
            print("JITGUARD|hook|%s|%s|%s" % (sym, state, detail))
            if state == "NON_INERT":
                print(ARMED_MESSAGE % (image, sym, detail), file=sys.stderr)
                armed = True
            elif state == "ABSENT":
                print("JITGUARD|FAIL|%s is ABSENT in %s (%s): the guard "
                      "measured nothing, which is not a pass"
                      % (sym, image, detail), file=sys.stderr)
                unusable = True

    if armed:
        print("JITGUARD|VERDICT=ARMED")
        return 1
    if unusable:
        print("JITGUARD|VERDICT=UNUSABLE")
        return 2
    print("JITGUARD|VERDICT=INERT|images=%d" % len(images))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
