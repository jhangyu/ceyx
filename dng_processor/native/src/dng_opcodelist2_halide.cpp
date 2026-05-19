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
//   - Image is host-only unless the production lossy pipeline explicitly
//     enables the Sprint E batched device handoff.
//   - Operates on dng_simple_image (the host's default Stage 2 image type).
//     Falls back if a different dng_image subclass is in play.

#include "dng_opcodelist2_halide.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>

#include "HalideBuffer.h"
#include "HalideRuntime.h"
#include "HalideRuntimeMetal.h"
#include "dng_opcode_polynomial.h"
#include "dng_opcode_polynomial3.h"

#include "dng_image.h"
#include "dng_misc_opcodes.h"
#include "dng_negative.h"
#include "dng_opcodes.h"
#include "dng_opcode_list.h"
#include "dng_pixel_buffer.h"
#include "dng_rect.h"
#include "dng_simple_image.h"
#include "dng_tag_types.h"

// ---------------------------------------------------------------------------
// File-scope helper (visible to both anonymous-namespace code and the public
// API functions below).
// ---------------------------------------------------------------------------

// Scatter a dense plane-major scratch buffer back into the interleaved
// dng_image host memory.
//
// scratch layout : plane-major, dense strides [1, W, W*H]
//   scratch[c * W * H + y * W + x]
//
// image layout   : interleaved with arbitrary col_step / row_step / plane_step
//   image_base[c * plane_step + y * row_step + x * col_step]
static void scatter_poly3_to_image(const uint16_t *scratch,
                                   uint16_t *image_base,
                                   int32_t width,
                                   int32_t height,
                                   int32_t col_step,
                                   int32_t row_step,
                                   int32_t plane_step) {
    for (int c = 0; c < 3; ++c) {
        const uint16_t *src_plane =
            scratch + static_cast<ptrdiff_t>(c) * width * height;
        uint16_t *dst_plane =
            image_base + static_cast<ptrdiff_t>(c) * plane_step;
        for (int32_t y = 0; y < height; ++y) {
            const uint16_t *src_row =
                src_plane + static_cast<ptrdiff_t>(y) * width;
            uint16_t *dst_row =
                dst_plane + static_cast<ptrdiff_t>(y) * row_step;
            for (int32_t x = 0; x < width; ++x) {
                dst_row[static_cast<ptrdiff_t>(x) * col_step] = src_row[x];
            }
        }
    }
}

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

