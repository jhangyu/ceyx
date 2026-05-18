// Phase 10 Sprint C3 — Stage 2 OpcodeList2 GPU bridge.
// Phase 10 Sprint D-A — Metal pre-warm + persistent device-resident scratch.
//
// Intercepts dng_opcode_MapPolynomial (opcodeID = 8) in dng_opcode_list::Apply
// and dispatches a Halide AOT kernel (dng_opcode_polynomial.a, Metal target).
// Stage 2 image stays host-resident uint16; the kernel reads uint16 → float
// normalize → Horner polynomial → uint16 round in a single GPU pass.
//
// Sprint D-A optimisations:
//   * Option 1 — Metal pre-warm: a 32×32 dispatch fired once via std::call_once
//     pays the JIT / MTLLibrary load on a cheap buffer instead of plane 0 of a
//     6048×4024 polynomial dispatch (saved ~30ms on the first real call).
//   * Option 2 — Persistent device-resident src/dst scratch: a single pair of
//     Halide::Runtime::Buffer<uint16_t> is allocated once and reused across
//     all three planes. We explicitly `device_malloc()` on the Metal interface
//     so subsequent dispatches reuse the same MTLBuffer (no per-plane device
//     alloc / free churn). After memcpying the gathered plane into host, we
//     mark host-dirty so Halide uploads on the next call; the kernel writes
//     to the device side of `dst`, which we then `copy_to_host()` for scatter.
//
// Limitations (Sprint C3 still apply):
//   - Only handles single-plane MapPolynomial with RowPitch=1 / ColPitch=1
//     (the actual lossy DNG observation; SDK fallback covers everything else).
//   - Image is host-only — Sprint D-B handles cross-stage device handoff.
//   - Operates on dng_simple_image (the host's default Stage 2 image type).
//     Falls back if a different dng_image subclass is in play.

#include "dng_opcodelist2_halide.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "HalideBuffer.h"
#include "HalideRuntime.h"
#include "HalideRuntimeMetal.h"
#include "dng_opcode_polynomial.h"

#include "dng_pipeline_config.h"

#include "dng_image.h"
#include "dng_misc_opcodes.h"
#include "dng_opcodes.h"
#include "dng_pixel_buffer.h"
#include "dng_rect.h"
#include "dng_simple_image.h"
#include "dng_tag_types.h"
#include "dng_tag_values.h"

