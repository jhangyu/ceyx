// Phase 10 Sprint C3 — Stage 2 OpcodeList2 GPU bridge.
// Phase 10 Sprint D-A — GPU pre-warm + persistent device-resident scratch.
//
// Intercepts dng_opcode_MapPolynomial (opcodeID = 8) in dng_opcode_list::Apply
// and dispatches a Halide AOT kernel (dng_opcode_polynomial.a, GPU target).
// Stage 2 image stays host-resident uint16; the kernel reads uint16 → float
// normalize → Horner polynomial → uint16 round in a single GPU pass.
//
// Sprint D-A optimisations:
//   * Option 1 — GPU pre-warm: a 32×32 dispatch fired once via std::call_once
//     pays the JIT / shader library load on a cheap buffer instead of plane 0
//     of a 6048×4024 polynomial dispatch (saved ~30ms on the first real call).
//   * Option 2 — Persistent device-resident src/dst scratch: a single pair of
//     Halide::Runtime::Buffer<uint16_t> is allocated once and reused across
//     all three planes. We explicitly `device_malloc()` on the GPU interface
//     so subsequent dispatches reuse the same device buffer (no per-plane device
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// M-6: dng_error_codes.h no longer needed here — DngOl2DispatchError removed,
// dispatch failure now signaled via g_ol2_dispatch_failed flag.
#include <utility>

#include "HalideBuffer.h"
#include "HalideRuntime.h"
#include "dng_halide_device.h"
#include "dng_opcode_polynomial.h"
#include "dng_opcode_polynomial3.h"
// PipelineConfig::stage2Opcodelist2HalideEnabled() is the canonical reader for
// DNG_STAGE2_OL2_HALIDE; this bridge defers to it (source-of-truth:
// dng_pipeline_config.h, RouteConfig category).
#include "dng_pipeline_config.h"
#include "dng_timing_utils.h"

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

// Scatter a dense interleaved RGB scratch buffer back into the dng_image host
// memory.
//
// scratch layout : interleaved RGB, dense strides [3, W*3, 1]
//   scratch[(y * W + x) * 3 + c]
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
    if (col_step == 3 && row_step == width * 3 && plane_step == 1) {
        const size_t elems =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
        std::memcpy(image_base, scratch, elems * sizeof(uint16_t));
        return;
    }

    for (int32_t y = 0; y < height; ++y) {
        const uint16_t *src_row =
            scratch + static_cast<ptrdiff_t>(y) * width * 3;
        uint16_t *dst_row =
            image_base + static_cast<ptrdiff_t>(y) * row_step;
        for (int32_t x = 0; x < width; ++x) {
            const uint16_t *src_px = src_row + static_cast<ptrdiff_t>(x) * 3;
            uint16_t *dst_px = dst_row + static_cast<ptrdiff_t>(x) * col_step;
            for (int c = 0; c < 3; ++c) {
                dst_px[static_cast<ptrdiff_t>(c) * plane_step] = src_px[c];
            }
        }
    }
}

