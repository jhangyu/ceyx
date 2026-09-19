// Fused normalize + Bayer demosaic: AOT kernel vs same-algorithm CPU reference.
//
// Two oracles on purpose. PSNR/max_abs catches codegen drift; the constant-field
// case catches phase transposition, which a whole-image PSNR can hide (spec
// section 11.2.3, and the 2026-08-16 CFA phase bug).
//
// T-V0 (2026-09-19) additions, all additive — no existing assertion weakened:
//   * every comparison is reported PER CHANNEL. The v21 Vulkan
//     materialized-producer defect collapses G/B to a single value; a pooled
//     PSNR is exactly how that hides (spec-cpu-levers.md section 3.3).
//   * a geometry sweep including heights that are NOT multiples of the 16x16
//     GPU tile (the haloed-producer tail hazard, spec section 3.2 caveat 2).
//     Production shape 6048x4024 runs under --production-geometry.
//   * a device-allocation proof and two red-state controls, so a green here is
//     not a blind instrument.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "HalideBuffer.h"
#include "raw_bayer_demosaic.h"
#include "raw_demosaic_reference.h"

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
    // The extra separator is emitted only for named cases, so the unnamed
    // per-phase case prints exactly "[RawBayerKernel] phase=... -> PASS".
    std::printf("[RawBayerKernel] %s%s%s -> %s\n", name,
                (name && name[0]) ? " " : "", detail, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

struct Geometry { const char* name; uint32_t w; uint32_t h; };

// h % 16 != 0 for "tail" and "prod": the tile tail is exercised, which Probe B
// (6000x4000, both multiples of 16) never was.
const Geometry kBaseGeometry{"128x96", 128, 96};
const Geometry kTailGeometry{"130x100", 130, 100};
const Geometry kProdGeometry{"6048x4024", 6048, 4024};

struct Phase { const char* name; int32_t red_x; int32_t red_y; };
const Phase kPhases[4] = {{"RGGB", 0, 0}, {"GRBG", 1, 0},
                          {"GBRG", 0, 1}, {"BGGR", 1, 1}};

// Deterministic pseudo-random mosaic: a fixed LCG, so a failure is reproducible
// without shipping a fixture file.
std::vector<uint16_t> makeNoiseMosaic(uint32_t w, uint32_t h, int64_t row_stride_bytes) {
    const size_t stride_px = static_cast<size_t>(row_stride_bytes) / 2;
    std::vector<uint16_t> src(stride_px * h, 0);
    uint32_t state = 0x13572468u;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            state = state * 1664525u + 1013904223u;
            src[y * stride_px + x] = static_cast<uint16_t>(600 + (state >> 18) % 15000);
        }
    }
    return src;
}

std::vector<uint16_t> makeConstantMosaic(const Geometry& gm, const Phase& p,
                                         uint16_t r, uint16_t g, uint16_t b) {
    std::vector<uint16_t> src(static_cast<size_t>(gm.w) * gm.h, 0);
    for (uint32_t y = 0; y < gm.h; ++y) {
        for (uint32_t x = 0; x < gm.w; ++x) {
            const bool red_row = (y % 2) == static_cast<uint32_t>(p.red_y);
            const bool red_col = (x % 2) == static_cast<uint32_t>(p.red_x);
            uint16_t v = g;
            if (red_row && red_col) v = r;
            else if (!red_row && !red_col) v = b;
            src[static_cast<size_t>(y) * gm.w + x] = v;
        }
    }
    return src;
}

struct Stats {
    double psnr;
    uint32_t max_abs;
    // Per channel: how many elements differ at all, how many differ by more
    // than the accepted 1-LSB codegen tolerance, and the worst magnitude.
    uint64_t differing[3];
    uint64_t beyond_tolerance[3];
    uint32_t channel_max_abs[3];
};