namespace {

// Process-level cache of the env-derived enable flag. Avoids re-reading the
// env on every opcode dispatch.
bool stage2_ol2_halide_enabled() {
    static const bool enabled = []() {
        const char *raw = std::getenv("DNG_STAGE2_OL2_HALIDE");
        return !(raw && raw[0] == '0');
    }();
    return enabled;
}

bool ol2_timing_enabled() {
    static const bool enabled = []() {
        const char *v = std::getenv("DNG_OPCODELIST2_TIMING");
        return v && v[0] == '1';
    }();
    return enabled;
}

bool ol2_prewarm_enabled() {
    // Default ON; allow disabling for A/B measurement via env.
    static const bool enabled = []() {
        const char *v = std::getenv("DNG_STAGE2_OL2_PREWARM");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

bool ol2_persistent_enabled() {
    // Default ON; allow disabling for A/B measurement via env.
    static const bool enabled = []() {
        const char *v = std::getenv("DNG_STAGE2_OL2_PERSISTENT");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// One-shot Metal pre-warm: fire a tiny dng_opcode_polynomial dispatch so that
// the Metal device, command queue, and MTLLibrary for this kernel are all
// initialised before the first 24MP plane comes through. Empirically this
// shaves ~30ms off the plane 0 latency (which otherwise pays for both Metal
// context bring-up and kernel compilation).
void prewarm_polynomial_kernel_once() {
    if (!ol2_prewarm_enabled()) {
        return;
    }
    static std::once_flag once;
    std::call_once(once, []() {
        // 256×256 is large enough to force Metal pipeline state creation +
        // kernel compilation for the polynomial generator, but cheap enough
        // (~0.5ms on Apple Silicon) that we don't waste time at startup.
        constexpr int W = 256;
        constexpr int H = 256;
        Halide::Runtime::Buffer<uint16_t> src(W, H);
        Halide::Runtime::Buffer<uint16_t> dst(W, H);
        std::memset(src.data(), 0, sizeof(uint16_t) * W * H);
        float coeff_f32[9] = {0};
        coeff_f32[0] = 0.0f;
        coeff_f32[1] = 1.0f;
        Halide::Runtime::Buffer<float> coeff(coeff_f32, 9);

        auto t0 = std::chrono::high_resolution_clock::now();
        const int rc = dng_opcode_polynomial(src, coeff,
                                             /*degree=*/1,
                                             /*pixel_range=*/65535.0f,
                                             dst);
        dst.copy_to_host();
        auto t1 = std::chrono::high_resolution_clock::now();

        if (ol2_timing_enabled()) {
            const double ms =
                std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::fprintf(stderr,
                         "[OpcodeList2Halide] prewarm dispatch rc=%d t=%.2fms\n",
                         rc, ms);
        }
    });
}

// Persistent device-resident scratch. Single-thread Stage 2 decode means we
// can rely on a global pair guarded by std::call_once for first-use init plus
// a mutex around the (width,height) check for resize.
struct PersistentScratch {
    Halide::Runtime::Buffer<uint16_t> src;
    Halide::Runtime::Buffer<uint16_t> dst;
    int width = 0;
    int height = 0;
    bool ready = false;
    std::mutex mu;
};

PersistentScratch &persistent_scratch() {
    static PersistentScratch instance;
    return instance;
}

// Ensure the persistent scratch matches the requested size and is bound to
// the Metal device. Returns false if device_malloc fails (caller falls back
// to per-call temporaries).
bool ensure_persistent_scratch(int width, int height) {
    PersistentScratch &s = persistent_scratch();
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.ready && s.width == width && s.height == height) {
        return true;
    }
    // Re-allocate fresh host buffers — Halide::Runtime::Buffer frees device
    // memory on destruction, so simply assigning a new buffer drops the old
    // device allocation.
    s.src = Halide::Runtime::Buffer<uint16_t>(width, height);
    s.dst = Halide::Runtime::Buffer<uint16_t>(width, height);
    s.width = width;
    s.height = height;
    s.ready = false;

    const halide_device_interface_t *metal_iface =
        halide_metal_device_interface();
    if (!metal_iface) {
        return false;
    }
    int rc = s.src.device_malloc(metal_iface);
    if (rc != 0) {
        std::fprintf(stderr,
                     "[OpcodeList2Halide] persistent src device_malloc rc=%d\n",
                     rc);
        return false;
    }
    rc = s.dst.device_malloc(metal_iface);
    if (rc != 0) {
        std::fprintf(stderr,
                     "[OpcodeList2Halide] persistent dst device_malloc rc=%d\n",
                     rc);
        return false;
    }
    s.ready = true;
    return true;
}

// Run polynomial AOT kernel on a single plane. For interleaved layouts
// (col_step != 1) we gather into a dense scratch buffer first, dispatch, then
// scatter back. Persistent device-resident scratch is used when enabled so
// that consecutive planes share a single MTLBuffer.
bool run_polynomial_kernel(uint16_t *plane_ptr,
                           int32_t width,
                           int32_t height,
                           int32_t col_step,         // pixels between adjacent x
                           int32_t row_step,         // pixels between adjacent y
                           uint32_t degree,
                           const double *coeff_real64,
                           float pixel_range) {
    // Pre-warm Metal + kernel before any large dispatch. No-op after the
    // first call.
    prewarm_polynomial_kernel_once();

    Halide::Runtime::Buffer<uint16_t> local_src;
    Halide::Runtime::Buffer<uint16_t> local_dst;
    Halide::Runtime::Buffer<uint16_t> *src_buf = nullptr;
    Halide::Runtime::Buffer<uint16_t> *dst_buf = nullptr;

    const bool use_persistent =
        ol2_persistent_enabled() && ensure_persistent_scratch(width, height);
    if (use_persistent) {
        PersistentScratch &s = persistent_scratch();
        src_buf = &s.src;
        dst_buf = &s.dst;
    } else {
        local_src = Halide::Runtime::Buffer<uint16_t>(width, height);
        local_dst = Halide::Runtime::Buffer<uint16_t>(width, height);
        src_buf = &local_src;
        dst_buf = &local_dst;
    }

    // Host-side gather: interleaved plane → dense scratch.
    uint16_t *src_data = src_buf->data();
    for (int32_t y = 0; y < height; ++y) {
        const uint16_t *row = plane_ptr + static_cast<ptrdiff_t>(y) * row_step;
        uint16_t *scratch = src_data + static_cast<ptrdiff_t>(y) * width;
        if (col_step == 1) {
            std::memcpy(scratch, row, sizeof(uint16_t) * static_cast<size_t>(width));
        } else {
            for (int32_t x = 0; x < width; ++x) {
                scratch[x] = row[x * col_step];
            }
        }
    }

    // Host got new data; Halide must re-upload before dispatch.
    src_buf->set_host_dirty(true);

    // Coeff buffer (9 entries, c0..c8, unused entries zeroed).
    float coeff_f32[9] = {0};
    const uint32_t fill = degree < 9 ? degree : 8;
    for (uint32_t i = 0; i <= fill; ++i) {
        coeff_f32[i] = static_cast<float>(coeff_real64[i]);
    }
    Halide::Runtime::Buffer<float> coeff(coeff_f32, 9);

    const int rc = dng_opcode_polynomial(*src_buf,
                                         coeff,
                                         static_cast<int32_t>(degree),
                                         pixel_range,
                                         *dst_buf);
    if (rc != 0) {
        std::fprintf(stderr,
                     "[OpcodeList2Halide] polynomial kernel rc=%d (degree=%u %dx%d)\n",
                     rc, static_cast<unsigned>(degree), width, height);
        return false;
    }

    dst_buf->copy_to_host();
    const uint16_t *src_dst = dst_buf->data();
    for (int32_t y = 0; y < height; ++y) {
        uint16_t *row = plane_ptr + static_cast<ptrdiff_t>(y) * row_step;
        const uint16_t *src_row = src_dst + static_cast<ptrdiff_t>(y) * width;
        if (col_step == 1) {
            std::memcpy(row, src_row, sizeof(uint16_t) * static_cast<size_t>(width));
        } else {
            for (int32_t x = 0; x < width; ++x) {
                row[x * col_step] = src_row[x];
            }
        }
    }
    return true;
}

}  // namespace

bool halide_try_dispatch_opcode2(dng_host & /* host */,
                                 dng_opcode &opcode,
                                 dng_image &image) {
    if (!stage2_ol2_halide_enabled()) {
        return false;
    }

    // Only MapPolynomial (opcodeID = 8) is handled in C3.
    if (opcode.OpcodeID() != dngOpcode_MapPolynomial) {
        return false;
    }

    // ttShort path only (Stage 2 default for uint16 DNG). Float-stage-2 falls
    // back to SDK (rare; covered by NeedDefloatStage2 / float-Stage1 inputs).
    if (image.PixelType() != ttShort) {
        return false;
    }

    auto *mp = dynamic_cast<dng_opcode_MapPolynomial *>(&opcode);
    if (!mp) {
        return false;
    }

    const dng_area_spec &spec = mp->PolyAreaSpec();
    if (spec.RowPitch() != 1 || spec.ColPitch() != 1) {
        return false;
    }
    if (spec.Planes() != 1) {
        return false;
    }

    // Need dng_simple_image for direct uint16 plane pointer (zero-copy).
    auto *simple = dynamic_cast<dng_simple_image *>(&image);
    if (!simple) {
        return false;
    }
    dng_pixel_buffer pbuf;
    simple->GetPixelBuffer(pbuf);
    if (pbuf.fPixelType != ttShort || pbuf.fPixelSize != 2) {
        return false;
    }

    const dng_rect overlap = spec.Overlap(image.Bounds());
    if (overlap.IsEmpty()) {
        return false;
    }

    const uint32 plane = spec.Plane();
    if (plane >= pbuf.fPlanes + pbuf.fPlane || plane < pbuf.fPlane) {
        return false;
    }

    if (pbuf.fColStep <= 0 || pbuf.fRowStep <= 0) {
        return false;
    }

    uint16_t *base =
        static_cast<uint16_t *>(pbuf.DirtyPixel(overlap.t, overlap.l, plane));
    const int32_t width = static_cast<int32_t>(overlap.W());
    const int32_t height = static_cast<int32_t>(overlap.H());
    const int32_t col_step = pbuf.fColStep;
    const int32_t row_step = pbuf.fRowStep;

    const uint32_t pixel_range = image.PixelRange();
    if (pixel_range == 0) {
        return false;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    const bool ok = run_polynomial_kernel(base,
                                          width,
                                          height,
                                          col_step,
                                          row_step,
                                          mp->PolyDegree(),
                                          mp->PolyCoefficients(),
                                          static_cast<float>(pixel_range));
    auto t1 = std::chrono::high_resolution_clock::now();

    if (ol2_timing_enabled()) {
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::fprintf(stderr,
                     "[OpcodeList2Timing] id=8 name=MapPolynomial path=halide_gpu "
                     "plane=%u degree=%u area=%dx%d t=%.2fms\n",
                     static_cast<unsigned>(plane),
                     static_cast<unsigned>(mp->PolyDegree()),
                     width, height, ms);
    }

    return ok;
}