namespace {

// DNG_STAGE2_OL2_HALIDE — RouteConfig (production kill-switch).
// source-of-truth: dng_pipeline_config.h (PipelineConfig::stage2Opcodelist2HalideEnabled).
// This wrapper exists so callers below can keep their local name; do NOT
// re-read the env here.
bool stage2_opcodelist2_halide_enabled() {
    return PipelineConfig::stage2Opcodelist2HalideEnabled();
}

// DNG_STAGE2_OL2_PREWARM — RouteConfig/DiagnosticConfig hybrid.
// Default ON; allow disabling for A/B measurement via env. Kept as a
// call-site lazy cache (rather than centralising in PipelineConfig) so the
// pre-warm decision happens before any RouteConfig load on the cold path.
bool ol2_prewarm_enabled() {
    static const bool enabled = []() {
        const char *v = std::getenv("DNG_STAGE2_OL2_PREWARM");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// ol2_persistent_enabled: hardcoded true (Phase 10 Sprint E-F cleanup).
// ol2_batched_enabled: hardcoded true (Phase 10 Sprint E-F cleanup).

// DNG_MAP_POLY_TIMING — DiagnosticConfig (observability only).
// Kept as a call-site lazy cache to avoid touching hot path with a config
// load when timing is disabled.
bool map_poly_timing_enabled() {
    static const bool enabled = []() {
        const char *v = std::getenv("DNG_MAP_POLY_TIMING");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

struct PolynomialTiming {
    double prewarm_ms = 0.0;
    double gather_ms = 0.0;
    double src_upload_ms = 0.0;
    double param_upload_ms = 0.0;
    double kernel_ms = 0.0;
    double copy_to_host_ms = 0.0;
    double scatter_ms = 0.0;
    double handoff_publish_ms = 0.0;
    double dispatch_call_ms = 0.0;
    // Phase 10 cold-path subdivision: precise timestamps so caller can split
    // the dispatch_call window into pre_call_setup / kernel_func_entry /
    // buffer_setup / inner-stage gaps / kernel_func_exit / post_call_destructors.
    // Only meaningful when DNG_MAP_POLY_TIMING is enabled.
    std::chrono::high_resolution_clock::time_point t_func_entry{};
    std::chrono::high_resolution_clock::time_point t_inner_start{};
    std::chrono::high_resolution_clock::time_point t_after_prewarm{};
    std::chrono::high_resolution_clock::time_point t_before_src_upload{};
    std::chrono::high_resolution_clock::time_point t_inner_end{};
    std::chrono::high_resolution_clock::time_point t_func_exit{};
    bool first_call = false;
};

// Process-level marker so the caller can flag the *very first* invocation of
// each kernel. Useful to separate Halide AOT first-use cost from steady state.
std::atomic<bool> g_polynomial_first_seen{false};
std::atomic<bool> g_polynomial3_first_seen{false};

// M-6: Dispatch failure flag.  Set by halide_try_dispatch_opcode2{_batch} on
// GPU dispatch failure; checked by the SDK opcode loop (to break) and by
// pipeline_v2 (to map to kDngErrOl2DispatchFailed).  Replaces the old
// DngOl2DispatchError throw-as-control-flow.  Protected by
// pipelineSingleFlightMutex (single-flight invariant).
std::atomic<bool> g_ol2_dispatch_failed{false};
std::string g_ol2_dispatch_failure_msg;  // NOT thread-safe to read without pipelineSingleFlightMutex

// M-5: Value object grouping the writeback destination pointer and its
// interleaved layout geometry.  Replaces the previous comment-only lifetime
// contract on the raw image_base / img_*_step fields.  No heap allocation;
// trivially copyable.
//
// Lifetime contract: the caller of publish_device_handoff() guarantees that
// `base` remains valid until halide_stage2_ol2_device_handoff_copy_to_host()
// completes or halide_stage2_ol2_clear_device_handoff() is called.  This is
// structurally enforced by the pipeline single-flight mutex
// (pipelineSingleFlightMutex in dng_pipeline.cpp).
struct WritebackTarget {
    uint16_t *base = nullptr;
    int32_t col_step = 0;
    int32_t row_step = 0;
    int32_t plane_step = 0;

    // True iff a valid writeback destination has been captured.
    bool has_target() const { return base != nullptr; }

    // Reset to empty / invalid state.
    void reset() { *this = WritebackTarget{}; }
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
    // Interleaved RGB scratch host storage that backs dst_buf in
    // run_polynomial3_kernel when defer_copy_to_host=true. Sized
    // W*H*planes*sizeof(uint16_t); grows on demand but never shrinks. Lives
    // here (under mu) so it outlives the call frame and the device buffer can
    // safely reference it.
    std::vector<uint16_t> poly3_scratch;
    // M-5: Destination for scattering GPU results back into the dng_image host
    // memory.  Validity is tied to the pipeline single-flight mutex; the
    // writeback is consumed (or reset) before the mutex is released.
    WritebackTarget writeback;
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

// Publish the device-resident dst buffer together with the writeback target so
// that copy_to_host() can scatter results back into the SDK image.
// scratch_ptr must point to interleaved RGB memory already owned by
// state.poly3_scratch.
void publish_device_handoff(Halide::Runtime::Buffer<uint16_t> &&buffer,
                            uint32_t width,
                            uint32_t height,
                            uint32_t planes,
                            uint32_t pixel_range,
                            const WritebackTarget &target) {
    DeviceHandoffState &state = device_handoff_state();
    std::lock_guard<std::mutex> lock(state.mu);
    state.buffer = std::make_unique<Halide::Runtime::Buffer<uint16_t>>(
        std::move(buffer));
    state.width = width;
    state.height = height;
    state.planes = planes;
    state.pixel_range = pixel_range;
    state.writeback = target;
    state.host_copied = false;
    state.valid = true;
}

// Time a single explicit copy_to_device upload and record the elapsed
// milliseconds. Returns the buffer's rc unchanged; the caller keeps full
// control of error labelling and return semantics (this helper only collapses
// the identical now()/copy_to_device/now()/elapsed_ms timing snippet that the
// single-plane and polynomial3 src uploads share).
//
// Template over the Halide buffer type because run_polynomial_kernel passes a
// Halide::Runtime::Buffer<uint16_t> while run_polynomial3_kernel passes one of
// a different (strided) construction; both expose copy_to_device(iface).
//
// Must stay in the anonymous namespace (do NOT move to a header): it depends on
// Halide runtime types and is a private bridge implementation detail.
template <typename BufT>
int measure_device_upload(BufT &buffer,
                          const halide_device_interface_t *iface,
                          double &out_ms) {
    const auto upload_start = std::chrono::high_resolution_clock::now();
    const int rc = buffer.copy_to_device(iface);
    const auto upload_end = std::chrono::high_resolution_clock::now();
    out_ms = dng_timing::elapsed_ms(upload_start, upload_end);
    return rc;
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
        src.set_host_dirty(true);
        coeff.set_host_dirty(true);
        degree.set_host_dirty(true);

        auto t0 = std::chrono::high_resolution_clock::now();
        const int rc = dng_opcode_polynomial3(src, coeff, degree,
                                              /*pixel_range=*/65535.0f, dst);
        dst.copy_to_host();
        auto t1 = std::chrono::high_resolution_clock::now();
        prewarm_ms = dng_timing::elapsed_ms(t0, t1);
    });
    return prewarm_ms;
}

// One-shot GPU pre-warm: fire a tiny dng_opcode_polynomial dispatch so that
// the GPU device, command queue, and shader library for this kernel are all
// initialised before the first 24MP plane comes through. Empirically this
// shaves ~30ms off the plane 0 latency (which otherwise pays for both GPU
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
        src.set_host_dirty(true);
        coeff.set_host_dirty(true);

        auto t0 = std::chrono::high_resolution_clock::now();
        const int rc = dng_opcode_polynomial(src, coeff,
                                             /*degree=*/1,
                                             /*pixel_range=*/65535.0f,
                                             dst);
        dst.copy_to_host();
        auto t1 = std::chrono::high_resolution_clock::now();
        prewarm_ms = dng_timing::elapsed_ms(t0, t1);
    });
    return prewarm_ms;
}

// Persistent device-resident scratch. Single-thread Stage 2 decode means we
// can rely on a global pair guarded by std::call_once for first-use init plus
// a mutex around the (width,height) check for resize.
//
// W8/L-6: only the dst side stays a persistent dense device buffer. The src
// side is now a fresh strided wrap over the caller's plane memory each call
// (see run_polynomial_kernel below, ported from the polynomial3 strided
// pattern at ~:596-609), so there is no longer a dense host-owned src buffer
// to keep device-resident here.
struct PersistentScratch {
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

// Ensure the persistent dst scratch matches the requested size and is bound
// to the Metal device. Returns false if device_malloc fails (caller falls
// back to a per-call temporary).
bool ensure_persistent_scratch(int width, int height) {
    PersistentScratch &s = persistent_scratch();
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.ready && s.width == width && s.height == height) {
        return true;
    }
    // Re-allocate a fresh host buffer — Halide::Runtime::Buffer frees device
    // memory on destruction, so simply assigning a new buffer drops the old
    // device allocation.
    s.dst = Halide::Runtime::Buffer<uint16_t>(width, height);
    s.width = width;
    s.height = height;
    s.ready = false;