// Interleaved RGB16: element index % 3 is the channel.
Stats compare(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    Stats s{};
    s.psnr = 999.0;
    double sse = 0.0;
    uint32_t max_abs = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const int diff = static_cast<int>(a[i]) - static_cast<int>(b[i]);
        sse += static_cast<double>(diff) * diff;
        const uint32_t mag = static_cast<uint32_t>(diff < 0 ? -diff : diff);
        if (mag > max_abs) max_abs = mag;
        const size_t ch = i % 3;
        if (mag != 0) {
            ++s.differing[ch];
            if (mag > 1) ++s.beyond_tolerance[ch];
            if (mag > s.channel_max_abs[ch]) s.channel_max_abs[ch] = mag;
        }
    }
    s.max_abs = max_abs;
    if (sse != 0.0) {
        const double mse = sse / static_cast<double>(a.size());
        s.psnr = 10.0 * std::log10(65535.0 * 65535.0 / mse);
    }
    return s;
}

// The per-channel signature is what the report is for: a G/B collapse shows up
// as beyond_tolerance[1]/[2] in the millions with [0] small.
std::string channelDetail(const Stats& s) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "perch_beyond_tol=R%llu/G%llu/B%llu perch_differing=R%llu/G%llu/B%llu "
                  "perch_max_abs=R%u/G%u/B%u",
                  static_cast<unsigned long long>(s.beyond_tolerance[0]),
                  static_cast<unsigned long long>(s.beyond_tolerance[1]),
                  static_cast<unsigned long long>(s.beyond_tolerance[2]),
                  static_cast<unsigned long long>(s.differing[0]),
                  static_cast<unsigned long long>(s.differing[1]),
                  static_cast<unsigned long long>(s.differing[2]),
                  s.channel_max_abs[0], s.channel_max_abs[1], s.channel_max_abs[2]);
    return std::string(buf);
}

bool perChannelClean(const Stats& s) {
    return s.beyond_tolerance[0] == 0 && s.beyond_tolerance[1] == 0 &&
           s.beyond_tolerance[2] == 0;
}

const float kBlackFlat[1] = {512.0f};
const float kInvRange = 65535.0f / (16383.0f - 512.0f);

// ---------------------------------------------------------------------------
// Case 1: kernel vs CPU reference on noise, all four CFA phases.
// ---------------------------------------------------------------------------
void runNoiseCases(const Geometry& gm) {
    const size_t out_elems = static_cast<size_t>(gm.w) * gm.h * 3;
    const int64_t stride = static_cast<int64_t>(gm.w) * 2;
    const std::vector<uint16_t> src = makeNoiseMosaic(gm.w, gm.h, stride);

    for (const Phase& p : kPhases) {
        std::vector<uint16_t> ref(out_elems, 0), got(out_elems, 0);
        raw_bayer_demosaic_reference(src.data(), gm.w, gm.h, stride, p.red_x, p.red_y,
                                     kBlackFlat, 1, 1, kInvRange, ref.data());
        const int ok = raw_bayer_demosaic_aot(src.data(), gm.w, gm.h, stride,
                                              p.red_x, p.red_y, kBlackFlat, 1, 1,
                                              kInvRange, got.data());
        const Stats s = compare(ref, got);
        char detail[512];
        std::snprintf(detail, sizeof(detail),
                      "geom=%s phase=%s psnr=%.2f max_abs=%u %s",
                      gm.name, p.name, s.psnr, s.max_abs, channelDetail(s).c_str());
        report("", ok == 1 && s.psnr >= 99.0 && s.max_abs <= 1 && perChannelClean(s),
               detail);
    }
}

