// raw_linear_rgb_normalize AOT kernel vs its same-formula CPU reference.
//
// Output contract: one line per case, final "[RawLinearRgb] ALL PASS"; exit 0
// only when every case passed.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "raw_demosaic_reference.h"

namespace {

int failures = 0;

void report(const char* line, bool ok) {
    std::printf("[RawLinearRgb] %s -> %s\n", line, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

// Deterministic pseudo-random source: a fixed LCG, so a failure is reproducible
// and never depends on the host's RNG.
uint16_t lcg(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return static_cast<uint16_t>((state >> 8) & 0x3FFF);   // 14-bit, like X3F
}

double psnrU16(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b,
               unsigned& max_abs) {
    double sum_sq = 0.0;
    max_abs = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
        const unsigned m = static_cast<unsigned>(d < 0 ? -d : d);
        if (m > max_abs) max_abs = m;
        sum_sq += static_cast<double>(d) * static_cast<double>(d);
    }
    const double mse = sum_sq / static_cast<double>(a.size());
    if (mse <= 0.0) return 999.0;
    return 10.0 * std::log10((65535.0 * 65535.0) / mse);
}

}  // namespace

int main() {
    const uint32_t W = 129;   // deliberately not a multiple of the 16x16 tile
    const uint32_t H = 97;
    const float black[3] = {256.0f, 260.0f, 252.0f};
    const float white = 16383.0f;
    const float inv_range = 65535.0f / (white - 260.0f);

    // --- case 1: packed source, kernel vs reference ------------------------
    {
        std::vector<uint16_t> src(static_cast<size_t>(W) * H * 3);
        uint32_t state = 12345u;
        for (auto& v : src) v = lcg(state);

        std::vector<uint16_t> ref(src.size(), 0), got(src.size(), 0);
        const int64_t stride = static_cast<int64_t>(W) * 3 * 2;

        raw_linear_rgb_normalize_reference(src.data(), W, H, stride, black,
                                           inv_range, ref.data());
        const int ok = raw_linear_rgb_normalize_aot(src.data(), W, H, stride,
                                                    black, inv_range, got.data());
        unsigned max_abs = 0;
        const double psnr = psnrU16(ref, got, max_abs);
        char line[160];
        std::snprintf(line, sizeof(line), "kernel-vs-reference psnr=%.2f max_abs=%u",
                      psnr, max_abs);
        report(line, ok == 1 && psnr >= 99.0 && max_abs <= 1);
    }

    // --- case 2: constant field oracle ------------------------------------
    // A flat field at value V must come out at round((V - black[c]) * inv_range)
    // for every pixel of component c. This catches a kernel that reads the
    // wrong component of `black`, which a PSNR against a reference sharing the
    // same bug would not.
    {
        const uint16_t V = 8192;
        std::vector<uint16_t> src(static_cast<size_t>(W) * H * 3, V);
        std::vector<uint16_t> got(src.size(), 0);
        const int64_t stride = static_cast<int64_t>(W) * 3 * 2;
        const int ok = raw_linear_rgb_normalize_aot(src.data(), W, H, stride,
                                                    black, inv_range, got.data());
        bool all_match = (ok == 1);
        uint16_t want[3] = {0, 0, 0};
        for (int c = 0; c < 3; ++c) {
            float v = (static_cast<float>(V) - black[c]) * inv_range;
            if (v < 0.0f) v = 0.0f;
            if (v > 65535.0f) v = 65535.0f;
            want[c] = static_cast<uint16_t>(std::floor(v + 0.5f));
        }
        for (size_t i = 0; all_match && i < got.size(); ++i) {
            if (got[i] != want[i % 3]) all_match = false;
        }
        char line[160];
        std::snprintf(line, sizeof(line),
                      "constant-field-oracle V=%u want=[%u,%u,%u] got0=[%u,%u,%u]",
                      V, want[0], want[1], want[2], got[0], got[1], got[2]);
        report(line, all_match);
    }

    // --- case 3: strided source -------------------------------------------
    // LibRaw hands us raw_pitch, which need not equal width*6. A kernel that
    // recomputes the stride from the width silently reads the wrong pixels.
    {
        const int64_t stride = static_cast<int64_t>(W) * 3 * 2 + 64;  // 32 spare u16
        const size_t elems_per_row = static_cast<size_t>(stride) / 2;
        std::vector<uint16_t> src(elems_per_row * H, 0);
        uint32_t state = 999u;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t i = 0; i < W * 3; ++i) src[y * elems_per_row + i] = lcg(state);
            for (size_t i = W * 3; i < elems_per_row; ++i)
                src[y * elems_per_row + i] = 0xBEEF;   // poison the padding
        }
        std::vector<uint16_t> ref(static_cast<size_t>(W) * H * 3, 0);
        std::vector<uint16_t> got(ref.size(), 0);

        raw_linear_rgb_normalize_reference(src.data(), W, H, stride, black,
                                           inv_range, ref.data());
        const int ok = raw_linear_rgb_normalize_aot(src.data(), W, H, stride,
                                                    black, inv_range, got.data());
        unsigned max_abs = 0;
        const double psnr = psnrU16(ref, got, max_abs);
        char line[160];
        std::snprintf(line, sizeof(line), "strided-source psnr=%.2f max_abs=%u",
                      psnr, max_abs);
        report(line, ok == 1 && psnr >= 99.0 && max_abs <= 1);
    }

    if (failures != 0) {
        std::printf("[RawLinearRgb] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[RawLinearRgb] ALL PASS\n");
    return 0;
}