    const halide_device_interface_t *metal_iface =
        dng_halide_gpu_device_interface();
    if (!metal_iface) {
        return false;
    }
    const int rc = s.dst.device_malloc(metal_iface);
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
    if (timing) {
        timing->t_func_entry = std::chrono::high_resolution_clock::now();
        timing->first_call = !g_polynomial_first_seen.exchange(true);
        timing->t_inner_start = timing->t_func_entry;
    }
    // Pre-warm Metal + kernel before any large dispatch. No-op after the
    // first call.
    if (timing) {
        timing->prewarm_ms = prewarm_polynomial_kernel_once();
        timing->t_after_prewarm = std::chrono::high_resolution_clock::now();
    } else {
        (void)prewarm_polynomial_kernel_once();
    }

    Halide::Runtime::Buffer<uint16_t> local_dst;
    Halide::Runtime::Buffer<uint16_t> *dst_buf = nullptr;

    const bool use_persistent =
        ensure_persistent_scratch(width, height);
    if (use_persistent) {
        PersistentScratch &s = persistent_scratch();
        dst_buf = &s.dst;
    } else {
        local_dst = Halide::Runtime::Buffer<uint16_t>(width, height);
        dst_buf = &local_dst;
    }

    // W8/L-6: direct strided wrap over the interleaved plane memory, ported
    // from the polynomial3 strided-src pattern (~:596-609). copy_to_device
    // below reads plane_ptr's strided layout straight off the host pointer,
    // eliminating the scalar/memcpy host-side gather loop this path used to
    // pay per plane before dispatch. gather_ms now only covers the
    // near-zero-cost buffer view construction.
    const auto gather_start = std::chrono::high_resolution_clock::now();
    halide_dimension_t src_shape[2] = {
        {0, width,  col_step, 0},
        {0, height, row_step, 0},
    };
    Halide::Runtime::Buffer<uint16_t> src_buf(plane_ptr, 2, src_shape);
    src_buf.set_host_dirty(true);
    const auto gather_end = std::chrono::high_resolution_clock::now();
    if (timing) {
        timing->gather_ms = dng_timing::elapsed_ms(gather_start, gather_end);
    }

    // Coeff buffer (9 entries, c0..c8, unused entries zeroed).
    float coeff_f32[9] = {0};
    const uint32_t fill = degree < 9 ? degree : 8;
    for (uint32_t i = 0; i <= fill; ++i) {
        coeff_f32[i] = static_cast<float>(coeff_real64[i]);
    }
    Halide::Runtime::Buffer<float> coeff(coeff_f32, 9);
    coeff.set_host_dirty(true);

    if (timing) {
        timing->t_before_src_upload =
            std::chrono::high_resolution_clock::now();
    }