// ---------------------------------------------------------------------------
// Case 2: constant-field oracle — exact reconstruction, every pixel, per channel.
// This is the case that catches phase transposition AND channel collapse.
// ---------------------------------------------------------------------------
void runConstantFieldCases(const Geometry& gm) {
    const size_t out_elems = static_cast<size_t>(gm.w) * gm.h * 3;
    for (const Phase& p : kPhases) {
        // Constants that survive normalization exactly.
        const uint16_t expect[3] = {4096, 8192, 12288};
        const std::vector<uint16_t> src =
            makeConstantMosaic(gm, p, expect[0], expect[1], expect[2]);
        const float black_zero[1] = {0.0f};
        const float unity = 1.0f;

        std::vector<uint16_t> got(out_elems, 0);
        const int ok = raw_bayer_demosaic_aot(src.data(), gm.w, gm.h,
                                              static_cast<int64_t>(gm.w) * 2,
                                              p.red_x, p.red_y, black_zero, 1, 1,
                                              unity, got.data());
        uint64_t bad[3] = {0, 0, 0};
        uint32_t first_bad_x = 0, first_bad_y = 0;
        bool have_first = false;
        for (uint32_t y = 0; y < gm.h; ++y) {
            for (uint32_t x = 0; x < gm.w; ++x) {
                const size_t base = (static_cast<size_t>(y) * gm.w + x) * 3;
                for (size_t c = 0; c < 3; ++c) {
                    if (got[base + c] != expect[c]) {
                        ++bad[c];
                        if (!have_first) {
                            first_bad_x = x; first_bad_y = y; have_first = true;
                        }
                    }
                }
            }
        }
        const bool exact = (ok == 1) && bad[0] == 0 && bad[1] == 0 && bad[2] == 0;
        char detail[512];
        std::snprintf(detail, sizeof(detail),
                      "geom=%s phase=%s perch_wrong=R%llu/G%llu/B%llu%s",
                      gm.name, p.name,
                      static_cast<unsigned long long>(bad[0]),
                      static_cast<unsigned long long>(bad[1]),
                      static_cast<unsigned long long>(bad[2]),
                      exact ? " exact" : "");
        if (!exact && have_first) {
            char more[96];
            std::snprintf(more, sizeof(more), " first=(%u,%u) got=(%u,%u,%u)",
                          first_bad_x, first_bad_y,
                          got[(static_cast<size_t>(first_bad_y) * gm.w + first_bad_x) * 3 + 0],
                          got[(static_cast<size_t>(first_bad_y) * gm.w + first_bad_x) * 3 + 1],
                          got[(static_cast<size_t>(first_bad_y) * gm.w + first_bad_x) * 3 + 2]);
            std::strncat(detail, more, sizeof(detail) - std::strlen(detail) - 1);
        }
        report("constant-field", exact, detail);
    }
}

// ---------------------------------------------------------------------------
// Red-state controls. A green byte-compare proves nothing unless the same
// binary, in the same run, demonstrates it could have gone red.
// ---------------------------------------------------------------------------
void runComparatorControl() {
    const Geometry& gm = kBaseGeometry;
    const size_t out_elems = static_cast<size_t>(gm.w) * gm.h * 3;
    const int64_t stride = static_cast<int64_t>(gm.w) * 2;
    const std::vector<uint16_t> src = makeNoiseMosaic(gm.w, gm.h, stride);

    std::vector<uint16_t> ref(out_elems, 0), got(out_elems, 0);
    raw_bayer_demosaic_reference(src.data(), gm.w, gm.h, stride, 0, 0,
                                 kBlackFlat, 1, 1, kInvRange, ref.data());
    const int ok = raw_bayer_demosaic_aot(src.data(), gm.w, gm.h, stride, 0, 0,
                                          kBlackFlat, 1, 1, kInvRange, got.data());
    const Stats clean = compare(ref, got);
    // Perturb one GREEN element by 2 (beyond the 1-LSB tolerance).
    got[1] = static_cast<uint16_t>(got[1] ^ 0x40);
    const Stats flipped = compare(ref, got);
    const bool can_fail = ok == 1 && perChannelClean(clean) &&
                          flipped.beyond_tolerance[1] >= 1;
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "clean_G=%llu after_flip_G=%llu %s",
                  static_cast<unsigned long long>(clean.beyond_tolerance[1]),
                  static_cast<unsigned long long>(flipped.beyond_tolerance[1]),
                  can_fail ? "COMPARATOR_CAN_FAIL" : "COMPARATOR_BLIND");
    report("comparator-control", can_fail, detail);
}

