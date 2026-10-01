#!/usr/bin/env python3
"""Self-check for run_decode_matrix's device-handoff parser (canned stdout,
no binaries). Grammar output; exit 0 = all cases passed, 1 = a case failed."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_decode_matrix as matrix  # noqa: E402

LABELS = ("Lossless / Stage3-Stage4", "Lossy / Stage2-Stage4")
LOSSLESS_PASS = (
    "\n[Lossless / Stage3-Stage4 handoff]\n"
    "  [Contract] post-Stage4 handoff ON vs OFF\n"
    "  [Contract] PASS\n"
    "  PSNR(handoff ON vs OFF): 999.00 dB  [PASS]  (threshold=99.00 dB)\n"
)
LOSSY_PASS = (
    "\n[Lossy / Stage2-Stage4 handoff]\n"
    "  [Contract] post-Stage4 handoff ON vs OFF\n"
    "  [Contract] PASS\n"
    "  PSNR(handoff ON vs OFF): 999.00 dB  [PASS]  (threshold=99.00 dB)\n"
)
LOSSY_FAIL = LOSSY_PASS.replace("999.00 dB  [PASS]", "50.00 dB  [FAIL]")
LOSSY_SKIP = (
    "\n[Lossy / Stage2-Stage4 handoff] [SKIP] qDNGUseLibJPEG=0 (JPEG decode unavailable)\n"
    "  [Contract] SKIP reason=libjpeg-unavailable\n"
    "  PSNR(handoff ON vs OFF): SKIP reason=libjpeg-unavailable\n"
    "[DeviceHandoff] lossy -> SKIP reason=libjpeg-unavailable\n"
)
LOSSLESS_SKIP = LOSSY_SKIP.replace("Lossy / Stage2-Stage4", "Lossless / Stage3-Stage4")
# The pre-change emitter's no-lossy-path output, verbatim shape.
OLD_SYNTHETIC = LOSSLESS_PASS + (
    "\n[Lossy / Stage2-Stage4 handoff] [SKIP] no lossy DNG path provided\n"
    "  [Contract] PASS\n"
    "  PSNR(handoff ON vs OFF): 999.00 dB  [PASS]  (threshold=99.00 dB) [SYNTHETIC: no path]\n"
    "\n=== Summary ===\n[ALL PASS] device-handoff PSNR gate passed for all samples\n"
)


def summary(executed, skipped):
    tail = " skipped_cases=lossy" if skipped else ""
    return f"\n=== Summary ===\n[DeviceHandoff SUMMARY] executed={executed} skipped={skipped} failed=0{tail}\n"


def parse(text):
    return matrix._parse_device_handoff_output(text, 0, LABELS, "SelfCheck")


def raises(text):
    try:
        parse(text)
    except RuntimeError:
        return True
    return False


def case_synthetic_text_rejected():
    return raises(OLD_SYNTHETIC)


def case_lossy_skip_recorded():
    results = parse(LOSSLESS_PASS + LOSSY_SKIP + summary(1, 1))
    lossy = results[1]
    return (len(results) == 2 and results[0].status == "PASS"
            and lossy.status == "SKIP" and lossy.psnr_db is None
            and lossy.byte_exact is False and lossy.skip_reason == "libjpeg-unavailable")


def case_lossless_skip_rejected():
    return raises(LOSSLESS_SKIP + LOSSY_PASS + summary(1, 1))


def case_both_pass_accepted():
    results = parse(LOSSLESS_PASS + LOSSY_PASS + summary(2, 0))
    return all(r.status == "PASS" and r.byte_exact and r.psnr_db == 999.0 for r in results)


def case_lossy_fail_raises():
    return raises(LOSSLESS_PASS + LOSSY_FAIL + summary(2, 0))


def main() -> int:
    cases = [
        ("synthetic_text_rejected", case_synthetic_text_rejected),
        ("lossy_skip_recorded", case_lossy_skip_recorded),
        ("lossless_skip_rejected", case_lossless_skip_rejected),
        ("both_pass_accepted", case_both_pass_accepted),
        ("lossy_fail_raises", case_lossy_fail_raises),
    ]
    failed = 0
    for name, function in cases:
        try:
            ok = bool(function())
        except Exception as error:  # a crash is a failed case, not a skip
            ok = False
            print(f"[ParserSelfCheck] {name} raised {type(error).__name__}: {error}")
        print(f"[ParserSelfCheck] {name} -> {'PASS' if ok else 'FAIL'}")
        failed += 0 if ok else 1
    print(f"[ParserSelfCheck SUMMARY] executed={len(cases)} skipped=0 failed={failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