    if (timing && map_poly_timing_enabled()) {
        const halide_device_interface_t *metal_iface =
            dng_halide_gpu_device_interface();
        if (metal_iface) {
            const int src_rc =
                measure_device_upload(src_buf, metal_iface,
                                      timing->src_upload_ms);
            if (src_rc != 0) {
                std::fprintf(stderr,
                             "[OpcodeList2Halide] polynomial src upload rc=%d\n",
                             src_rc);
                return false;
            }

            const int coeff_rc =
                measure_device_upload(coeff, metal_iface,
                                      timing->param_upload_ms);
            if (coeff_rc != 0) {
                std::fprintf(stderr,
                             "[OpcodeList2Halide] polynomial coeff upload rc=%d\n",
                             coeff_rc);
                return false;
            }
        }
    }

    const auto kernel_start = std::chrono::high_resolution_clock::now();
    const int rc = dng_opcode_polynomial(src_buf,
                                         coeff,
                                         static_cast<int32_t>(degree),
                                         pixel_range,
                                         *dst_buf);
    const auto kernel_end = std::chrono::high_resolution_clock::now();
    if (timing) {
        timing->kernel_ms = dng_timing::elapsed_ms(kernel_start, kernel_end);
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
        timing->copy_to_host_ms = dng_timing::elapsed_ms(copy_start, copy_end);
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
        timing->scatter_ms = dng_timing::elapsed_ms(scatter_start, scatter_end);
        timing->t_inner_end = scatter_end;
        timing->t_func_exit = std::chrono::high_resolution_clock::now();
    }
    return true;
}

// W8/L-6 self-check: the single-plane strided-wrap gather (above) isn't
// exercised by the current lossy/lossless fixtures — they always route
// through the 3-consecutive-opcode batched3 path. This verifies the
// strided-wrap src_buf produces the same result as a reference scalar Horner
// evaluation for a synthetic non-contiguous plane (col_step > 1, row_step
// wider than width*col_step, i.e. embedded in larger padded/interleaved
// memory), so the change is proven correct even without fixture coverage.
// Opt-in only (DNG_OL2_SELFTEST=1); never runs on the production/matrix
// hot path.
void ol2_selftest_strided_gather() {
    static std::once_flag once;
    std::call_once(once, []() {
        const char *v = std::getenv("DNG_OL2_SELFTEST");
        if (!(v && v[0] != '0' && v[0] != '\0')) {
            return;
        }
        constexpr int32_t width = 17;
        constexpr int32_t height = 13;
        constexpr int32_t col_step = 5;    // simulates an interleaved plane
        constexpr int32_t row_step = 131;  // > width*col_step: padded rows
        constexpr float pixel_range = 65535.0f;
        const double coeff[9] = {1000.0, 0.5, 0, 0, 0, 0, 0, 0, 0};

        std::vector<uint16_t> host(static_cast<size_t>(row_step) * height, 0);
        for (int32_t y = 0; y < height; ++y) {
            for (int32_t x = 0; x < width; ++x) {
                host[static_cast<size_t>(y) * row_step +
                     static_cast<size_t>(x) * col_step] =
                    static_cast<uint16_t>((y * 37 + x * 101) % 60000);
            }
        }

        // Reference: same Horner(degree=1) + clamp + round formula as
        // DngOpcodePolynomialGenerator::generate().
        std::vector<uint16_t> expected(static_cast<size_t>(width) * height);
        for (int32_t y = 0; y < height; ++y) {
            for (int32_t x = 0; x < width; ++x) {
                const uint16_t src_val =
                    host[static_cast<size_t>(y) * row_step +
                         static_cast<size_t>(x) * col_step];
                const float x_norm = static_cast<float>(src_val) / pixel_range;
                float y_val = static_cast<float>(coeff[0]) +
                              x_norm * static_cast<float>(coeff[1]);
                y_val = std::min(1.0f, std::max(0.0f, y_val));
                expected[static_cast<size_t>(y) * width + x] =
                    static_cast<uint16_t>(y_val * pixel_range + 0.5f);
            }
        }

        std::vector<uint16_t> actual_host = host;
        const bool dispatched = run_polynomial_kernel(
            actual_host.data(), width, height, col_step, row_step,
            /*degree=*/1, coeff, pixel_range, /*timing=*/nullptr);

        bool ok = dispatched;
        for (int32_t y = 0; ok && y < height; ++y) {
            for (int32_t x = 0; x < width; ++x) {
                const uint16_t got =
                    actual_host[static_cast<size_t>(y) * row_step +
                                static_cast<size_t>(x) * col_step];
                const uint16_t want = expected[static_cast<size_t>(y) * width + x];
                if (got != want) {
                    std::fprintf(stderr,
                                 "[OL2SelfTest] FAIL at (%d,%d) got=%u want=%u\n",
                                 x, y, static_cast<unsigned>(got),
                                 static_cast<unsigned>(want));
                    ok = false;
                    break;
                }
            }
        }
        std::fprintf(stderr, "[OL2SelfTest] strided_gather %s\n",
                     ok ? "PASS" : "FAIL");
    });
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
        timing->t_func_entry = std::chrono::high_resolution_clock::now();
        timing->first_call = !g_polynomial3_first_seen.exchange(true);
        timing->t_inner_start = timing->t_func_entry;
    }
    if (timing) {
        timing->prewarm_ms = prewarm_polynomial3_kernel_once();
        timing->t_after_prewarm = std::chrono::high_resolution_clock::now();
    } else {
        (void)prewarm_polynomial3_kernel_once();
    }