void runPhaseMutationControl() {
    // Ask the kernel for the WRONG CFA phase and require the comparison to go
    // red. Proves the kernel genuinely responds to its inputs, so a green
    // elsewhere is not a dead-input artefact.
    const Geometry& gm = kBaseGeometry;
    const size_t out_elems = static_cast<size_t>(gm.w) * gm.h * 3;
    const int64_t stride = static_cast<int64_t>(gm.w) * 2;
    const std::vector<uint16_t> src = makeNoiseMosaic(gm.w, gm.h, stride);

    int red = 0;
    for (const Phase& p : kPhases) {
        std::vector<uint16_t> ref(out_elems, 0), got(out_elems, 0);
        raw_bayer_demosaic_reference(src.data(), gm.w, gm.h, stride, p.red_x, p.red_y,
                                     kBlackFlat, 1, 1, kInvRange, ref.data());
        // Wrong phase: flip red_x.
        const int ok = raw_bayer_demosaic_aot(src.data(), gm.w, gm.h, stride,
                                              p.red_x ^ 1, p.red_y, kBlackFlat, 1, 1,
                                              kInvRange, got.data());
        const Stats s = compare(ref, got);
        if (ok == 1 && !perChannelClean(s)) ++red;
    }
    char detail[128];
    std::snprintf(detail, sizeof(detail), "red=%d/4 %s", red,
                  red == 4 ? "KERNEL_PATH_CAN_FAIL" : "KERNEL_PATH_BLIND");
    report("phase-mutation-control", red == 4, detail);
}

// ---------------------------------------------------------------------------
// Device-allocation proof: call the production AOT entry directly so the
// halide_buffer_t is visible, and assert the output really carried a device
// allocation (non-zero .device) on a GPU target.
// ---------------------------------------------------------------------------
void runDeviceAllocationProof() {
    const Geometry& gm = kBaseGeometry;
    const int64_t stride = static_cast<int64_t>(gm.w) * 2;
    const std::vector<uint16_t> src = makeNoiseMosaic(gm.w, gm.h, stride);
    std::vector<uint16_t> dst(static_cast<size_t>(gm.w) * gm.h * 3, 0);

    halide_dimension_t src_dims[2] = {
        {0, static_cast<int32_t>(gm.w), 1, 0},
        {0, static_cast<int32_t>(gm.h), static_cast<int32_t>(stride / 2), 0}};
    Halide::Runtime::Buffer<const uint16_t> src_buf(src.data(), 2, src_dims);
    Halide::Runtime::Buffer<const float> black_buf(kBlackFlat, 1, 1);
    halide_dimension_t dst_dims[3] = {
        {0, static_cast<int32_t>(gm.w), 3, 0},
        {0, static_cast<int32_t>(gm.h), static_cast<int32_t>(gm.w) * 3, 0},
        {0, 3, 1, 0}};
    Halide::Runtime::Buffer<uint16_t> dst_buf(dst.data(), 3, dst_dims);
    src_buf.set_host_dirty();
    black_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    const int rc = raw_bayer_demosaic(src_buf, 0, 0, black_buf, kInvRange, dst_buf);
    const uint64_t dst_device = dst_buf.raw_buffer()->device;
    const void* dst_iface =
        static_cast<const void*>(dst_buf.raw_buffer()->device_interface);
    const int copy_rc = dst_buf.copy_to_host();

    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "kernel_rc=%d copy_rc=%d dst_device=0x%llx dst_device_interface=%p",
                  rc, copy_rc, static_cast<unsigned long long>(dst_device), dst_iface);
