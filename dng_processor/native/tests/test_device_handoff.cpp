/**
 * ---
 * file_summary: "Stage3->4 / Stage2->4 device handoff correctness harness"
 * functions:
 *   - name: "computePsnr8"
 *     description: "Compute uint8 RGB PSNR, returning 999 dB for byte-exact output"
 *     lines: "57-66"
 *   - name: "runDecode"
 *     description: "Decode one DNG through pipeline v2 and retain an owned RGB copy"
 *     lines: "75-88"
 *   - name: "validateContract"
 *     description: "Validate post-Stage4 handoff ON/OFF interleaved RGB contract before PSNR"
 *     lines: "90-134"
 *   - name: "testSample"
 *     description: "Run one handoff ON/OFF sample, enforce contract-first validation, then gate PSNR"
 *     lines: "140-197"
 *   - name: "main"
 *     description: "Run lossless Stage3->4 and lossy Stage2->4 handoff regression samples"
 *     lines: "209-259"
 * ---
 *
 * test_device_handoff.cpp — W5-4 / TD-2: Device handoff path independent test
 *
 * Validates that Stage3→Stage4 device handoff (lossless) and Stage2→Stage4
 * device handoff (lossy) produce bit-identical (PSNR ≥99dB) output compared
 * to the host-copy fallback path. Both paths call the same dng_render_stage4
 * Halide AOT kernel; any difference points to a buffer dim / crop / clamp
 * alignment bug (Gotcha #44, memory.md).
 *
 * Usage:
 *   ./test_device_handoff <lossless_dng> <lossy_dng>
 *
 * Exit codes:
 *   0  ALL PASS  (both samples, both handoff variants ≥99dB)
 *   1  FAIL      (at least one sample failed the PSNR gate or decode error)
 */

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

// POSIX setenv/unsetenv for env override within the same process
#include <unistd.h>

#include "dng_pipeline_v2.h"

using namespace std;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double computePsnr8(const uint8_t* a, const uint8_t* b, size_t n) {
    double mse = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        mse += d * d;
    }
    if (mse == 0.0) return 999.0;
    mse /= static_cast<double>(n);
    return 10.0 * log10(255.0 * 255.0 / mse);
}

struct DecodeOutput {
    vector<uint8_t> rgb;  // deep copy
    uint32_t width = 0;
    uint32_t height = 0;
    bool ok = false;
};

static DecodeOutput runDecode(const char* path) {
    DngPipelineV2Result result;
    bool success = dng_pipeline_v2_decode_to_rgb(path, result);
    if (!success || result.error_code != 0 || !result.rgb_ptr) {
        cerr << "  [decode FAIL] error_code=" << result.error_code << "\n";
        return {};
    }
    DecodeOutput out;
    out.width = result.width;
    out.height = result.height;
    out.rgb.assign(result.rgb_ptr, result.rgb_ptr + result.rgb_size);
    out.ok = true;
    return out;
}

static bool validateContract(
    const DecodeOutput& handoffOut,
    const DecodeOutput& fallbackOut,
    size_t& logicalRgbBytes
) {
    constexpr size_t kPlanes = 3;
    constexpr size_t kPixelSize = sizeof(uint8_t);
    const uint64_t expectedBytes =
        static_cast<uint64_t>(handoffOut.width) *
        static_cast<uint64_t>(handoffOut.height) *
        kPlanes *
        kPixelSize;

    cout << "  [Contract] post-Stage4 handoff ON vs OFF\n";
    cout << "    width=" << handoffOut.width
         << "  height=" << handoffOut.height
         << "  planes=" << kPlanes
         << "  pixelType=uint8"
         << "  pixelSize=" << kPixelSize
         << "  buffer_size=" << handoffOut.rgb.size()
         << "  logical_rgb_bytes=" << expectedBytes
         << "  layout=interleaved RGB\n";

    if (handoffOut.width == 0 || handoffOut.height == 0) {
        cerr << "  [Contract] FAIL: width/height must be non-zero\n";
        return false;
    }
    if (handoffOut.width != fallbackOut.width ||
        handoffOut.height != fallbackOut.height) {
        cerr << "  [Contract] FAIL: width/height mismatch between handoff ON and OFF\n";
        return false;
    }
    if (handoffOut.rgb.size() != fallbackOut.rgb.size()) {
        cerr << "  [Contract] FAIL: buffer size mismatch between handoff ON and OFF\n";
        return false;
    }
    if (handoffOut.rgb.size() < expectedBytes) {
        cerr << "  [Contract] FAIL: buffer size is smaller than interleaved RGB layout\n";
        return false;
    }

    logicalRgbBytes = static_cast<size_t>(expectedBytes);
    cout << "  [Contract] PASS\n";
    return true;
}