    // src_buf wraps the dng_image interleaved memory (read-only from Halide's
    // perspective).  dst_buf must NOT alias src_buf — allocate a dense
    // interleaved RGB scratch buffer that is independent of the image's
    // lifetime and matches DngOpcodePolynomial3Generator's dst stride
    // contract.
    //
    // Scratch layout: interleaved RGB, dense strides [3, W*3, 1].
    // This matches halide_dimension_t { {0,W,3}, {0,H,W*3}, {0,3,1} }.
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

    // dst: dense interleaved RGB layout backed by scratch (not base).
    halide_dimension_t dst_shape[3] = {
        {0, width,          3,             0},
        {0, height,         width * 3,     0},
        {0, 3,              1,             0},
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
    coeff.set_host_dirty(true);
    degree.set_host_dirty(true);

    // Mark src host-dirty so Halide uploads it to the Metal device before the
    // kernel reads it.  Do NOT set host-dirty on dst — it is an output buffer.
    src_buf.set_host_dirty(true);

    if (timing) {
        timing->t_before_src_upload =
            std::chrono::high_resolution_clock::now();
    }

    if (timing && map_poly_timing_enabled()) {
        const halide_device_interface_t *metal_iface =
            dng_halide_gpu_device_interface();
        if (metal_iface) {
            const int src_rc =
                measure_device_upload(src_buf, metal_iface,
                                      timing->src_upload_ms);
            if (src_rc != 0) {
                std::fprintf(stderr,
                             "[OpcodeList2Halide] polynomial3 src upload rc=%d\n",
                             src_rc);
                return false;
            }

            // coeff + degree are timed together as one param_upload window
            // (poly3 uploads both, unlike the single-plane path which only has
            // coeff). Keep this joint window inline: folding it into
            // measure_device_upload would change the per-buffer timing
            // granularity and the combined coeff_rc||degree_rc error label.
            const auto param_upload_start =
                std::chrono::high_resolution_clock::now();
            const int coeff_rc = coeff.copy_to_device(metal_iface);
            const int degree_rc = degree.copy_to_device(metal_iface);
            const auto param_upload_end =
                std::chrono::high_resolution_clock::now();
            timing->param_upload_ms =
                dng_timing::elapsed_ms(param_upload_start, param_upload_end);
            if (coeff_rc != 0 || degree_rc != 0) {
                std::fprintf(stderr,
                             "[OpcodeList2Halide] polynomial3 param upload "
                             "coeff_rc=%d degree_rc=%d\n",
                             coeff_rc, degree_rc);
                return false;
            }
        }
    }

    const auto kernel_start = std::chrono::high_resolution_clock::now();
    const int rc = dng_opcode_polynomial3(src_buf, coeff, degree,
                                          pixel_range, dst_buf);
    const auto kernel_end = std::chrono::high_resolution_clock::now();
    if (timing) {
        timing->kernel_ms = dng_timing::elapsed_ms(kernel_start, kernel_end);
    }
    if (rc != 0) {
        std::fprintf(stderr,
                     "[OpcodeList2Halide] polynomial3 kernel rc=%d (%dx%d)\n",
                     rc, width, height);
        return false;
    }

    if (defer_copy_to_host) {
        // Leave GPU result device-resident.  Publish dst_buf together with the
        // writeback target so the eventual copy_to_host() call can scatter
        // results back into the SDK image.  scratch_ptr remains valid because
        // poly3_scratch is owned by DeviceHandoffState and is never freed until
        // halide_stage2_ol2_clear_device_handoff() is called.
        const WritebackTarget target{base, col_step, row_step, plane_step};
        const auto handoff_start = std::chrono::high_resolution_clock::now();
        publish_device_handoff(std::move(dst_buf),
                               static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height),
                               3,
                               pixel_range_u32,
                               target);
        const auto handoff_end = std::chrono::high_resolution_clock::now();
        if (timing) {
            timing->handoff_publish_ms =
                dng_timing::elapsed_ms(handoff_start, handoff_end);
            timing->t_inner_end = handoff_end;
            timing->t_func_exit =
                std::chrono::high_resolution_clock::now();
        }
        return true;
    }

    // Immediate path: copy GPU result to scratch, then scatter into image.
    const auto copy_start = std::chrono::high_resolution_clock::now();
    const int copy_rc = dst_buf.copy_to_host();
    const auto copy_end = std::chrono::high_resolution_clock::now();
    if (timing) {
        timing->copy_to_host_ms = dng_timing::elapsed_ms(copy_start, copy_end);
    }
    if (copy_rc != 0) {
        std::fprintf(stderr,
                     "[OpcodeList2Halide] polynomial3 copy_to_host rc=%d\n",
                     copy_rc);
        return false;
    }