#if defined(DNG_EXPECT_GPU_DEVICE)
    const bool ok = rc == 0 && copy_rc == 0 && dst_device != 0 && dst_iface != nullptr;
#else
    const bool ok = rc == 0 && copy_rc == 0;
#endif
    report("device-allocation-proof", ok, detail);
}

}  // namespace

int main(int argc, char** argv) {
    bool production_geometry = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--production-geometry") == 0) production_geometry = true;
    }
    std::printf("[RawBayerKernel] production_geometry=%d\n", production_geometry ? 1 : 0);

    runNoiseCases(kBaseGeometry);
    runConstantFieldCases(kBaseGeometry);

    // Tail geometry: 100 % 16 != 0, so the 16x16 GPU tile has a y tail and the
    // haloed producer must still be correct there.
    runNoiseCases(kTailGeometry);
    runConstantFieldCases(kTailGeometry);

    if (production_geometry) {
        // 4024 % 16 == 8: the production y tail.
        runNoiseCases(kProdGeometry);
        runConstantFieldCases(kProdGeometry);
    }

    // 3. Padded stride must not change a single output byte.
    {
        const Geometry& gm = kBaseGeometry;
        const size_t out_elems = static_cast<size_t>(gm.w) * gm.h * 3;
        const int64_t tight = static_cast<int64_t>(gm.w) * 2;
        const int64_t padded = tight + 64;
        const std::vector<uint16_t> a = makeNoiseMosaic(gm.w, gm.h, tight);
        std::vector<uint16_t> b = makeNoiseMosaic(gm.w, gm.h, padded);

        std::vector<uint16_t> out_a(out_elems, 0), out_b(out_elems, 0);
        const int ok_a = raw_bayer_demosaic_aot(a.data(), gm.w, gm.h, tight, 0, 0,
                                                kBlackFlat, 1, 1, kInvRange, out_a.data());
        const int ok_b = raw_bayer_demosaic_aot(b.data(), gm.w, gm.h, padded, 0, 0,
                                                kBlackFlat, 1, 1, kInvRange, out_b.data());
        const Stats s = compare(out_a, out_b);
        char detail[384];
        std::snprintf(detail, sizeof(detail), "row_stride_bytes = width*2 + 64 %s",
                      channelDetail(s).c_str());
        report("padded-stride", ok_a == 1 && ok_b == 1 && out_a == out_b, detail);
    }

    // 4. Per-site black tile.
    {
        const Geometry& gm = kBaseGeometry;
        const size_t out_elems = static_cast<size_t>(gm.w) * gm.h * 3;
        const int64_t stride = static_cast<int64_t>(gm.w) * 2;
        const std::vector<uint16_t> src = makeNoiseMosaic(gm.w, gm.h, stride);
        const float black_tile[4] = {100.0f, 200.0f, 200.0f, 300.0f};

        std::vector<uint16_t> ref(out_elems, 0), got(out_elems, 0);
        raw_bayer_demosaic_reference(src.data(), gm.w, gm.h, stride, 0, 0,
                                     black_tile, 2, 2, kInvRange, ref.data());
        const int ok = raw_bayer_demosaic_aot(src.data(), gm.w, gm.h, stride, 0, 0,
                                              black_tile, 2, 2, kInvRange, got.data());
        const Stats s = compare(ref, got);
        char detail[384];
        std::snprintf(detail, sizeof(detail), "psnr=%.2f max_abs=%u %s",
                      s.psnr, s.max_abs, channelDetail(s).c_str());
        report("black-tile-2x2",
               ok == 1 && s.psnr >= 99.0 && s.max_abs <= 1 && perChannelClean(s), detail);
    }

    runDeviceAllocationProof();
    runComparatorControl();
    runPhaseMutationControl();

    if (failures != 0) {
        std::printf("[RawBayerKernel] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[RawBayerKernel] ALL PASS\n");
    return 0;
}