bool ol2_batched_enabled() {
    // Default ON; single-plane bridge remains the fallback path.
    static const bool enabled = []() {
        const char *v = std::getenv("DNG_STAGE2_OL2_BATCHED");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

struct PolynomialTiming {
    double prewarm_ms = 0.0;
    double gather_ms = 0.0;
    double kernel_ms = 0.0;
    double copy_to_host_ms = 0.0;
    double scatter_ms = 0.0;
};

struct DeviceHandoffState {
    std::mutex mu;
    bool requested = false;
    bool valid = false;
    bool host_copied = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t planes = 0;
    uint32_t pixel_range = 0;
    std::unique_ptr<Halide::Runtime::Buffer<uint16_t>> buffer;
    // Scratch host storage that backs dst_buf in run_polynomial3_kernel when
    // defer_copy_to_host=true. Sized W*H*planes*sizeof(uint16_t); grows on
    // demand but never shrinks. Lives here (under mu) so it outlives the call
    // frame and the device buffer can safely reference it.
    std::vector<uint16_t> poly3_scratch;
    // Pointer to the dng_image-internal memory where results must be written
    // back when copy_to_host is eventually requested.  The caller guarantees
    // this pointer remains valid until halide_stage2_ol2_device_handoff_copy_to_host()
    // completes.
    uint16_t *image_base = nullptr;
    // The interleaved layout geometry needed to memcpy from dense scratch back
    // into the image buffer.  Only used when image_base != nullptr.
    int32_t img_col_step = 0;
    int32_t img_row_step = 0;
    int32_t img_plane_step = 0;
};

DeviceHandoffState &device_handoff_state() {
    static DeviceHandoffState state;
    return state;
}

bool device_handoff_requested() {
    DeviceHandoffState &state = device_handoff_state();
    std::lock_guard<std::mutex> lock(state.mu);
    return state.requested;
}

// Publish the device-resident dst buffer together with the image pointer so
// that copy_to_host() can write results back to the SDK image.
// scratch_ptr must point to memory already owned by state.poly3_scratch.
void publish_device_handoff(Halide::Runtime::Buffer<uint16_t> &&buffer,
                            uint32_t width,
                            uint32_t height,
                            uint32_t planes,
                            uint32_t pixel_range,
                            uint16_t *image_base,
                            int32_t img_col_step,
                            int32_t img_row_step,
                            int32_t img_plane_step) {
    DeviceHandoffState &state = device_handoff_state();
    std::lock_guard<std::mutex> lock(state.mu);
    state.buffer = std::make_unique<Halide::Runtime::Buffer<uint16_t>>(
        std::move(buffer));
    state.width = width;
    state.height = height;
    state.planes = planes;
    state.pixel_range = pixel_range;
    state.image_base = image_base;
    state.img_col_step = img_col_step;
    state.img_row_step = img_row_step;
    state.img_plane_step = img_plane_step;
    state.host_copied = false;
    state.valid = true;
}

double elapsed_ms(std::chrono::high_resolution_clock::time_point start,
                  std::chrono::high_resolution_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

double prewarm_polynomial3_kernel_once() {
    if (!ol2_prewarm_enabled()) {
        return 0.0;
    }
    static std::once_flag once;
    double prewarm_ms = 0.0;
    std::call_once(once, [&prewarm_ms]() {
        constexpr int W = 256;
        constexpr int H = 256;
        Halide::Runtime::Buffer<uint16_t> src =
            Halide::Runtime::Buffer<uint16_t>::make_interleaved(W, H, 3);
        Halide::Runtime::Buffer<uint16_t> dst =
            Halide::Runtime::Buffer<uint16_t>::make_interleaved(W, H, 3);
        std::memset(src.data(), 0, sizeof(uint16_t) * W * H * 3);
        float coeff_f32[27] = {0};
        int32_t degree_i32[3] = {1, 1, 1};
        for (int c = 0; c < 3; ++c) {
            coeff_f32[static_cast<size_t>(c) * 9 + 1] = 1.0f;
        }
        Halide::Runtime::Buffer<float> coeff(coeff_f32, 9, 3);
        Halide::Runtime::Buffer<int32_t> degree(degree_i32, 3);

        auto t0 = std::chrono::high_resolution_clock::now();
        const int rc = dng_opcode_polynomial3(src, coeff, degree,
                                              /*pixel_range=*/65535.0f, dst);
        dst.copy_to_host();
        auto t1 = std::chrono::high_resolution_clock::now();
        prewarm_ms = elapsed_ms(t0, t1);

        if (ol2_timing_enabled()) {
            std::fprintf(stderr,
                         "[OpcodeList2Halide] prewarm3 dispatch rc=%d t=%.2fms\n",
                         rc, prewarm_ms);
        }
    });
    return prewarm_ms;
}

// One-shot Metal pre-warm: fire a tiny dng_opcode_polynomial dispatch so that
// the Metal device, command queue, and MTLLibrary for this kernel are all
// initialised before the first 24MP plane comes through. Empirically this
// shaves ~30ms off the plane 0 latency (which otherwise pays for both Metal
// context bring-up and kernel compilation).
double prewarm_polynomial_kernel_once() {
    if (!ol2_prewarm_enabled()) {
        return 0.0;
    }
    static std::once_flag once;
    double prewarm_ms = 0.0;
    std::call_once(once, [&prewarm_ms]() {
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
        prewarm_ms = elapsed_ms(t0, t1);
    });
    return prewarm_ms;
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
                           float pixel_range,
                           PolynomialTiming *timing) {
    // Pre-warm Metal + kernel before any large dispatch. No-op after the
    // first call.
    if (timing) {
        timing->prewarm_ms = prewarm_polynomial_kernel_once();
    } else {
        (void)prewarm_polynomial_kernel_once();
    }

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
    const auto gather_start = std::chrono::high_resolution_clock::now();
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
    const auto gather_end = std::chrono::high_resolution_clock::now();
    if (timing) {
        timing->gather_ms = elapsed_ms(gather_start, gather_end);
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

    const auto kernel_start = std::chrono::high_resolution_clock::now();
    const int rc = dng_opcode_polynomial(*src_buf,
                                         coeff,
                                         static_cast<int32_t>(degree),
                                         pixel_range,
                                         *dst_buf);
    const auto kernel_end = std::chrono::high_resolution_clock::now();
    if (timing) {
        timing->kernel_ms = elapsed_ms(kernel_start, kernel_end);
    }
    if (rc != 0) {
        std::fprintf(stderr,
                     "[OpcodeList2Halide] polynomial kernel rc=%d (degree=%u %dx%d)\n",
                     rc, static_cast<unsigned>(degree), width, height);
        return false;
    }

    const auto copy_start = std::chrono::high_resolution_clock::now();
    dst_buf->copy_to_host();
    const auto copy_end = std::chrono::high_resolution_clock::now();
    if (timing) {
        timing->copy_to_host_ms = elapsed_ms(copy_start, copy_end);
    }

    const auto scatter_start = std::chrono::high_resolution_clock::now();
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
    const auto scatter_end = std::chrono::high_resolution_clock::now();
    if (timing) {
        timing->scatter_ms = elapsed_ms(scatter_start, scatter_end);
    }
    return true;
}

bool run_polynomial3_kernel(uint16_t *base,
                            int32_t width,
                            int32_t height,
                            int32_t col_step,
                            int32_t row_step,
                            int32_t plane_step,
                            const uint32_t degrees[3],
                            const double *coeff_real64[3],
                            uint32_t pixel_range_u32,
                            float pixel_range,
                            bool defer_copy_to_host,
                            PolynomialTiming *timing) {
    if (timing) {
        timing->prewarm_ms = prewarm_polynomial3_kernel_once();
    } else {
        (void)prewarm_polynomial3_kernel_once();
    }

    // src_buf wraps the dng_image interleaved memory (read-only from Halide's
    // perspective).  dst_buf must NOT alias src_buf — allocate a dense
    // plane-major scratch buffer that is independent of the image's lifetime.
    //
    // Scratch layout: plane-major, dense strides [1, W, W*H].
    // This matches halide_dimension_t { {0,W,1}, {0,H,W}, {0,3,W*H} }.
    const size_t scratch_elems =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 3;

    // Grow the scratch vector if needed, then grab a stable pointer.
    // We must NOT hold the mutex while running the kernel (kernel may be
    // long-running on GPU).  The pointer is stable because no other code path
    // resizes poly3_scratch between here and the publish/scatter calls below.
    DeviceHandoffState &state = device_handoff_state();
    uint16_t *scratch_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(state.mu);
        if (state.poly3_scratch.size() < scratch_elems) {
            state.poly3_scratch.resize(scratch_elems);
        }
        scratch_ptr = state.poly3_scratch.data();
    }

    // src: original interleaved image layout (base + non-unit strides).
    halide_dimension_t src_shape[3] = {
        {0, width,  col_step,   0},
        {0, height, row_step,   0},
        {0, 3,      plane_step, 0},
    };
    Halide::Runtime::Buffer<uint16_t> src_buf(base, 3, src_shape);

    // dst: dense plane-major layout backed by scratch (not base).
    halide_dimension_t dst_shape[3] = {
        {0, width,          1,             0},
        {0, height,         width,         0},
        {0, 3,              width * height, 0},
    };
    Halide::Runtime::Buffer<uint16_t> dst_buf(scratch_ptr, 3, dst_shape);

    float coeff_f32[27] = {0};
    int32_t degree_i32[3] = {0, 0, 0};
    for (int c = 0; c < 3; ++c) {
        const uint32_t fill = degrees[c] < 9 ? degrees[c] : 8;
        degree_i32[c] = static_cast<int32_t>(degrees[c]);
        for (uint32_t i = 0; i <= fill; ++i) {
            coeff_f32[static_cast<size_t>(c) * 9 + i] =
                static_cast<float>(coeff_real64[c][i]);
        }
    }
    Halide::Runtime::Buffer<float> coeff(coeff_f32, 9, 3);
    Halide::Runtime::Buffer<int32_t> degree(degree_i32, 3);

    // Mark src host-dirty so Halide uploads it to the Metal device before the
    // kernel reads it.  Do NOT set host-dirty on dst — it is an output buffer.
    src_buf.set_host_dirty(true);

    const auto kernel_start = std::chrono::high_resolution_clock::now();
    const int rc = dng_opcode_polynomial3(src_buf, coeff, degree,
                                          pixel_range, dst_buf);
    const auto kernel_end = std::chrono::high_resolution_clock::now();
    if (timing) {
        timing->kernel_ms = elapsed_ms(kernel_start, kernel_end);
    }
    if (rc != 0) {
        std::fprintf(stderr,
                     "[OpcodeList2Halide] polynomial3 kernel rc=%d (%dx%d)\n",
                     rc, width, height);
        return false;
    }

    if (defer_copy_to_host) {
        // Leave GPU result device-resident.  Publish dst_buf together with the
        // image pointer so the eventual copy_to_host() call can scatter results
        // back into the SDK image.  scratch_ptr remains valid because
        // poly3_scratch is owned by DeviceHandoffState and is never freed until
        // halide_stage2_ol2_clear_device_handoff() is called.
        publish_device_handoff(std::move(dst_buf),
                               static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height),
                               3,
                               pixel_range_u32,
                               base,
                               col_step,
                               row_step,
                               plane_step);
        return true;
    }

    // Immediate path: copy GPU result to scratch, then scatter into image.
    const auto copy_start = std::chrono::high_resolution_clock::now();
    const int copy_rc = dst_buf.copy_to_host();
    const auto copy_end = std::chrono::high_resolution_clock::now();
    if (timing) {
        timing->copy_to_host_ms = elapsed_ms(copy_start, copy_end);
    }
    if (copy_rc != 0) {
        std::fprintf(stderr,
                     "[OpcodeList2Halide] polynomial3 copy_to_host rc=%d\n",
                     copy_rc);
        return false;
    }

    // Scatter dense scratch back into the interleaved dng_image buffer.
    scatter_poly3_to_image(scratch_ptr, base,
                           width, height, col_step, row_step, plane_step);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Process-level per-size prewarm cache for the batched 3-plane polynomial
// kernel.  Keyed by (width << 32 | height) so each unique image dimension is
// warmed exactly once per process lifetime.
// ---------------------------------------------------------------------------

void halide_prewarm_polynomial3_for_size(int width, int height) {
    if (!ol2_prewarm_enabled() || !stage2_ol2_halide_enabled()) {
        return;
    }
    if (width <= 0 || height <= 0) {
        return;
    }

    // Fast path: check cache without the kernel dispatch lock.
    static std::mutex cache_mu;
    static std::unordered_set<uint64_t> warmed;

    const uint64_t key = (static_cast<uint64_t>(width) << 32) |
                         static_cast<uint64_t>(static_cast<uint32_t>(height));

    {
        std::lock_guard<std::mutex> lock(cache_mu);
        if (warmed.count(key)) {
            return;
        }
    }

    // Allocate interleaved src/dst at actual image dimensions so Halide
    // specialises the Metal pipeline state for this exact size.
    Halide::Runtime::Buffer<uint16_t> src =
        Halide::Runtime::Buffer<uint16_t>::make_interleaved(width, height, 3);
    Halide::Runtime::Buffer<uint16_t> dst =
        Halide::Runtime::Buffer<uint16_t>::make_interleaved(width, height, 3);
    std::memset(src.data(), 0, sizeof(uint16_t) * static_cast<size_t>(width) *
                                   static_cast<size_t>(height) * 3);

    // Identity polynomial (degree=1, c1=1) — correct output but no real work.
    float coeff_f32[27] = {0};
    int32_t degree_i32[3] = {1, 1, 1};
    for (int c = 0; c < 3; ++c) {
        coeff_f32[static_cast<size_t>(c) * 9 + 1] = 1.0f;
    }
    Halide::Runtime::Buffer<float> coeff(coeff_f32, 9, 3);
    Halide::Runtime::Buffer<int32_t> degree(degree_i32, 3);

    const auto t0 = std::chrono::high_resolution_clock::now();
    const int rc = dng_opcode_polynomial3(src, coeff, degree,
                                          /*pixel_range=*/65535.0f, dst);
    dst.copy_to_host();
    const auto t1 = std::chrono::high_resolution_clock::now();

    if (ol2_timing_enabled()) {
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::fprintf(stderr,
                     "[OpcodeList2Halide] prewarm3_for_size rc=%d "
                     "size=%dx%d t=%.2fms\n",
                     rc, width, height, ms);
    }

    // Mark this dimension as warmed regardless of rc — even a failed dispatch
    // means Metal has attempted compilation; we should not retry on every call.
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        warmed.insert(key);
    }
}

void halide_stage2_ol2_set_device_handoff_enabled(bool enabled) {
    DeviceHandoffState &state = device_handoff_state();
    std::lock_guard<std::mutex> lock(state.mu);
    state.requested = enabled;
}

void halide_stage2_ol2_clear_device_handoff() {
    DeviceHandoffState &state = device_handoff_state();
    std::lock_guard<std::mutex> lock(state.mu);
    state.buffer.reset();
    state.valid = false;
    state.host_copied = false;
    state.width = 0;
    state.height = 0;
    state.planes = 0;
    state.pixel_range = 0;
    state.image_base = nullptr;
    state.img_col_step = 0;
    state.img_row_step = 0;
    state.img_plane_step = 0;
    // Note: poly3_scratch vector is intentionally NOT cleared here — its
    // allocation is reused across frames (grow-on-demand, never shrinks).
}

bool halide_stage2_ol2_get_device_handoff(Stage2Opcode2DeviceHandoff &out) {
    DeviceHandoffState &state = device_handoff_state();
    std::lock_guard<std::mutex> lock(state.mu);
    if (!state.valid || !state.buffer) {
        out = Stage2Opcode2DeviceHandoff{};
        return false;
    }
    out.device_buffer = state.buffer->raw_buffer();
    out.width = state.width;
    out.height = state.height;
    out.planes = state.planes;
    out.pixel_range = state.pixel_range;
    return out.device_buffer != nullptr;
}

bool halide_stage2_ol2_device_handoff_copy_to_host() {
    DeviceHandoffState &state = device_handoff_state();
    std::lock_guard<std::mutex> lock(state.mu);
    if (!state.valid || !state.buffer || state.host_copied) {
        return true;
    }
    // Pull GPU results into the scratch buffer (dst_buf's host pointer).
    const int rc = state.buffer->copy_to_host();
    if (rc != 0) {
        std::fprintf(stderr,
                     "[OpcodeList2Halide] device_handoff copy_to_host rc=%d\n",
                     rc);
        return false;
    }
    // If the caller recorded an image pointer, scatter scratch → dng_image.
    if (state.image_base != nullptr) {
        const uint16_t *scratch = state.poly3_scratch.data();
        scatter_poly3_to_image(scratch,
                               state.image_base,
                               static_cast<int32_t>(state.width),
                               static_cast<int32_t>(state.height),
                               state.img_col_step,
                               state.img_row_step,
                               state.img_plane_step);
    }
    state.host_copied = true;
    return true;
}

uint32_t halide_try_dispatch_opcode2_batch(dng_host &host,
                                           dng_negative &negative,
                                           dng_opcode_list &list,
                                           uint32_t start_index,
                                           dng_image &image) {
    if (!stage2_ol2_halide_enabled() || !ol2_batched_enabled()) {
        return 0;
    }
    halide_stage2_ol2_clear_device_handoff();
    if (start_index + 2 >= list.Count()) {
        return 0;
    }
    if (image.PixelType() != ttShort) {
        return 0;
    }

    dng_opcode_MapPolynomial *mp[3] = {nullptr, nullptr, nullptr};
    const dng_rect *first_overlap = nullptr;
    dng_rect overlaps[3];
    uint32_t degrees[3] = {0, 0, 0};
    const double *coeffs[3] = {nullptr, nullptr, nullptr};

    auto *simple = dynamic_cast<dng_simple_image *>(&image);
    if (!simple) {
        return 0;
    }
    dng_pixel_buffer pbuf;
    simple->GetPixelBuffer(pbuf);
    if (pbuf.fPixelType != ttShort || pbuf.fPixelSize != 2 ||
        pbuf.fPlanes < 3 || pbuf.fColStep != 3 || pbuf.fPlaneStep != 1 ||
        pbuf.fRowStep <= 0) {
        return 0;
    }

    for (uint32_t i = 0; i < 3; ++i) {
        dng_opcode &op = list.Entry(start_index + i);
        if (op.OpcodeID() != dngOpcode_MapPolynomial) {
            return 0;
        }
        mp[i] = dynamic_cast<dng_opcode_MapPolynomial *>(&op);
        if (!mp[i]) {
            return 0;
        }
        const dng_area_spec &spec = mp[i]->PolyAreaSpec();
        if (spec.RowPitch() != 1 || spec.ColPitch() != 1 ||
            spec.Planes() != 1 ||
            spec.Plane() != static_cast<uint32>(pbuf.fPlane + i)) {
            return 0;
        }
        overlaps[i] = spec.Overlap(image.Bounds());
        if (overlaps[i].IsEmpty()) {
            return 0;
        }
        if (i == 0) {
            first_overlap = &overlaps[i];
        } else if (overlaps[i] != *first_overlap) {
            return 0;
        }
        degrees[i] = mp[i]->PolyDegree();
        coeffs[i] = mp[i]->PolyCoefficients();
    }

    // The caller has already run AboutToApply for start_index. Preserve SDK
    // semantics for the two additional opcodes before consuming them.
    if (!list.Entry(start_index + 1).AboutToApply(host, negative) ||
        !list.Entry(start_index + 2).AboutToApply(host, negative)) {
        return 0;
    }

    const uint32_t pixel_range = image.PixelRange();
    if (pixel_range == 0) {
        return 0;
    }

    uint16_t *base = static_cast<uint16_t *>(
        pbuf.DirtyPixel(first_overlap->t, first_overlap->l,
                        static_cast<uint32>(pbuf.fPlane)));
    if (!base) {
        return 0;
    }

    const int32_t width = static_cast<int32_t>(first_overlap->W());
    const int32_t height = static_cast<int32_t>(first_overlap->H());
    const bool defer_copy_to_host = device_handoff_requested();
    PolynomialTiming detail;
    const auto t0 = std::chrono::high_resolution_clock::now();
    const bool ok = run_polynomial3_kernel(base,
                                           width,
                                           height,
                                           pbuf.fColStep,
                                           pbuf.fRowStep,
                                           pbuf.fPlaneStep,
                                           degrees,
                                           coeffs,
                                           pixel_range,
                                           static_cast<float>(pixel_range),
                                           defer_copy_to_host,
                                           &detail);
    const auto t1 = std::chrono::high_resolution_clock::now();
    if (!ok) {
        return 0;
    }

    if (ol2_timing_enabled()) {
        const double ms = elapsed_ms(t0, t1);
        std::fprintf(stderr,
                     "[OpcodeList2Timing] id=8 name=MapPolynomial path=halide_gpu_batched "
                     "planes=0,1,2 degrees=%u,%u,%u area=%dx%d prewarm=%.2fms "
                     "gather=0.00ms kernel=%.2fms copy_to_host=%.2fms "
                     "scatter=0.00ms device_handoff=%s t=%.2fms\n",
                     static_cast<unsigned>(degrees[0]),
                     static_cast<unsigned>(degrees[1]),
                     static_cast<unsigned>(degrees[2]),
                     width, height,
                     detail.prewarm_ms,
                     detail.kernel_ms,
                     detail.copy_to_host_ms,
                     defer_copy_to_host ? "yes" : "no",
                     ms);
    }
    return 3;
}

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
    PolynomialTiming detail;
    const bool ok = run_polynomial_kernel(base,
                                          width,
                                          height,
                                          col_step,
                                          row_step,
                                          mp->PolyDegree(),
                                          mp->PolyCoefficients(),
                                          static_cast<float>(pixel_range),
                                          &detail);
    auto t1 = std::chrono::high_resolution_clock::now();

    if (ol2_timing_enabled()) {
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::fprintf(stderr,
                     "[OpcodeList2Timing] id=8 name=MapPolynomial path=halide_gpu "
                     "plane=%u degree=%u area=%dx%d prewarm=%.2fms gather=%.2fms "
                     "kernel=%.2fms copy_to_host=%.2fms scatter=%.2fms t=%.2fms\n",
                     static_cast<unsigned>(plane),
                     static_cast<unsigned>(mp->PolyDegree()),
                     width, height,
                     detail.prewarm_ms,
                     detail.gather_ms,
                     detail.kernel_ms,
                     detail.copy_to_host_ms,
                     detail.scatter_ms,
                     ms);
    }

    return ok;
}