    // Scatter dense scratch back into the interleaved dng_image buffer.
    const auto scatter_start = std::chrono::high_resolution_clock::now();
    scatter_poly3_to_image(scratch_ptr, base,
                           width, height, col_step, row_step, plane_step);
    const auto scatter_end = std::chrono::high_resolution_clock::now();
    if (timing) {
        timing->scatter_ms = dng_timing::elapsed_ms(scatter_start, scatter_end);
        timing->t_inner_end = scatter_end;
        timing->t_func_exit = std::chrono::high_resolution_clock::now();
    }
    return true;
}

// Emit the [MapPolynomialColdSubdivide] diagnostic line. Both the batched3 and
// single dispatch paths produce byte-identical output structure; the only
// behavioural difference is whether handoff_publish_ms participates in the
// core_accounted figure (batched3 publishes a device handoff, single does not).
//
// Caller contract (unchanged): only invoke under
// `map_poly_timing_enabled()` after a successful dispatch. The timestamps must
// already be populated by run_polynomial*_kernel via the PolynomialTiming
// struct. path_label is "batched3" or "single"; has_handoff selects whether
// handoff_publish_ms is folded into core_accounted.
void emit_cold_subdivide_log(
    const char *path_label,
    const PolynomialTiming &detail,
    std::chrono::high_resolution_clock::time_point dispatch_start,
    std::chrono::high_resolution_clock::time_point dispatch_end,
    bool has_handoff) {
    const double pre_call_setup_ms =
        dng_timing::elapsed_ms(dispatch_start, detail.t_func_entry);
    const double kernel_func_entry_ms =
        dng_timing::elapsed_ms(detail.t_func_entry, detail.t_inner_start);
    const double prewarm_window_ms =
        dng_timing::elapsed_ms(detail.t_inner_start, detail.t_after_prewarm);
    const double buffer_setup_ms =
        dng_timing::elapsed_ms(detail.t_after_prewarm, detail.t_before_src_upload);
    const double core_window_ms =
        dng_timing::elapsed_ms(detail.t_before_src_upload, detail.t_inner_end);
    const double kernel_func_exit_ms =
        dng_timing::elapsed_ms(detail.t_inner_end, detail.t_func_exit);
    const double post_call_destructors_ms =
        dng_timing::elapsed_ms(detail.t_func_exit, dispatch_end);
    double core_accounted =
        detail.src_upload_ms + detail.param_upload_ms +
        detail.kernel_ms + detail.copy_to_host_ms +
        detail.scatter_ms;
    if (has_handoff) {
        core_accounted += detail.handoff_publish_ms;
    }
    const double core_unaccounted_ms = core_window_ms - core_accounted;
    const double subdiv_sum =
        pre_call_setup_ms + kernel_func_entry_ms + prewarm_window_ms +
        buffer_setup_ms + core_window_ms + kernel_func_exit_ms +
        post_call_destructors_ms;
    std::fprintf(stderr,
                 "[MapPolynomialColdSubdivide] path=%s first_call=%d "
                 "pre_call_setup=%.3f kernel_func_entry=%.3f "
                 "prewarm_window=%.3f buffer_setup=%.3f core_window=%.3f "
                 "core_unaccounted=%.3f kernel_func_exit=%.3f "
                 "post_call_destructors=%.3f subdiv_sum=%.3f "
                 "dispatch_call=%.3f residual=%.3f\n",
                 path_label,
                 detail.first_call ? 1 : 0,
                 pre_call_setup_ms, kernel_func_entry_ms,
                 prewarm_window_ms, buffer_setup_ms, core_window_ms,
                 core_unaccounted_ms, kernel_func_exit_ms,
                 post_call_destructors_ms, subdiv_sum,
                 detail.dispatch_call_ms,
                 detail.dispatch_call_ms - subdiv_sum);
}

}  // namespace

// ---------------------------------------------------------------------------
// Process-level per-size prewarm cache for the batched 3-plane polynomial
// kernel.  Keyed by (width << 32 | height) so each unique image dimension is
// warmed exactly once per process lifetime.
// ---------------------------------------------------------------------------