// ---------------------------------------------------------------------------
// Per-sample gate
// ---------------------------------------------------------------------------

static bool testSample(
    const char* label,
    const char* path,
    const char* handoffEnvKey,      // e.g. "DNG_STAGE3_STAGE4_DEVICE_HANDOFF"
    double psnrThreshold            // required ≥ this value
) {
    cout << "\n[" << label << "] path=" << path << "\n";
    cout << "  handoff_env=" << handoffEnvKey << "\n";

    // --- Run with handoff ENABLED ---
    setenv(handoffEnvKey, "1", 1);
    cout << "  [Run] device-handoff ON\n";
    DecodeOutput handoffOut = runDecode(path);
    if (!handoffOut.ok) {
        cerr << "  [FAIL] device-handoff ON decode failed\n";
        unsetenv(handoffEnvKey);
        return false;
    }
    cout << "    dimensions: " << handoffOut.width << "x" << handoffOut.height
         << "  rgb_bytes=" << handoffOut.rgb.size() << "\n";

    // --- Run with handoff DISABLED ---
    setenv(handoffEnvKey, "0", 1);
    cout << "  [Run] device-handoff OFF\n";
    DecodeOutput fallbackOut = runDecode(path);
    if (!fallbackOut.ok) {
        cerr << "  [FAIL] device-handoff OFF decode failed\n";
        unsetenv(handoffEnvKey);
        return false;
    }
    cout << "    dimensions: " << fallbackOut.width << "x" << fallbackOut.height
         << "  rgb_bytes=" << fallbackOut.rgb.size() << "\n";

    unsetenv(handoffEnvKey);

    // --- Contract gate: PSNR is invalid until layout compatibility is proven. ---
    size_t logicalRgbBytes = 0;
    if (!validateContract(handoffOut, fallbackOut, logicalRgbBytes)) {
        return false;
    }

    // --- PSNR gate ---
    double psnr = computePsnr8(
        handoffOut.rgb.data(),
        fallbackOut.rgb.data(),
        logicalRgbBytes
    );
    bool passed = psnr >= psnrThreshold;
    cout << "  PSNR(handoff ON vs OFF): " << fixed << setprecision(2) << psnr << " dB  "
         << (passed ? "[PASS]" : "[FAIL]")
         << "  (threshold=" << psnrThreshold << " dB)\n";

    if (!passed) {
        cerr << "  [FAIL] " << label << ": PSNR " << psnr
             << " dB < threshold " << psnrThreshold << " dB\n";
    }
    return passed;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static void printUsage(const char* prog) {
    cerr << "Usage: " << prog << " <lossless_dng> <lossy_dng>\n";
    cerr << "  Validates device-handoff path against host-copy fallback.\n";
    cerr << "  Exit 0 = ALL PASS; Exit 1 = FAIL.\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    const char* lossless = argv[1];
    const char* lossy    = argv[2];

    // Force both handoff switches ON as default for the first run.
    // W5-4: C++ equivalent of "os.environ.setdefault(...)" — only set if not
    // already in env (setenv with overwrite=0).
    setenv("DNG_STAGE3_STAGE4_DEVICE_HANDOFF", "1", 0);
    setenv("DNG_STAGE2_STAGE4_DEVICE_HANDOFF", "1", 0);

    cout << "=== test_device_handoff ===\n";
    cout << "Lossless DNG: " << lossless << "\n";
    cout << "Lossy DNG:    " << lossy    << "\n";

    // Threshold: ≥99dB (Gotcha #44: same Halide AOT kernel, different src
    // residency; result must be bit-identical, but we allow 1dB slack for
    // any minor env-induced path variation).
    constexpr double kPsnrThreshold = 99.0;

    bool allPassed = true;

    // Test 1: lossless — Stage3→Stage4 device handoff
    if (!testSample("Lossless / Stage3-Stage4 handoff",
                    lossless,
                    "DNG_STAGE3_STAGE4_DEVICE_HANDOFF",
                    kPsnrThreshold)) {
        allPassed = false;
    }

    // Test 2: lossy — Stage2→Stage4 device handoff
    if (!testSample("Lossy / Stage2-Stage4 handoff",
                    lossy,
                    "DNG_STAGE2_STAGE4_DEVICE_HANDOFF",
                    kPsnrThreshold)) {
        allPassed = false;
    }

    cout << "\n=== Summary ===\n";
    if (allPassed) {
        cout << "[ALL PASS] device-handoff PSNR gate passed for all samples\n";
        return 0;
    } else {
        cerr << "[FAIL] One or more device-handoff tests did not meet the PSNR gate\n";
        return 1;
    }
}