void halide_prewarm_polynomial3_for_size(int width, int height) {
    if (!ol2_prewarm_enabled() || !stage2_opcodelist2_halide_enabled()) {
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

    // (Candidate A — P1) Pre-grow + page-touch the DeviceHandoffState scratch
    // vector so the first real run_polynomial3_kernel() call avoids the 144MB
    // page-fault zero-init that impl-instrument located as
    // [MapPolynomialColdSubdivide] buffer_setup ≈ 249ms (see
    // docs/logs/2026-05-27/Task_map_polynomial_coldpath_fix.md §2 Candidate A).
    //
    // std::vector::resize default-inits to 0 for uint16_t, which already
    // commits and touches every page; the explicit std::memset(0) below makes
    // the intent durable against any future switch to uninitialised storage.
    const size_t scratch_elems =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    {
        DeviceHandoffState &state = device_handoff_state();
        std::lock_guard<std::mutex> lock(state.mu);
        if (state.poly3_scratch.size() < scratch_elems) {
            state.poly3_scratch.resize(scratch_elems);
        }
        std::memset(state.poly3_scratch.data(), 0,
                    state.poly3_scratch.size() * sizeof(uint16_t));
    }

    // W8/L-7: measured (docs/logs/2026-07-02/Task_w8_opcodelist2.md) that a
    // fixed 256x256 identity dispatch warms the Metal pipeline state just as
    // effectively as dispatching at the actual image dimensions — the
    // generator schedule (DngOpcodePolynomial3Generator::schedule) has no
    // width/height-dependent specialize()/bound() calls, only a fixed 16x16
    // gpu_tile, so PSO compilation is size-independent. Measured on the
    // lossy sample (6000x4000): full-size prewarm ~86-115ms vs 256x256
    // ~33-38ms, with the subsequent real first dispatch's kernel_ms
    // unchanged (~0.07-0.14ms either way) — confirming no re-compile is paid
    // on the real dispatch. poly3_scratch above is still pre-grown to the
    // real (width, height) since the real dispatch writes into it at full
    // size.
    const auto prewarm_start = std::chrono::high_resolution_clock::now();

    constexpr int kPrewarmW = 256;
    constexpr int kPrewarmH = 256;
    Halide::Runtime::Buffer<uint16_t> src =
        Halide::Runtime::Buffer<uint16_t>::make_interleaved(kPrewarmW, kPrewarmH, 3);
    Halide::Runtime::Buffer<uint16_t> dst =
        Halide::Runtime::Buffer<uint16_t>::make_interleaved(kPrewarmW, kPrewarmH, 3);
    std::memset(src.data(), 0, sizeof(uint16_t) * static_cast<size_t>(kPrewarmW) *
                                   static_cast<size_t>(kPrewarmH) * 3);

    // Identity polynomial (degree=1, c1=1) — correct output but no real work.
    float coeff_f32[27] = {0};
    int32_t degree_i32[3] = {1, 1, 1};
    for (int c = 0; c < 3; ++c) {
        coeff_f32[static_cast<size_t>(c) * 9 + 1] = 1.0f;
    }
    Halide::Runtime::Buffer<float> coeff(coeff_f32, 9, 3);
    Halide::Runtime::Buffer<int32_t> degree(degree_i32, 3);
    src.set_host_dirty(true);
    coeff.set_host_dirty(true);
    degree.set_host_dirty(true);

    const int rc = dng_opcode_polynomial3(src, coeff, degree,
                                          /*pixel_range=*/65535.0f, dst);
    dst.copy_to_host();
    (void)rc;

    if (map_poly_timing_enabled()) {
        const auto prewarm_end = std::chrono::high_resolution_clock::now();
        std::fprintf(stderr,
                     "[MapPolynomial3PrewarmForSize] w=%d h=%d total_ms=%.3f\n",
                     width, height,
                     dng_timing::elapsed_ms(prewarm_start, prewarm_end));
    }

    // Mark this dimension as warmed regardless of rc — even a failed dispatch
    // means Metal has attempted compilation; we should not retry on every call.
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        warmed.insert(key);
    }
}

// M-6: public dispatch-failure query and clear functions.
bool halide_stage2_ol2_dispatch_failed() {
    return g_ol2_dispatch_failed.load(std::memory_order_acquire);
}

void halide_stage2_ol2_clear_dispatch_failure() {
    g_ol2_dispatch_failed.store(false, std::memory_order_release);
    g_ol2_dispatch_failure_msg.clear();
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
    state.writeback.reset();
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
    const auto copy_start = std::chrono::high_resolution_clock::now();
    const int rc = state.buffer->copy_to_host();
    const auto copy_end = std::chrono::high_resolution_clock::now();
    if (rc != 0) {
        std::fprintf(stderr,
                     "[OpcodeList2Halide] device_handoff copy_to_host rc=%d\n",
                     rc);
        return false;
    }
    double scatter_ms = 0.0;
    // If the caller recorded a writeback target, scatter scratch → dng_image.
    if (state.writeback.has_target()) {
        const uint16_t *scratch = state.poly3_scratch.data();
        const auto scatter_start = std::chrono::high_resolution_clock::now();
        scatter_poly3_to_image(scratch,
                               state.writeback.base,
                               static_cast<int32_t>(state.width),
                               static_cast<int32_t>(state.height),
                               state.writeback.col_step,
                               state.writeback.row_step,
                               state.writeback.plane_step);
        const auto scatter_end = std::chrono::high_resolution_clock::now();
        scatter_ms = dng_timing::elapsed_ms(scatter_start, scatter_end);
    }
    if (map_poly_timing_enabled()) {
        const double copy_ms = dng_timing::elapsed_ms(copy_start, copy_end);
        std::fprintf(stderr,
                     "[MapPolynomialHandoffTiming] copy_to_host=%.3f "
                     "scatter=%.3f total=%.3f\n",
                     copy_ms, scatter_ms, copy_ms + scatter_ms);
    }
    state.host_copied = true;
    return true;
}

uint32_t halide_try_dispatch_opcode2_batch(dng_host &host,
                                           dng_negative &negative,
                                           dng_opcode_list &list,
                                           uint32_t start_index,
                                           dng_image &image) {
    if (!stage2_opcodelist2_halide_enabled()) {
        return 0;
    }
    ol2_selftest_strided_gather();  // W8/L-6: opt-in, no-op unless DNG_OL2_SELFTEST=1
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
    const auto dispatch_start = std::chrono::high_resolution_clock::now();
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
    const auto dispatch_end = std::chrono::high_resolution_clock::now();
    detail.dispatch_call_ms = dng_timing::elapsed_ms(dispatch_start, dispatch_end);
    if (!ok) {
        // M-6: set dispatch-failure flag and return 0 (= not handled) so the
        // SDK opcode loop can check the flag and break.  Replaces the old
        // throw-as-control-flow via DngOl2DispatchError.
        g_ol2_dispatch_failed.store(true, std::memory_order_release);
        g_ol2_dispatch_failure_msg =
            "[OpcodeList2Halide] batched GPU dispatch failed; refusing SDK CPU fallback";
        std::fprintf(stderr, "%s\n", g_ol2_dispatch_failure_msg.c_str());
        return 0;
    }
    if (map_poly_timing_enabled()) {
        const double total = detail.prewarm_ms + detail.gather_ms +
                             detail.src_upload_ms + detail.param_upload_ms +
                             detail.kernel_ms + detail.copy_to_host_ms +
                             detail.scatter_ms + detail.handoff_publish_ms;
        std::fprintf(stderr,
                     "[MapPolynomialTiming] path=batched3 w=%d h=%d "
                     "defer_handoff=%d prewarm=%.3f gather=%.3f "
                     "src_upload=%.3f param_upload=%.3f kernel=%.3f "
                     "copy_to_host=%.3f scatter=%.3f handoff_publish=%.3f "
                     "inner_total=%.3f dispatch_call=%.3f gap=%.3f\n",
                     width, height, defer_copy_to_host ? 1 : 0,
                     detail.prewarm_ms, detail.gather_ms,
                     detail.src_upload_ms, detail.param_upload_ms,
                     detail.kernel_ms, detail.copy_to_host_ms,
                     detail.scatter_ms, detail.handoff_publish_ms, total,
                     detail.dispatch_call_ms, detail.dispatch_call_ms - total);

        // Cold-path subdivision: split dispatch_call into pre_call /
        // func_entry / prewarm window / buffer_setup / core (uploads+kernel
        // +copy+scatter+handoff) / func_exit / post_call destructors.
        emit_cold_subdivide_log("batched3", detail, dispatch_start,
                                dispatch_end, /*has_handoff=*/true);
    }
    return 3;
}

bool halide_try_dispatch_opcode2(dng_host & /* host */,
                                 dng_opcode &opcode,
                                 dng_image &image) {
    if (!stage2_opcodelist2_halide_enabled()) {
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

    PolynomialTiming detail;
    const auto dispatch_start = std::chrono::high_resolution_clock::now();
    const bool ok = run_polynomial_kernel(base,
                                          width,
                                          height,
                                          col_step,
                                          row_step,
                                          mp->PolyDegree(),
                                          mp->PolyCoefficients(),
                                          static_cast<float>(pixel_range),
                                          &detail);
    const auto dispatch_end = std::chrono::high_resolution_clock::now();
    detail.dispatch_call_ms = dng_timing::elapsed_ms(dispatch_start, dispatch_end);
    if (ok && map_poly_timing_enabled()) {
        const double total = detail.prewarm_ms + detail.gather_ms +
                             detail.src_upload_ms + detail.param_upload_ms +
                             detail.kernel_ms + detail.copy_to_host_ms +
                             detail.scatter_ms;
        std::fprintf(stderr,
                     "[MapPolynomialTiming] path=single plane=%u w=%d h=%d "
                     "prewarm=%.3f gather=%.3f src_upload=%.3f "
                     "param_upload=%.3f kernel=%.3f copy_to_host=%.3f "
                     "scatter=%.3f inner_total=%.3f dispatch_call=%.3f "
                     "gap=%.3f\n",
                     static_cast<unsigned>(plane), width, height,
                     detail.prewarm_ms, detail.gather_ms,
                     detail.src_upload_ms, detail.param_upload_ms,
                     detail.kernel_ms, detail.copy_to_host_ms,
                     detail.scatter_ms, total, detail.dispatch_call_ms,
                     detail.dispatch_call_ms - total);

        emit_cold_subdivide_log("single", detail, dispatch_start,
                                dispatch_end, /*has_handoff=*/false);
    }
    if (!ok) {
        // M-6: set dispatch-failure flag and return false (= not handled) so
        // the SDK opcode loop can check the flag and break.  Replaces the old
        // throw-as-control-flow via DngOl2DispatchError.
        g_ol2_dispatch_failed.store(true, std::memory_order_release);
        g_ol2_dispatch_failure_msg =
            "[OpcodeList2Halide] GPU dispatch failed; refusing SDK CPU fallback";
        std::fprintf(stderr, "%s\n", g_ol2_dispatch_failure_msg.c_str());
        return false;
    }
    return true;
}
