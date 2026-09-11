#include "raw_gpu_pipeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "HalideBuffer.h"
#include "ceyx_decode_into.h"   // WP10 A3: caller-buffer forwarding on the DNG route
#include "ceyx_orient.h"
#include "dng_ffi_api.h"
#include "dng_halide_device.h"
// C2 (plan §4.1.4, §4.2.2, §4.3): the single capability-gate query
// (ceyx::zero_copy_path_is_enabled()) and the shared MTLDevice accessor used
// to wrap the caller's own destination memory as an MTLBuffer. Owned by
// impl-capability-opus (native/src/pipeline/dng_metal_context.{h,cpp}); this
// file only ever calls the published accessors, never re-derives the probe
// (plan §4.1.2's "no second source of truth").
#include "dng_metal_context.h"
#include "dng_pipeline.h"
#include "dng_render_params.h"
#include "libraw_gpu_input_adapter.h"
#include "raw_bayer_demosaic.h"
#include "raw_contract_validate.h"
#include "raw_file_router.h"
#include "raw_linear_rgb_normalize.h"
// C1 (plan §3.2, §2.6): per-lane persistent device arena. Owned by
// impl-b1-opus (native/include/raw_persistent_device_arena.h,
// native/src/pipeline/raw_persistent_device_arena.cpp). nullptr from
// raw_persistent_device_arena_for_current_lane() is always a legal answer
// (non-Metal target, lane ceiling, allocation failure) and every call site
// below takes today's unwrapped path when it sees one (plan §3.4).
#include "raw_persistent_device_arena.h"
#include "raw_render_params_builder.h"
#include "raw_xtrans_demosaic.h"

#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace {

#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)
using ObjectiveCSendNewBufferNoCopy = void* (*)(void*, SEL, void*, unsigned long,
                                                unsigned long, void*);
using ObjectiveCSendReleaseNoArgument = void (*)(void*, SEL);
// MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache == 0,
// same constant raw_persistent_device_arena.cpp uses for its own regions
// (plan §3.1 "Storage mode").
constexpr unsigned long kMetalResourceStorageModeSharedOptions = 0;
#endif  // __APPLE__ && !DNG_FORCE_VULKAN

// C2 (plan §4.2.2, §4.3): wraps the CALLER's own RGBA destination memory as an
// MTLBuffer via newBufferWithBytesNoCopy:length:options:deallocator:, so
// Stage4 can write the caller's bytes directly with zero device->host copy
// (the unified-wrapped path). Metal-only, and constructed unconditionally so
// every call site stays free of #ifdef (plan §4.6): on a non-Metal target, or
// whenever the caller buffer fails the §4.3 page-alignment contract, or when
// no Metal device exists yet, get() is nullptr and the caller falls back to
// the arena's destination region (unified-degraded) -- never a decode
// failure, exactly the alignment probe's "a probe, never a refusal" rule.
// The deallocator block is passed NULL: this class never owns the caller's
// memory, so Metal must not attempt to free it (Apple's documented contract
// for a NULL deallocator on this selector).
class CallerDestinationMetalBufferWrap {
 public:
    CallerDestinationMetalBufferWrap(uint8_t* dst, size_t byte_count) {
#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)
        if (dst == nullptr || byte_count == 0) return;
        if ((reinterpret_cast<uintptr_t>(dst) % ceyx::kRawDeviceArenaAlignmentBytes) != 0 ||
            (byte_count % ceyx::kRawDeviceArenaAlignmentBytes) != 0) {
            return;
        }
        void* const device = ceyx::metal_shared_device_handle();
        if (device == nullptr) return;
        buffer_ = reinterpret_cast<ObjectiveCSendNewBufferNoCopy>(objc_msgSend)(
            device,
            sel_registerName("newBufferWithBytesNoCopy:length:options:deallocator:"),
            dst, static_cast<unsigned long>(byte_count),
            kMetalResourceStorageModeSharedOptions, nullptr);
#else
        (void)dst;
        (void)byte_count;
#endif
    }

    ~CallerDestinationMetalBufferWrap() {
#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)
        if (buffer_ != nullptr) {
            reinterpret_cast<ObjectiveCSendReleaseNoArgument>(objc_msgSend)(
                buffer_, sel_registerName("release"));
        }
#endif
    }

    CallerDestinationMetalBufferWrap(const CallerDestinationMetalBufferWrap&) = delete;
    CallerDestinationMetalBufferWrap& operator=(const CallerDestinationMetalBufferWrap&) = delete;

    void* get() const {
#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)
        return buffer_;
#else
        return nullptr;
#endif
    }

 private:
#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)
    void* buffer_ = nullptr;
#endif
};

// C2 (plan §4.2.1, §4.4): the arena's source-region host pointer to memcpy the
// LibRaw plane into on the unified path, or nullptr to keep today's borrowed
// wrap over `plane.data` (gate off, no arena for this lane, or the region
// could not grow) -- an arena/gate miss is never a decode failure (plan
// §3.4). ensure_region_host_pointer is the C2 read-side counterpart of
// bind_region: same grow-on-demand and failure semantics, but returns the
// region's CPU-visible (MTLStorageModeShared) pointer instead of wrapping a
// halide_buffer_t, because the memcpy must land BEFORE construction of the
// Halide source view (plan §4.2.1's "construct the ... Buffer over that arena
// host pointer").
void* acquireZeroCopySourceHost(ceyx::RawPersistentDeviceArena* arena,
                                bool use_zero_copy, size_t required_bytes) {
    if (!use_zero_copy || arena == nullptr) return nullptr;
    void* const host = arena->ensure_region_host_pointer(
        ceyx::RawDeviceArenaRegion::kSourceMosaicRegion, required_bytes);
    // C2 (plan §4.5): counted per decode that actually took the unified
    // source path, so a build with the arena silently disabled cannot pass
    // this counter's non-zero expectation.
    if (host != nullptr) {
        ceyx::zero_copy_note_source_mosaic_wrapped();
    }
    return host;
}

double nowMs() {
    // There is no dng_now_ms() in this tree; include/dng_timing_utils.h only
    // exposes an elapsed helper, so the clock read is spelled out here.
    using Clock = std::chrono::high_resolution_clock;
    return std::chrono::duration<double, std::milli>(
               Clock::now().time_since_epoch())
        .count();
}

// Borrow-only. There is no owning mode and no pool to acquire from: every entry
// that reaches here supplies a caller-owned buffer (WP3 Task 3.2 proved the
// owning mode unreachable — docs/logs/2026-09-07/pool-retire-v2-t32-verdict.txt).
//
// This is what makes invariant I1 STRUCTURAL rather than reviewed: there is no
// code path from a RgbaCheckoutGuard to any pool release, so a caller-owned
// (Dart-owned) address cannot be handed back to the pool — not because every
// branch remembers not to, but because nothing here releases to a pool at all.
class RgbaCheckoutGuard {
 public:
    RgbaCheckoutGuard(uint8_t* borrowed, size_t bytes)
        : ptr_(borrowed), bytes_(bytes) {}
    ~RgbaCheckoutGuard() { ptr_ = nullptr; }
    RgbaCheckoutGuard(const RgbaCheckoutGuard&) = delete;
    RgbaCheckoutGuard& operator=(const RgbaCheckoutGuard&) = delete;
    uint8_t* get() const { return ptr_; }
    size_t bytes() const { return bytes_; }
    // Hands the caller's own pointer back to the branch, which is exactly the
    // behaviour the three `out.rgba_ptr = rgba.release()` lines already want.
    uint8_t* release() { uint8_t* p = ptr_; ptr_ = nullptr; return p; }
 private:
    uint8_t* ptr_;
    size_t bytes_;
};

// Trust-boundary extent check (spec section 10.1). Every product is formed in
// uint64_t, because the whole point is to catch the value that would wrap the
// narrower type the allocator uses. Returns false when the extent must never
// reach an allocator.
bool extentWithinCeiling(uint64_t width, uint64_t height, const char* what) {
    const uint64_t pixels = width * height;
    // width and height are 32-bit fields, so their product cannot overflow
    // uint64_t; the ceiling below is what keeps pixels*4 (the RGBA product) far
    // inside both uint64_t and size_t.
    if (pixels > static_cast<uint64_t>(kRawMaxPixelCount)) {
        std::fprintf(stderr,
                     "[RawPipeline] declared %s extent %llux%llu = %llu pixels "
                     "exceeds the %lld ceiling\n",
                     what, static_cast<unsigned long long>(width),
                     static_cast<unsigned long long>(height),
                     static_cast<unsigned long long>(pixels),
                     static_cast<long long>(kRawMaxPixelCount));
        return false;
    }
    return true;
}

// WP10: the ONE place that decides pool-vs-caller for the RGBA output. All
// three GPU branches call this instead of constructing a guard directly, so
// they cannot drift apart — "only the Bayer branch got converted" (risk R11.3)
// is not expressible here.
//
// Returns kRawErrDstTooSmall when a caller buffer is present but too small
// (the in-decode backstop for a stale probe: a refused decode with the correct
// extent, never a write past the end), and kRawErrAllocationFailed when a pool
// acquire fails, which is what the three branches used to report themselves.
//
// WP10 extent propagation: `out` is non-const because the TRUE post-unpack
// extent is published HERE, before any early return. That is what makes the
// refusal actionable rather than merely safe.
RawErrorCode makeRgbaCheckout(RawPipelineResult& out, size_t bytes,
                              std::optional<RgbaCheckoutGuard>* guard,
                              uint32_t out_w, uint32_t out_h) {
    // Publish the real extent FIRST, so every exit carries it — including the
    // kRawErrDstTooSmall early return below.
    //
    // Why here and not at the three call sites: on the refusal path the
    // branches' own `out.width = out_w` lines are never reached, so the result
    // kept its post-reset zeros, the FFI layer fell back to the PROBE's stale
    // extent, and a caller that resized to what it was told re-acquired
    // exactly the same insufficient buffer — the retry could never advance.
    // Assigning at the three call sites would fix it too, but it would
    // recreate the drift surface this factory exists to remove (risk R11.3):
    // a fourth branch, or an edit to one of the three, silently restores the
    // bug on one route only. The function that owns the pool-vs-caller
    // decision also owns publishing the extent that decision was made against.
    //
    // Idempotent on the success path: the branches assign the same out_w/out_h
    // again later, so no existing behaviour changes.
    out.width = out_w;
    out.height = out_h;

    if (out.caller_dst) {
        if (out.caller_dst_capacity < bytes) {
            std::fprintf(stderr,
                         "[RawPipeline] caller buffer %zu < needed %zu for "
                         "%ux%u\n",
                         out.caller_dst_capacity, bytes, out_w, out_h);
            return kRawErrDstTooSmall;
        }
        guard->emplace(out.caller_dst, bytes);
        return kRawSuccess;
    }
    // WP3: there is no pool to fall back to. Every surviving entry supplies
    // a caller buffer (Task 3.2's reachability proof), so a null here means
    // a programming error in a NEW caller. Reported, not served.
    std::fprintf(stderr,
                 "[RawPipeline] makeRgbaCheckout called with no caller_dst\n");
    return kRawErrAllocationFailed;
}

bool cancelRequested(const RawCancelToken& cancel) {
    return cancel.callback && cancel.callback(cancel.user_data) != 0;
}

RawGpuBackend currentGpuBackend() {
#if defined(__APPLE__)
    return kRawGpuBackendMetal;
#else
    return kRawGpuBackendVulkan;
#endif
}

// Single black/white scale for the fused normalize expression.
float computeInvRange(const RawGpuInput& input) {
    const uint32_t bw = input.black.repeat_width ? input.black.repeat_width : 1;
    const uint32_t bh = input.black.repeat_height ? input.black.repeat_height : 1;
    float black_max = 0.0f;
    for (uint32_t i = 0; i < bw * bh; ++i) {
        if (input.black.values[i] > black_max) black_max = input.black.values[i];
    }
    const float range = input.white_level[0] - black_max;
    return range > 0.0f ? 65535.0f / range : 1.0f;
}

// Linear-RGB counterpart. Deliberately a separate function rather than a branch
// inside computeInvRange: the two take their black level from DIFFERENT contract
// fields (spatial tile vs per-component vector), so a shared function would have
// to consult the layout class -- exactly the hidden coupling the dispatch rule
// forbids. For this layout the spatial tile is a 1x1 zero by construction
// (libraw_gpu_input_adapter.cpp), so reading it here would under-scale the image.
float computeInvRangeLinearRgb(const RawGpuInput& input) {
    float black_max = 0.0f;
    for (int c = 0; c < 3; ++c) {
        if (input.component_black[c] > black_max) black_max = input.component_black[c];
    }
    const float range = input.white_level[0] - black_max;
    return range > 0.0f ? 65535.0f / range : 1.0f;
}

// Scaled decode output extent, mirroring the DNG path's MaximumSize cap
// (dng_pipeline.cpp). max_long_edge == 0 or >= the source long edge means
// full resolution (never upscale) — dst == src, so the shared Stage4 entry
// stays bit-identical to the pre-scaled crop path. Otherwise scale both edges
// by the same factor, preserving aspect, rounding to nearest, clamped to >= 1.
void scaledOutputExtent(uint32_t src_w, uint32_t src_h, uint32_t max_long_edge,
                        uint32_t* dst_w, uint32_t* dst_h) {
    const uint32_t long_edge = std::max(src_w, src_h);
    if (max_long_edge == 0 || max_long_edge >= long_edge || long_edge == 0) {
        *dst_w = src_w;
        *dst_h = src_h;
        return;
    }
    const double s = static_cast<double>(max_long_edge) / long_edge;
    *dst_w = std::max<uint32_t>(1u, static_cast<uint32_t>(std::llround(src_w * s)));
    *dst_h = std::max<uint32_t>(1u, static_cast<uint32_t>(std::llround(src_h * s)));
}

RawErrorCode runBayerBranch(const RawGpuInput& input,
                            const RawDevelopParams& develop,
                            RawPipelineResult& out) {
    int32_t red_x = 0, red_y = 0;
    if (!raw_bayer_phase_from_pattern(&input.layout, &red_x, &red_y)) {
        // No RGGB fallback on purpose: a guessed phase silently mis-colours the
        // image (spec section 3.3.5).
        return kRawErrLayoutUnsupported;
    }

    const RawPlaneView& plane = input.planes[0];
    const uint32_t w = plane.width;
    const uint32_t h = plane.height;
    if (w == 0 || h == 0 || !plane.data) return kRawErrMetadataInvalid;

    // The Stage4 crop is expressed in plane coordinates, exactly like the DNG
    // route's DefaultCropArea against its Stage3 buffer
    // (src/dng_render_halide.cpp:1988-2008).
    const RawRect& crop = input.default_crop;
    if (crop.width == 0 || crop.height == 0 ||
        crop.x < 0 || crop.y < 0 ||
        static_cast<uint32_t>(crop.x) + crop.width > w ||
        static_cast<uint32_t>(crop.y) + crop.height > h) {
        return kRawErrMetadataInvalid;
    }

    // C2 (plan §4.1.4): the single capability-gate query. Computed once and
    // reused for both the source and destination decisions below, never
    // re-derived (plan §4.1.2).
    const bool use_zero_copy = ceyx::zero_copy_path_is_enabled();
    ceyx::RawPersistentDeviceArena* arena = ceyx::raw_persistent_device_arena_for_current_lane();

    // C2 (plan §4.2.1): on the unified path, memcpy the plane rows into the
    // arena's source region BEFORE constructing the Halide view, so the
    // view's host pointer IS the arena's shared-storage memory (true
    // zero-copy from that point on). A null return (gate off, no lane arena,
    // or the region could not grow) keeps today's borrowed wrap over
    // `plane.data` -- structurally the same branch, just a different source
    // pointer (plan §4.4: one branch per buffer, not a forked pipeline).
    const size_t src_required_bytes = static_cast<size_t>(plane.row_stride_bytes) * h;
    void* const zero_copy_src_host =
        acquireZeroCopySourceHost(arena, use_zero_copy, src_required_bytes);
    const uint16_t* src_host_ptr = static_cast<const uint16_t*>(plane.data);
    // R4-T4 S3: this memcpy runs before gpu_t0 opens below, so it was
    // previously billed to NOTHING (not raw_unpack_ms, which is sampled
    // before this function runs; not gpu_process_ms/host_to_device_copy_ms,
    // which start after this line). Time it explicitly and report it on its
    // OWN field, out.timing.source_mosaic_copy_milliseconds, set further
    // down -- round-4 review B1: folding it into host_to_device_copy_ms
    // broke the gpu_submit_wait_ms == gpu_process_ms - host_copy_ms identity
    // (the memcpy is not inside the gpu_process_ms window it would have been
    // subtracted from), so it stays additive and separate instead.
    double source_memcpy_ms = 0.0;
    if (zero_copy_src_host != nullptr) {
        // R4-T4 S5: copy full rows for all but the last, and only the pixel
        // bytes (w * sizeof(uint16_t)) for the last row, so the read never
        // extends past the last pixel of `plane.data` -- row_stride_bytes
        // can exceed w*2 (padding), and the previous full-stride*h memcpy
        // read up to that padding on the final row, which is genuinely
        // outside the pixel rectangle (no documented LibRaw allocation
        // guarantee was found to cite instead).
        const double memcpy_t0 = nowMs();
        const size_t row_bytes = static_cast<size_t>(plane.row_stride_bytes);
        const size_t last_row_bytes = static_cast<size_t>(w) * sizeof(uint16_t);
        if (h > 1) {
            std::memcpy(zero_copy_src_host, plane.data, row_bytes * (h - 1));
        }
        std::memcpy(static_cast<uint8_t*>(zero_copy_src_host) + row_bytes * (h - 1),
                    static_cast<const uint8_t*>(plane.data) + row_bytes * (h - 1),
                    last_row_bytes);
        source_memcpy_ms = nowMs() - memcpy_t0;
        src_host_ptr = static_cast<const uint16_t*>(zero_copy_src_host);
    }

    // Borrowed, stride-aware wrap: no host copy (spec section 5.2.1).
    halide_dimension_t src_dims[2] = {
        {0, static_cast<int32_t>(w), 1, 0},
        {0, static_cast<int32_t>(h),
         static_cast<int32_t>(plane.row_stride_bytes / 2), 0}};
    Halide::Runtime::Buffer<const uint16_t> src_buf(src_host_ptr, 2, src_dims);

    const uint32_t bw = input.black.repeat_width ? input.black.repeat_width : 1;
    const uint32_t bh = input.black.repeat_height ? input.black.repeat_height : 1;
    Halide::Runtime::Buffer<const float> black_buf(
        input.black.values, static_cast<int>(bw), static_cast<int>(bh));

    // Interleaved RGB16 intermediate, byte-identical in shape to the DNG
    // route's device-handoff buffer (src/dng_warp_halide.cpp:1123). Left
    // device-dirty for the handoff, so there is no GPU->host->GPU round trip
    // (spec section 5.2.3). The host allocation is never read on the success
    // path; it exists because the shared Stage4 entry accepts a host-backed
    // halide_buffer_t and Halide device-mallocs on first dispatch.
    // ponytail: host side stays resident for the whole decode, same as the DNG
    // route's Stage3 workspace; swap to a device-only allocation only if a
    // measurement shows this footprint matters.
    Halide::Runtime::Buffer<uint16_t> stage3 =
        Halide::Runtime::Buffer<uint16_t>::make_interleaved(
            static_cast<int>(w), static_cast<int>(h), 3);

    // C1 (plan §3.2 item 1-2, §2.6): wrap the source mosaic and Stage3
    // intermediate onto their arena regions (arena acquired above, shared
    // with the C2 gate/memcpy decision). Scope-lifetime bindings: they detach
    // (RAII) at branch exit, i.e. AFTER the Stage4 call below returns, per
    // plan §3.2's requirement that the RAII scope span the kernel calls.
    // arena == nullptr and/or a failed bind_region() both leave the bindings
    // falsy and the buffers exactly as they are today (plan §3.4 — arena
    // absence is never a decode failure). On the zero-copy path src_buf's
    // host pointer is already the arena's own memory (set above), so this
    // bind just attaches the SAME region's device counterpart -- no data
    // movement here.
    ceyx::RawDeviceArenaRegionBinding src_arena_binding(
        arena, src_buf.raw_buffer(), ceyx::RawDeviceArenaRegion::kSourceMosaicRegion,
        src_required_bytes);
    ceyx::RawDeviceArenaRegionBinding stage3_arena_binding(
        arena, stage3.raw_buffer(),
        ceyx::RawDeviceArenaRegion::kStageThreeInterleavedRgb16Region,
        static_cast<size_t>(w) * h * 3 * sizeof(uint16_t));

    // GPU targets only upload an input whose host_dirty flag is set; without
    // these the kernel reads freshly device-malloc'd memory. Same handshake as
    // src/raw_demosaic_reference.cpp:139-141. C1 (plan §3.5 "Explicitly NOT
    // changed by C1"): set_host_dirty() stays on the fallback path — C1
    // changes where the bytes live, not how many times they move. C2 (plan
    // §4.2.1 item 2): when the memcpy above actually landed the bytes in the
    // arena's shared-storage memory, host and device are the SAME bytes, so
    // marking either dirty would re-introduce a transfer that shared storage
    // makes unnecessary -- both flags are cleared instead.
    if (zero_copy_src_host != nullptr) {
        src_buf.set_host_dirty(false);
        src_buf.set_device_dirty(false);
    } else {
        src_buf.set_host_dirty();
    }
    black_buf.set_host_dirty();
    stage3.set_host_dirty(false);

    const double gpu_t0 = nowMs();
    // C4 (plan §6.2 item 1): explicit host->device upload, timed. This is a
    // re-attribution, not added work -- the kernel below finds src_buf clean
    // and performs no copy of its own (Halide's copy_to_device is a no-op
    // when the buffer is not host-dirty).
    const double h2d_t0 = nowMs();
    const int h2d_rc = src_buf.copy_to_device(dng_halide_gpu_device_interface());
    // R4-T4 S3 (round-4 review B1): NOT folding source_memcpy_ms in here --
    // this bracket is the pure copy_to_device window, so
    // gpu_submit_wait_ms == gpu_process_ms - host_copy_ms keeps holding.
    // source_memcpy_ms is reported separately via
    // out.timing.source_mosaic_copy_milliseconds below.
    const double host_to_device_copy_ms = nowMs() - h2d_t0;
    // R1 should-fix #6: a non-zero RC here means the upload itself failed, so
    // the kernel below would read undefined/stale device memory. Fail via the
    // existing kernel-failure path -- host_to_device_copy_ms stays a local
    // that is never assigned into out.timing on this early return, so a
    // failed copy is never reported as a measured duration.
    if (h2d_rc != 0) {
        return kRawErrKernelFailed;
    }
    if (raw_bayer_demosaic(src_buf, red_x, red_y, black_buf,
                           computeInvRange(input), stage3) != 0) {
        return kRawErrKernelFailed;
    }

    RenderParams params;
    if (!raw_build_render_params(input, develop, params)) {
        return kRawErrMetadataInvalid;
    }

    // Scaled decode: src is the full crop; dst is the (possibly) downscaled
    // output extent. On the macOS/Metal build the shared Stage4 entry runs the
    // pre-average scaled AOT when they differ, and is bit-identical to the
    // previous crop path when equal. Split (Vulkan/Android/Linux) builds never
    // reach here with a downscale — raw_pipeline_decode_to_rgba rejects it up
    // front (no scaled AOT exists there, matching the DNG path / AC-D1).
    const uint32_t src_w = crop.width;
    const uint32_t src_h = crop.height;
    uint32_t out_w = 0, out_h = 0;
    scaledOutputExtent(src_w, src_h, develop.max_output_long_edge, &out_w, &out_h);

    // Bug fix (post-Task-3 review): dst_w/dst_h passed to the low-level kernel
    // entry below stay the UNORIENTED extent (§1.3), but unlike the high-level
    // bridge wrappers (render_stage4_halide_from_device_buffer), the low-level
    // runRenderStage4HalideAotFromDevice used on this route has NO out_w/out_h
    // reference params to write the oriented extent back through -- so this
    // caller must derive and publish the oriented extent itself, or every
    // transposing orientation (5-8) reports/allocates the wrong (unswapped)
    // dimensions. Mirrors §1.3's derivation exactly.
    const bool transposes =
        ceyx_orientation_transposes_inline(develop.exif_orientation);
    const uint32_t oriented_w = transposes ? out_h : out_w;
    const uint32_t oriented_h = transposes ? out_w : out_h;
    const size_t rgba_bytes = static_cast<size_t>(oriented_w) * oriented_h * 4;

    // WP10: pool-vs-caller is decided in makeRgbaCheckout and nowhere else, so
    // all three branches stay structurally identical to one another.
    // oriented_w/oriented_h (not out_w/out_h) is what makeRgbaCheckout
    // publishes into out.width/out.height and checks the caller buffer
    // against -- the caller's buffer was sized against the (oriented) probe.
    std::optional<RgbaCheckoutGuard> rgba;
    if (const RawErrorCode grc =
            makeRgbaCheckout(out, rgba_bytes, &rgba, oriented_w, oriented_h);
        grc != kRawSuccess) {
        return grc;
    }

    // C2 (plan §4.2.2, §4.3): attempt to wrap the caller's OWN destination
    // memory as an MTLBuffer, so Stage4 writes it directly (unified-wrapped)
    // instead of the arena's destination region. Only attempted when the
    // gate is on AND the probe forwarded via `develop` says the caller's
    // buffer actually meets the page-alignment contract -- that probe is
    // computed once, upstream, in ceyxDecodeIntoPrepare (plan §4.3 "the one
    // place both entries share"). Constructed unconditionally so no call site
    // needs #ifdef (plan §4.6); its own internal check is a second,
    // structural guard against a wrap ever being attempted on memory that
    // fails the contract. When neither the wrap is attempted nor it succeeds,
    // Stage4 falls back to the arena's kDestinationRgba8Region
    // (unified-degraded) or today's behaviour -- never a decode failure.
    // Destroyed (RAII) after the Stage4 call returns on every exit path,
    // including the failure return just below.
    //
    // Counters (plan §4.5, names per impl-capability-opus's dng_metal_context.h
    // announcement): zero_copy_note_destination_alignment_degraded() marks
    // the decode where the gate was ON but the destination could not be
    // wrapped for alignment reasons -- the one decision this file is in a
    // position to see (Stage4 cannot distinguish "degraded" from "plain
    // fallback"; both arrive there as a null caller_destination_metal_buffer).
    std::optional<CallerDestinationMetalBufferWrap> dst_metal_wrap;
    if (use_zero_copy && develop.caller_destination_is_page_aligned) {
        // R3 gate-13/14 root cause fix: newBufferWithBytesNoCopy requires the
        // LENGTH argument itself to be a page multiple (§4.3), not just the
        // pointer. rgba_bytes (oriented_w*oriented_h*4, the exact image byte
        // count) is essentially never a multiple of kRawDeviceArenaAlignmentBytes,
        // so passing it made this class's own internal length check reject
        // the wrap on every decode, silently degrading regardless of how well
        // aligned the caller's real buffer was -- no refusal ever reached
        // Stage4 because the wrap was never attempted successfully here.
        // out.caller_dst_capacity is what the alignment probe upstream
        // (ceyxDecodeIntoPrepare) actually checked for page-multiple-ness
        // before setting caller_destination_is_page_aligned, and
        // makeRgbaCheckout's kRawErrDstTooSmall guard already proved
        // caller_dst_capacity >= rgba_bytes earlier in this branch, so the
        // MTLBuffer covers the full extent Stage4 will write.
        dst_metal_wrap.emplace(rgba->get(), out.caller_dst_capacity);
    } else if (use_zero_copy) {
        ceyx::zero_copy_note_destination_alignment_degraded();
    }
    void* const caller_destination_metal_buffer =
        dst_metal_wrap ? dst_metal_wrap->get() : nullptr;
    if (use_zero_copy && caller_destination_metal_buffer == nullptr &&
        dst_metal_wrap.has_value()) {
        // The wrap was attempted (alignment probe said yes) but
        // newBufferWithBytesNoCopy itself failed (e.g. no Metal device) --
        // still a degraded decode, same counter.
        ceyx::zero_copy_note_destination_alignment_degraded();
    }

    // src extent (crop) vs dst extent (scaled): equal on the full-res path, so
    // the shared Stage4 takes the crop branch at
    // src/dng_render_halide.cpp:1261-1269, which does the raw->dim[i].min = 0
    // mutation itself (never set_min/translate, which would trigger
    // device_deallocate — memory.md Key Gotchas); when they differ it dispatches
    // the pre-average scaled AOT instead (same entry the DNG path uses).
    if (!runRenderStage4HalideAotFromDevice(stage3.raw_buffer(),
                                            1.0f / 65535.0f,
                                            crop.x, crop.y,
                                            static_cast<int>(src_w),
                                            static_cast<int>(src_h),
                                            static_cast<int>(out_w),
                                            static_cast<int>(out_h),
                                            params, rgba->get(),
                                            /*ctx=*/nullptr,
                                            develop.exif_orientation,
                                            arena,
                                            caller_destination_metal_buffer)) {
        return kRawErrKernelFailed;
    }

    out.diag.gpu_process_ms = nowMs() - gpu_t0;
    // C4 (plan §6.2/§6.4/§6.7): device_to_host_copy_ms is read back from
    // dng_render_halide.cpp's own bracket around its copy_to_host call
    // (impl-2-sonnet's accessor, dng_render_params.h) rather than bracketed
    // here, since that call happens inside runRenderStage4HalideAotFromDevice
    // itself -- out of this task's file ownership. host_copy_ms and
    // gpu_submit_wait_ms are computed per the formula (plan §6.2 item 3) so
    // the identity gpu_submit_wait_ms == gpu_process_ms - host_copy_ms always
    // holds.
    out.timing.host_to_device_copy_ms = host_to_device_copy_ms;
    // R4-T4 S3 (round-4 review B1): named separately, never folded into
    // host_to_device_copy_ms (see the h2d bracket comment above).
    out.timing.source_mosaic_copy_milliseconds = source_memcpy_ms;
    out.timing.device_to_host_copy_ms =
        runRenderStage4LastDeviceToHostCopyMilliseconds();
    out.timing.host_copy_ms =
        out.timing.host_to_device_copy_ms + out.timing.device_to_host_copy_ms;
    out.timing.gpu_submit_wait_ms =
        out.diag.gpu_process_ms - out.timing.host_copy_ms;
    // C2 (round-close audit): read on the same thread, immediately after the
    // Stage4 call above. Reports "this decode wrote the caller's pages
    // directly" -- false covers both "no buffer was passed" and "the wrap was
    // attempted but refused", not a gate-state signal.
    out.timing.unified_memory_path_active =
        runRenderStage4LastCallerDestinationWrapWasUsed() ? 1u : 0u;
    out.width = oriented_w;
    out.height = oriented_h;
    out.rgba_size = rgba_bytes;
    out.rgba_ptr = rgba->release();   // ownership moves to the caller
    return kRawSuccess;
}

// Structurally identical to runBayerBranch: same borrowed stride-aware wrap,
// same interleaved RGB16 device-dirty intermediate, the SAME shared Stage4
// entry, the same RGBA pool checkout and the same ownership ordering. The only
// differences are which AOT kernel runs and that the validated 6x6 CFA tile is
// passed instead of the Bayer red-site phase.
RawErrorCode runXTransBranch(const RawGpuInput& input,
                             const RawDevelopParams& develop,
                             RawPipelineResult& out) {
    // Driven by the VALIDATED descriptor only: never by a decoder's mosaic
    // shorthand and never by a camera make/model test (spec section 6.4.5).
    // raw_classify_layout has already accepted the arrangement; this guard is
    // the local precondition of the fixed-size copy below.
    if (!input.layout.cfa_pattern || input.layout.cfa_pattern_count != 36) {
        return kRawErrLayoutUnsupported;
    }
    // The kernel's channel test is `own == c` for c in {0,1,2} only, so the
    // copy must NORMALISE, not transcribe: kRawColorKeyFujiGreen (7) is green
    // at 6x6 (the S4 carve-out in raw_contract_validate.cpp's
    // xtransColorIndex), and handing the raw 7 to the kernel matched no channel
    // at all - every green site was discarded and the green plane collapsed to
    // zero for a tile the validator explicitly blesses (round-6 review finding
    // F1 / S-R6-01, measured: fuji_green_mean=0.00). Anything outside {0,1,2}
    // after that mapping is a key the kernel has no channel for, and is
    // rejected rather than silently reinterpreted: the kernel contract is total
    // or it is a guess.
    int32_t cfa[36];
    for (int i = 0; i < 36; ++i) {
        const RawColorKey key = input.layout.cfa_pattern[i];
        const int32_t channel = (key == kRawColorKeyFujiGreen)
                                    ? static_cast<int32_t>(kRawColorKeyGreen)
                                    : static_cast<int32_t>(key);
        if (channel < 0 || channel > 2) {
            std::fprintf(stderr,
                         "[RawPipeline] X-Trans tile slot %d carries colour key "
                         "%d, which no kernel channel matches\n",
                         i, static_cast<int>(key));
            return kRawErrLayoutUnsupported;
        }
        cfa[i] = channel;
    }

    const RawPlaneView& plane = input.planes[0];
    const uint32_t w = plane.width;
    const uint32_t h = plane.height;
    if (w == 0 || h == 0 || !plane.data) return kRawErrMetadataInvalid;

    const RawRect& crop = input.default_crop;
    if (crop.width == 0 || crop.height == 0 ||
        crop.x < 0 || crop.y < 0 ||
        static_cast<uint32_t>(crop.x) + crop.width > w ||
        static_cast<uint32_t>(crop.y) + crop.height > h) {
        return kRawErrMetadataInvalid;
    }

    // C2 (plan §4.1.4): see runBayerBranch's identical comment.
    const bool use_zero_copy = ceyx::zero_copy_path_is_enabled();
    ceyx::RawPersistentDeviceArena* arena = ceyx::raw_persistent_device_arena_for_current_lane();

    // C2 (plan §4.2.1): see runBayerBranch's identical comment.
    const size_t src_required_bytes = static_cast<size_t>(plane.row_stride_bytes) * h;
    void* const zero_copy_src_host =
        acquireZeroCopySourceHost(arena, use_zero_copy, src_required_bytes);
    const uint16_t* src_host_ptr = static_cast<const uint16_t*>(plane.data);
    // R4-T4 S3/S5: see runBayerBranch's identical comment -- timed and
    // reported on its own field below, last row copied short to avoid
    // reading past the pixel rectangle.
    double source_memcpy_ms = 0.0;
    if (zero_copy_src_host != nullptr) {
        const double memcpy_t0 = nowMs();
        const size_t row_bytes = static_cast<size_t>(plane.row_stride_bytes);
        const size_t last_row_bytes = static_cast<size_t>(w) * sizeof(uint16_t);
        if (h > 1) {
            std::memcpy(zero_copy_src_host, plane.data, row_bytes * (h - 1));
        }
        std::memcpy(static_cast<uint8_t*>(zero_copy_src_host) + row_bytes * (h - 1),
                    static_cast<const uint8_t*>(plane.data) + row_bytes * (h - 1),
                    last_row_bytes);
        source_memcpy_ms = nowMs() - memcpy_t0;
        src_host_ptr = static_cast<const uint16_t*>(zero_copy_src_host);
    }

    halide_dimension_t src_dims[2] = {
        {0, static_cast<int32_t>(w), 1, 0},
        {0, static_cast<int32_t>(h),
         static_cast<int32_t>(plane.row_stride_bytes / 2), 0}};
    Halide::Runtime::Buffer<const uint16_t> src_buf(src_host_ptr, 2, src_dims);

    // The kernel indexes the tile as cfa(x % 6, y % 6) with dim 0 stride 1, so
    // the row-major descriptor array wraps directly (src/raw_demosaic_
    // reference.cpp:228 builds the identical buffer for the same kernel).
    Halide::Runtime::Buffer<const int32_t> cfa_buf(cfa, 6, 6);

    const uint32_t bw = input.black.repeat_width ? input.black.repeat_width : 1;
    const uint32_t bh = input.black.repeat_height ? input.black.repeat_height : 1;
    Halide::Runtime::Buffer<const float> black_buf(
        input.black.values, static_cast<int>(bw), static_cast<int>(bh));

    Halide::Runtime::Buffer<uint16_t> stage3 =
        Halide::Runtime::Buffer<uint16_t>::make_interleaved(
            static_cast<int>(w), static_cast<int>(h), 3);

    // C1 (plan §3.2, §2.6): same lane-arena bind as runBayerBranch — see that
    // branch's comment for the full rationale. Structurally identical sibling
    // per §3.5's change list. arena acquired above, shared with the C2 gate.
    ceyx::RawDeviceArenaRegionBinding src_arena_binding(
        arena, src_buf.raw_buffer(), ceyx::RawDeviceArenaRegion::kSourceMosaicRegion,
        src_required_bytes);
    ceyx::RawDeviceArenaRegionBinding stage3_arena_binding(
        arena, stage3.raw_buffer(),
        ceyx::RawDeviceArenaRegion::kStageThreeInterleavedRgb16Region,
        static_cast<size_t>(w) * h * 3 * sizeof(uint16_t));

    // C2 (plan §4.2.1 item 2): see runBayerBranch's identical comment.
    if (zero_copy_src_host != nullptr) {
        src_buf.set_host_dirty(false);
        src_buf.set_device_dirty(false);
    } else {
        src_buf.set_host_dirty();
    }
    cfa_buf.set_host_dirty();
    black_buf.set_host_dirty();
    stage3.set_host_dirty(false);

    const double gpu_t0 = nowMs();
    // C4 (plan §6.2 item 1): explicit host->device upload, timed. Same
    // re-attribution as runBayerBranch -- the kernel finds src_buf clean.
    const double h2d_t0 = nowMs();
    const int h2d_rc = src_buf.copy_to_device(dng_halide_gpu_device_interface());
    // R4-T4 S3 (round-4 review B1): see runBayerBranch's identical comment.
    const double host_to_device_copy_ms = nowMs() - h2d_t0;
    // R1 should-fix #6: same convention as runBayerBranch -- fail via the
    // existing kernel-failure path on a non-zero RC, so host_to_device_copy_ms
    // is never reported for a failed copy.
    if (h2d_rc != 0) {
        return kRawErrKernelFailed;
    }
    if (raw_xtrans_demosaic(src_buf, cfa_buf, black_buf,
                            computeInvRange(input), stage3) != 0) {
        return kRawErrKernelFailed;
    }

    RenderParams params;
    if (!raw_build_render_params(input, develop, params)) {
        return kRawErrMetadataInvalid;
    }

    // Scaled decode: src is the full crop; dst is the (possibly) downscaled
    // output extent. On the macOS/Metal build the shared Stage4 entry runs the
    // pre-average scaled AOT when they differ, and is bit-identical to the
    // previous crop path when equal. Split (Vulkan/Android/Linux) builds never
    // reach here with a downscale — raw_pipeline_decode_to_rgba rejects it up
    // front (no scaled AOT exists there, matching the DNG path / AC-D1).
    const uint32_t src_w = crop.width;
    const uint32_t src_h = crop.height;
    uint32_t out_w = 0, out_h = 0;
    scaledOutputExtent(src_w, src_h, develop.max_output_long_edge, &out_w, &out_h);

    // Bug fix (post-Task-3 review): dst_w/dst_h passed to the low-level kernel
    // entry below stay the UNORIENTED extent (§1.3), but unlike the high-level
    // bridge wrappers (render_stage4_halide_from_device_buffer), the low-level
    // runRenderStage4HalideAotFromDevice used on this route has NO out_w/out_h
    // reference params to write the oriented extent back through -- so this
    // caller must derive and publish the oriented extent itself, or every
    // transposing orientation (5-8) reports/allocates the wrong (unswapped)
    // dimensions. Mirrors §1.3's derivation exactly.
    const bool transposes =
        ceyx_orientation_transposes_inline(develop.exif_orientation);
    const uint32_t oriented_w = transposes ? out_h : out_w;
    const uint32_t oriented_h = transposes ? out_w : out_h;
    const size_t rgba_bytes = static_cast<size_t>(oriented_w) * oriented_h * 4;

    // WP10: pool-vs-caller is decided in makeRgbaCheckout and nowhere else, so
    // all three branches stay structurally identical to one another.
    // oriented_w/oriented_h (not out_w/out_h) is what makeRgbaCheckout
    // publishes into out.width/out.height and checks the caller buffer
    // against -- the caller's buffer was sized against the (oriented) probe.
    std::optional<RgbaCheckoutGuard> rgba;
    if (const RawErrorCode grc =
            makeRgbaCheckout(out, rgba_bytes, &rgba, oriented_w, oriented_h);
        grc != kRawSuccess) {
        return grc;
    }

    // C2 (plan §4.2.2, §4.3): see runBayerBranch's identical comment -- same
    // wrap attempt (gated on develop.caller_destination_is_page_aligned),
    // same degraded-counter bookkeeping, same fallback shape, same RAII
    // lifetime.
    std::optional<CallerDestinationMetalBufferWrap> dst_metal_wrap;
    if (use_zero_copy && develop.caller_destination_is_page_aligned) {
        // R3 gate-13/14 root cause fix: newBufferWithBytesNoCopy requires the
        // LENGTH argument itself to be a page multiple (§4.3), not just the
        // pointer. rgba_bytes (oriented_w*oriented_h*4, the exact image byte
        // count) is essentially never a multiple of kRawDeviceArenaAlignmentBytes,
        // so passing it made this class's own internal length check reject
        // the wrap on every decode, silently degrading regardless of how well
        // aligned the caller's real buffer was -- no refusal ever reached
        // Stage4 because the wrap was never attempted successfully here.
        // out.caller_dst_capacity is what the alignment probe upstream
        // (ceyxDecodeIntoPrepare) actually checked for page-multiple-ness
        // before setting caller_destination_is_page_aligned, and
        // makeRgbaCheckout's kRawErrDstTooSmall guard already proved
        // caller_dst_capacity >= rgba_bytes earlier in this branch, so the
        // MTLBuffer covers the full extent Stage4 will write.
        dst_metal_wrap.emplace(rgba->get(), out.caller_dst_capacity);
    } else if (use_zero_copy) {
        ceyx::zero_copy_note_destination_alignment_degraded();
    }
    void* const caller_destination_metal_buffer =
        dst_metal_wrap ? dst_metal_wrap->get() : nullptr;
    if (use_zero_copy && caller_destination_metal_buffer == nullptr &&
        dst_metal_wrap.has_value()) {
        ceyx::zero_copy_note_destination_alignment_degraded();
    }

    // Same shared Stage4 call as the Bayer branch: no second render path.
    if (!runRenderStage4HalideAotFromDevice(stage3.raw_buffer(),
                                            1.0f / 65535.0f,
                                            crop.x, crop.y,
                                            static_cast<int>(src_w),
                                            static_cast<int>(src_h),
                                            static_cast<int>(out_w),
                                            static_cast<int>(out_h),
                                            params, rgba->get(),
                                            /*ctx=*/nullptr,
                                            develop.exif_orientation,
                                            arena,
                                            caller_destination_metal_buffer)) {
        return kRawErrKernelFailed;
    }

    out.diag.gpu_process_ms = nowMs() - gpu_t0;
    // C4 (plan §6.2/§6.4/§6.7): see runBayerBranch's identical comment.
    out.timing.host_to_device_copy_ms = host_to_device_copy_ms;
    // R4-T4 S3 (round-4 review B1): named separately, never folded into
    // host_to_device_copy_ms (see the h2d bracket comment above).
    out.timing.source_mosaic_copy_milliseconds = source_memcpy_ms;
    out.timing.device_to_host_copy_ms =
        runRenderStage4LastDeviceToHostCopyMilliseconds();
    out.timing.host_copy_ms =
        out.timing.host_to_device_copy_ms + out.timing.device_to_host_copy_ms;
    out.timing.gpu_submit_wait_ms =
        out.diag.gpu_process_ms - out.timing.host_copy_ms;
    // C2 (round-close audit): read on the same thread, immediately after the
    // Stage4 call above. Reports "this decode wrote the caller's pages
    // directly" -- false covers both "no buffer was passed" and "the wrap was
    // attempted but refused", not a gate-state signal.
    out.timing.unified_memory_path_active =
        runRenderStage4LastCallerDestinationWrapWasUsed() ? 1u : 0u;
    out.width = oriented_w;
    out.height = oriented_h;
    out.rgba_size = rgba_bytes;
    out.rgba_ptr = rgba->release();   // ownership moves to the caller
    return kRawSuccess;
}

// The third sibling of runBayerBranch/runXTransBranch: same borrowed stride-aware
// wrap, same interleaved RGB16 device-dirty intermediate, the SAME shared Stage4
// entry, the same RGBA pool checkout and the same ownership ordering.
//
// Three differences, all forced by the input already being full-colour: the
// source wrap is 3-D (dim 0 stride 3, dim 2 stride 1) rather than a 2-D mosaic;
// the kernel is the normalize-only pre-pass; and the black term is the
// per-component vector. There is NO demosaic here -- an X3F pixel already
// carries all three components (spec section 4.2).
//
// [F-R5-03] Explicit dispatch decision: raw_frontend_pixels_live_in_color3_image
// (libraw_frontend.cpp) accepts ANY LibRaw decode with filters==0 && colors==3,
// not only Foveon bodies -- a hypothetical RawSpeed3 cpp==3 output (raw_alloc
// == nullptr, e.g. a fully demosaiced/linear buffer some vendor decoder hands
// back) would satisfy the same predicate and arrive here too. This branch
// ACCEPTS that case rather than rejecting it, because nothing below is
// Foveon-specific: the black term is
// raw_component_black_from_libraw(v.black_scalar, v.black_channel, ...)
// (libraw_gpu_input_adapter.cpp:356-358), i.e. whatever per-channel black
// LibRaw reports for THAT file's decoder, not a Foveon constant; the white
// level is the file's own v.white_level; and the colour matrix is the file's
// own cam_xyz. A three-component interleaved U16 buffer with correct
// per-component black/white/matrix metadata is colorimetrically identical
// whether LibRaw's decoder happened to be x3f_load_raw or some other
// colors==3 path -- there is no Foveon-only default anywhere in this branch
// for a non-Foveon file to wrongly inherit. Phase 17 refused color3 dispatch
// entirely (no branch existed); this phase's decision is to accept it
// generically, keyed on the LAYOUT the validator already blessed (the linear
// RGB layout class below), never on decoder_backend/make/model.
RawErrorCode runLinearRgbBranch(const RawGpuInput& input,
                                const RawDevelopParams& develop,
                                RawPipelineResult& out) {
    const RawPlaneView& plane = input.planes[0];
    const uint32_t w = plane.width;
    const uint32_t h = plane.height;
    if (w == 0 || h == 0 || !plane.data) return kRawErrMetadataInvalid;

    // [R6 parking, r6_review.md] The borrowed wrap below divides
    // row_stride_bytes by 2 (U16 elements). An odd stride would silently
    // truncate that division; guard it explicitly rather than inherit the
    // sibling branches' unguarded pattern silently.
    if ((plane.row_stride_bytes % 2) != 0) return kRawErrMetadataInvalid;

    const RawRect& crop = input.default_crop;
    if (crop.width == 0 || crop.height == 0 ||
        crop.x < 0 || crop.y < 0 ||
        static_cast<uint32_t>(crop.x) + crop.width > w ||
        static_cast<uint32_t>(crop.y) + crop.height > h) {
        return kRawErrMetadataInvalid;
    }

    // C2 (plan §4.1.4): see runBayerBranch's identical comment.
    const bool use_zero_copy = ceyx::zero_copy_path_is_enabled();
    ceyx::RawPersistentDeviceArena* arena = ceyx::raw_persistent_device_arena_for_current_lane();

    // C2 (plan §4.2.1): see runBayerBranch's identical comment. row_stride_bytes
    // already accounts for the three interleaved components, so the required
    // byte count is the same row_stride_bytes*h shape as the mosaic branches.
    const size_t src_required_bytes = static_cast<size_t>(plane.row_stride_bytes) * h;
    void* const zero_copy_src_host =
        acquireZeroCopySourceHost(arena, use_zero_copy, src_required_bytes);
    const uint16_t* src_host_ptr = static_cast<const uint16_t*>(plane.data);
    // R4-T4 S3/S5: see runBayerBranch's identical comment. Last row copied
    // short at w*3 components (3 interleaved channels per pixel here, unlike
    // the mosaic branches' single component per pixel).
    double source_memcpy_ms = 0.0;
    if (zero_copy_src_host != nullptr) {
        const double memcpy_t0 = nowMs();
        const size_t row_bytes = static_cast<size_t>(plane.row_stride_bytes);
        const size_t last_row_bytes =
            static_cast<size_t>(w) * 3 * sizeof(uint16_t);
        if (h > 1) {
            std::memcpy(zero_copy_src_host, plane.data, row_bytes * (h - 1));
        }
        std::memcpy(static_cast<uint8_t*>(zero_copy_src_host) + row_bytes * (h - 1),
                    static_cast<const uint8_t*>(plane.data) + row_bytes * (h - 1),
                    last_row_bytes);
        source_memcpy_ms = nowMs() - memcpy_t0;
        src_host_ptr = static_cast<const uint16_t*>(zero_copy_src_host);
    }

    // Borrowed, stride-aware 3-D wrap: no host copy. row_stride_bytes comes
    // from the decoder's pitch and already accounts for the three components.
    halide_dimension_t src_dims[3] = {
        {0, static_cast<int32_t>(w), 3, 0},
        {0, static_cast<int32_t>(h),
         static_cast<int32_t>(plane.row_stride_bytes / 2), 0},
        {0, 3, 1, 0}};
    Halide::Runtime::Buffer<const uint16_t> src_buf(src_host_ptr, 3, src_dims);

    // Per-COMPONENT black, three entries. The kernel indexes black(c) with
    // c in {0,1,2}, matching the dst channel order. component_black[c] >=
    // white_level[c] cannot reach this point: raw_validate_gpu_input rejects
    // it before dispatch (raw_contract_validate.cpp:356-359) and the reason
    // string is already surfaced by raw_pipeline_decode_to_rgba's
    // "[RawPipeline] contract FAIL" fprintf below the validator call.
    float black3[3] = {input.component_black[0], input.component_black[1],
                       input.component_black[2]};
    Halide::Runtime::Buffer<const float> black_buf(black3, 3);

    Halide::Runtime::Buffer<uint16_t> stage3 =
        Halide::Runtime::Buffer<uint16_t>::make_interleaved(
            static_cast<int>(w), static_cast<int>(h), 3);

    // C1 (plan §3.2, §2.6): same lane-arena bind as runBayerBranch — see that
    // branch's comment for the full rationale. Structurally identical sibling
    // per §3.5's change list. arena acquired above, shared with the C2 gate.
    ceyx::RawDeviceArenaRegionBinding src_arena_binding(
        arena, src_buf.raw_buffer(), ceyx::RawDeviceArenaRegion::kSourceMosaicRegion,
        src_required_bytes);
    ceyx::RawDeviceArenaRegionBinding stage3_arena_binding(
        arena, stage3.raw_buffer(),
        ceyx::RawDeviceArenaRegion::kStageThreeInterleavedRgb16Region,
        static_cast<size_t>(w) * h * 3 * sizeof(uint16_t));

    // C2 (plan §4.2.1 item 2): see runBayerBranch's identical comment.
    if (zero_copy_src_host != nullptr) {
        src_buf.set_host_dirty(false);
        src_buf.set_device_dirty(false);
    } else {
        src_buf.set_host_dirty();
    }
    black_buf.set_host_dirty();
    stage3.set_host_dirty(false);

    const double gpu_t0 = nowMs();
    // C4 (plan §6.2 item 1): explicit host->device upload, timed. Same
    // re-attribution as runBayerBranch -- the kernel finds src_buf clean.
    const double h2d_t0 = nowMs();
    const int h2d_rc = src_buf.copy_to_device(dng_halide_gpu_device_interface());
    // R4-T4 S3 (round-4 review B1): see runBayerBranch's identical comment.
    const double host_to_device_copy_ms = nowMs() - h2d_t0;
    // R1 should-fix #6: same convention as runBayerBranch -- fail via the
    // existing kernel-failure path on a non-zero RC, so host_to_device_copy_ms
    // is never reported for a failed copy.
    if (h2d_rc != 0) {
        return kRawErrKernelFailed;
    }
    if (raw_linear_rgb_normalize(src_buf, black_buf,
                                 computeInvRangeLinearRgb(input), stage3) != 0) {
        return kRawErrKernelFailed;
    }

    RenderParams params;
    if (!raw_build_render_params(input, develop, params)) {
        return kRawErrMetadataInvalid;
    }

    // Scaled decode: src is the full crop; dst is the (possibly) downscaled
    // output extent. On the macOS/Metal build the shared Stage4 entry runs the
    // pre-average scaled AOT when they differ, and is bit-identical to the
    // previous crop path when equal. Split (Vulkan/Android/Linux) builds never
    // reach here with a downscale — raw_pipeline_decode_to_rgba rejects it up
    // front (no scaled AOT exists there, matching the DNG path / AC-D1).
    const uint32_t src_w = crop.width;
    const uint32_t src_h = crop.height;
    uint32_t out_w = 0, out_h = 0;
    scaledOutputExtent(src_w, src_h, develop.max_output_long_edge, &out_w, &out_h);

    // Bug fix (post-Task-3 review): dst_w/dst_h passed to the low-level kernel
    // entry below stay the UNORIENTED extent (§1.3), but unlike the high-level
    // bridge wrappers (render_stage4_halide_from_device_buffer), the low-level
    // runRenderStage4HalideAotFromDevice used on this route has NO out_w/out_h
    // reference params to write the oriented extent back through -- so this
    // caller must derive and publish the oriented extent itself, or every
    // transposing orientation (5-8) reports/allocates the wrong (unswapped)
    // dimensions. Mirrors §1.3's derivation exactly.
    const bool transposes =
        ceyx_orientation_transposes_inline(develop.exif_orientation);
    const uint32_t oriented_w = transposes ? out_h : out_w;
    const uint32_t oriented_h = transposes ? out_w : out_h;
    const size_t rgba_bytes = static_cast<size_t>(oriented_w) * oriented_h * 4;

    // WP10: pool-vs-caller is decided in makeRgbaCheckout and nowhere else, so
    // all three branches stay structurally identical to one another.
    // oriented_w/oriented_h (not out_w/out_h) is what makeRgbaCheckout
    // publishes into out.width/out.height and checks the caller buffer
    // against -- the caller's buffer was sized against the (oriented) probe.
    std::optional<RgbaCheckoutGuard> rgba;
    if (const RawErrorCode grc =
            makeRgbaCheckout(out, rgba_bytes, &rgba, oriented_w, oriented_h);
        grc != kRawSuccess) {
        return grc;
    }

    // C2 (plan §4.2.2, §4.3): see runBayerBranch's identical comment -- same
    // wrap attempt (gated on develop.caller_destination_is_page_aligned),
    // same degraded-counter bookkeeping, same fallback shape, same RAII
    // lifetime.
    std::optional<CallerDestinationMetalBufferWrap> dst_metal_wrap;
    if (use_zero_copy && develop.caller_destination_is_page_aligned) {
        // R3 gate-13/14 root cause fix: newBufferWithBytesNoCopy requires the
        // LENGTH argument itself to be a page multiple (§4.3), not just the
        // pointer. rgba_bytes (oriented_w*oriented_h*4, the exact image byte
        // count) is essentially never a multiple of kRawDeviceArenaAlignmentBytes,
        // so passing it made this class's own internal length check reject
        // the wrap on every decode, silently degrading regardless of how well
        // aligned the caller's real buffer was -- no refusal ever reached
        // Stage4 because the wrap was never attempted successfully here.
        // out.caller_dst_capacity is what the alignment probe upstream
        // (ceyxDecodeIntoPrepare) actually checked for page-multiple-ness
        // before setting caller_destination_is_page_aligned, and
        // makeRgbaCheckout's kRawErrDstTooSmall guard already proved
        // caller_dst_capacity >= rgba_bytes earlier in this branch, so the
        // MTLBuffer covers the full extent Stage4 will write.
        dst_metal_wrap.emplace(rgba->get(), out.caller_dst_capacity);
    } else if (use_zero_copy) {
        ceyx::zero_copy_note_destination_alignment_degraded();
    }
    void* const caller_destination_metal_buffer =
        dst_metal_wrap ? dst_metal_wrap->get() : nullptr;
    if (use_zero_copy && caller_destination_metal_buffer == nullptr &&
        dst_metal_wrap.has_value()) {
        ceyx::zero_copy_note_destination_alignment_degraded();
    }

    // Same shared Stage4 call as the other two branches: no second render path.
    if (!runRenderStage4HalideAotFromDevice(stage3.raw_buffer(),
                                            1.0f / 65535.0f,
                                            crop.x, crop.y,
                                            static_cast<int>(src_w),
                                            static_cast<int>(src_h),
                                            static_cast<int>(out_w),
                                            static_cast<int>(out_h),
                                            params, rgba->get(),
                                            /*ctx=*/nullptr,
                                            develop.exif_orientation,
                                            arena,
                                            caller_destination_metal_buffer)) {
        return kRawErrKernelFailed;
    }

    out.diag.gpu_process_ms = nowMs() - gpu_t0;
    // C4 (plan §6.2/§6.4/§6.7): see runBayerBranch's identical comment.
    out.timing.host_to_device_copy_ms = host_to_device_copy_ms;
    // R4-T4 S3 (round-4 review B1): named separately, never folded into
    // host_to_device_copy_ms (see the h2d bracket comment above).
    out.timing.source_mosaic_copy_milliseconds = source_memcpy_ms;
    out.timing.device_to_host_copy_ms =
        runRenderStage4LastDeviceToHostCopyMilliseconds();
    out.timing.host_copy_ms =
        out.timing.host_to_device_copy_ms + out.timing.device_to_host_copy_ms;
    out.timing.gpu_submit_wait_ms =
        out.diag.gpu_process_ms - out.timing.host_copy_ms;
    // C2 (round-close audit): read on the same thread, immediately after the
    // Stage4 call above. Reports "this decode wrote the caller's pages
    // directly" -- false covers both "no buffer was passed" and "the wrap was
    // attempted but refused", not a gate-state signal.
    out.timing.unified_memory_path_active =
        runRenderStage4LastCallerDestinationWrapWasUsed() ? 1u : 0u;
    out.width = oriented_w;
    out.height = oriented_h;
    out.rgba_size = rgba_bytes;
    out.rgba_ptr = rgba->release();   // ownership moves to the caller
    return kRawSuccess;
}

}  // namespace

RawErrorCode raw_pipeline_decode_to_rgba(const RawGpuInput& input,
                                         const RawDevelopParams& develop,
                                         RawPipelineResult& out) {
    // Trust-boundary ceiling FIRST, before the validator and before anything
    // allocates (spec section 10.1, precedence per section 9): a declared
    // extent this large must never reach an allocator, whatever else is wrong
    // with the file. The crop is checked too because it drives the RGBA
    // allocation independently of the source plane.
    for (size_t i = 0; input.planes && i < input.plane_count; ++i) {
        if (!extentWithinCeiling(input.planes[i].width, input.planes[i].height,
                                 "plane")) {
            out.error = kRawErrSizeOverflow;
            return out.error;
        }
    }
    if (!extentWithinCeiling(input.default_crop.width, input.default_crop.height,
                             "crop")) {
        out.error = kRawErrSizeOverflow;
        return out.error;
    }
    if (!extentWithinCeiling(input.active_area.width, input.active_area.height,
                             "active_area")) {
        out.error = kRawErrSizeOverflow;
        return out.error;
    }

#if defined(DNG_STAGE4_SPLIT_KERNEL)
    // Scaled/sized decode has no AOT on the split (Vulkan/Android/Linux) build,
    // and the raw path has no host SDK fallback (spec section 2.6). Reject a
    // sized request that would actually downscale — matching the DNG path's
    // documented rejection (linux-vulkan-handover.md §3 / AC-D1) rather than
    // silently returning full resolution, which would violate the caller's
    // max_output_long_edge contract. A cap >= the crop long edge is satisfiable
    // at full resolution, so it is allowed through unchanged.
    if (develop.max_output_long_edge > 0) {
        const uint32_t long_edge =
            std::max(input.default_crop.width, input.default_crop.height);
        if (develop.max_output_long_edge < long_edge) {
            std::fprintf(stderr,
                         "[RawPipeline] sized decode unsupported on this build "
                         "(no scaled AOT); requested max_long_edge=%u vs crop "
                         "long edge %u\n",
                         develop.max_output_long_edge, long_edge);
            out.error = kRawErrSizedUnsupported;
            return out.error;
        }
    }
#endif

    char reason[256] = {0};
    const RawErrorCode validated = raw_validate_gpu_input(&input, reason, sizeof(reason));
    raw_contract_print("RawGpuPipeline", &input, validated, reason, stdout);
    if (validated != kRawSuccess) {
        std::fprintf(stderr, "[RawPipeline] contract FAIL (%s: %s)\n",
                     raw_error_name(validated), reason);
        out.error = validated;
        return validated;
    }

    out.diag.sample_model = input.layout.sample_model;
    out.diag.cfa_repeat_width = input.layout.cfa_repeat_width;
    out.diag.cfa_repeat_height = input.layout.cfa_repeat_height;
    out.diag.gpu_backend = currentGpuBackend();

    // No CPU render fallback exists (spec section 2.6), so an unavailable GPU
    // is an explicit error rather than a slower path.
    //
    // Placement note (deviation from the plan's step 4b, lead-approved): the
    // plan puts this immediately after the pixel ceiling, i.e. BEFORE the
    // validator. It stays here, after the validator, because moving it would
    // change error precedence for malformed input - a corrupt file decoded with
    // the test override set would report kRawErrGpuUnavailable instead of its
    // real metadata/layout error, and the malformed matrix depends on those
    // specific codes. The ceiling above is genuinely first, as the plan
    // requires, because it is the check that must precede all allocation.
    if (!raw_pipeline_gpu_available()) {
        std::fprintf(stderr, "[RawPipeline] GPU capability gate FAILED: "
                             "backend=%s\n", dng_halide_gpu_backend_name());
        out.diag.gpu_backend = kRawGpuBackendNone;
        out.error = kRawErrGpuUnavailable;
        return out.error;
    }

    // Dispatch is a switch with NO fallthrough to a kernel: an unlisted class
    // can never reach the Bayer path by accident (spec section 13.1).
    const RawLayoutClass cls = raw_classify_layout(&input.layout);
    RawErrorCode rc = kRawErrLayoutUnsupported;
    switch (cls) {
        case kRawLayoutClassBayer2x2:
            rc = runBayerBranch(input, develop, out);
            break;
        case kRawLayoutClassXTrans6x6:
            rc = runXTransBranch(input, develop, out);
            break;
        case kRawLayoutClassLinearRgb:
            rc = runLinearRgbBranch(input, develop, out);
            break;
        default:
            std::fprintf(stderr, "[RawPipeline] layout class '%s' unsupported\n",
                         raw_layout_class_name(cls));
            rc = kRawErrLayoutUnsupported;
            break;
    }

    out.error = rc;
    return rc;
}

// WP3: caller-buffer sibling of raw_pipeline_decode_to_rgba. The binding
// happens HERE, inside the pipeline, not at the call site: A3.2 rejects
// caller-pre-set result fields, and this entry (unlike decodeFileImpl) does not
// reset `out`, so a test pre-setting the fields would work by accident and
// teach the wrong pattern.
RawErrorCode raw_pipeline_decode_to_rgba_into(const RawGpuInput& input,
                                              const RawDevelopParams& develop,
                                              uint8_t* dst, size_t dst_capacity,
                                              RawPipelineResult& out) {
    out.caller_dst = dst;
    out.caller_dst_capacity = dst_capacity;
    return raw_pipeline_decode_to_rgba(input, develop, out);
}

namespace {

RawErrorCode decodeFileImpl(const char* file_path,
                            const RawDevelopParams& develop,
                            RawForcedBackend forced,
                            const RawCancelToken& cancel,
                            uint8_t* dst, size_t dst_capacity,
                            RawPipelineResult& out) {
    out = RawPipelineResult{};          // unchanged reset, still first
    // WP10 (AMENDMENT 3 / A3.2): the caller's buffer arrives as a PARAMETER and
    // is bound to the result AFTER the reset above. It is deliberately not
    // communicated through pre-set struct fields: this function and its DNG
    // counterpart both reset their result at entry, so a pre-set field is wiped
    // by construction on both pipelines. Binding post-reset is immune to this
    // reset and to any future reset either function grows.
    out.caller_dst = dst;
    out.caller_dst_capacity = dst_capacity;
    const double t0 = nowMs();

    if (!file_path || file_path[0] == '\0') {
        out.error = kRawErrNullPath;
        return out.error;
    }

    RawRoute route = kRawRouteUnknown;
    const RawErrorCode probe_rc = raw_probe_file(file_path, &route);
    if (probe_rc != kRawSuccess) {
        out.error = probe_rc;
        return out.error;
    }
    if (route == kRawRouteUnknown) {
        out.error = kRawErrProbeFailed;
        return out.error;
    }

    if (route == kRawRouteDng) {
        // WP10 (A3 Step 15.3): with a caller buffer, forward to the
        // format-agnostic entry, which takes the DNG arm and honours the
        // caller's buffer. Without one, the DNG route is untouched and takes
        // exactly the call it always took.
        //
        // In production this branch is not reached with a caller buffer at all:
        // ceyx_decode_into_buffer routes DNG paths to the DNG arm BEFORE
        // entering this function (A3.1 fact 3 — the delegation edge is designed
        // out, which is what AC15.7 asserts). It is implemented properly rather
        // than refused so that a direct native caller of
        // raw_pipeline_decode_file_into with a DNG path gets a correct decode
        // into its own buffer instead of an error or, worse, correct pixels
        // under pool ownership.
        //
        // The ownership-move block below is unchanged and is already right for
        // this case: it clears dng->rgba_data before dng_free_result, which is
        // exactly what a caller-owned buffer needs.
        // WP5: this was a ternary whose else-arm called the allocating sized
        // DNG entry when out.caller_dst was null. After WP3 collapsed the
        // checkout guard to borrow-only, caller_dst is never null on any
        // surviving route, so the else-arm was already dead; WP5 deletes the
        // entry it called. Collapsed to the arm that was always taken -- the
        // correct code was already here.
        DngResult* dng = ceyx_decode_into_buffer(
            file_path, static_cast<int32_t>(develop.max_output_long_edge),
            out.caller_dst, out.caller_dst_capacity);
        if (!dng) {
            out.error = kRawErrAllocationFailed;
            return out.error;
        }
        out.diag.frontend = kRawFrontendDngSdk;
        out.diag.unpack_backend = kRawDecoderBackendDngSdk;
        out.diag.gpu_backend = currentGpuBackend();
        out.diag.raw_unpack_ms = dng->decode_ms;
        out.diag.gpu_process_ms = dng->process_ms;
        out.width = static_cast<uint32_t>(dng->width);
        out.height = static_cast<uint32_t>(dng->height);
        out.rgba_ptr = dng->rgba_data;
        out.rgba_size = static_cast<size_t>(dng->width) * dng->height * 4;
        out.error = dng->error_code == 0 ? kRawSuccess
                                         : static_cast<RawErrorCode>(dng->error_code);
        dng->rgba_data = nullptr;   // ownership moved; avoid the double free
        dng_free_result(dng);
        out.diag.total_ms = nowMs() - t0;
        return out.error;
    }

    // Poll 1 of 4 (spec section 10.4): after the probe, before the decoder is
    // opened. Nothing has been allocated yet, so this is a free abort. The DNG
    // route above returns before this point on purpose - its cancellation and
    // teardown behaviour is unchanged by this round.
    if (cancelRequested(cancel)) {
        out.error = kRawErrCancelled;
        out.diag.total_ms = nowMs() - t0;
        return out.error;
    }

    // The context is a local of THIS function on purpose: its scope extends
    // past the GPU wait below, which is the ownership guarantee of spec 5.1.5.
    LibRawFrontendContext ctx;
    ctx.set_forced_backend(forced);
    // Polls 2 and 3 happen inside open_and_unpack (between open_file and
    // unpack, and after unpack); a cancellation there surfaces as
    // kRawErrCancelled with the processor already recycled.
    ctx.set_cancel_hook(cancel.callback, cancel.user_data);
    const RawErrorCode unpack_rc = ctx.open_and_unpack(file_path);
    out.diag = ctx.diagnostics();
    out.diag.gpu_backend = currentGpuBackend();
    if (unpack_rc != kRawSuccess) {
        out.error = unpack_rc;
        out.diag.total_ms = nowMs() - t0;
        return out.error;
    }

    LibRawGpuInputAdapter adapter;
    RawGpuInput input{};
    RawDevelopParams effective = develop;
    char reason[256] = {0};
    const RawErrorCode build_rc =
        adapter.build(ctx, &input, &effective, reason, sizeof(reason));
    // Round 2 Task 2.6: read back what build() computed but could not return
    // directly (see raw_adapter_last_color_diagnostics's declaration). Must
    // happen before anything else on this thread calls build() again -- there
    // is nothing between here and the next build() call on this call path.
    out.color_diag = raw_adapter_last_color_diagnostics();
    // C4 (plan §6.2 item 2 / §6.3): the estimator runs entirely inside
    // adapter.build(), OUTSIDE both raw_unpack_ms (already captured above at
    // out.diag = ctx.diagnostics()) and gpu_process_ms (gpu_t0 has not opened
    // yet -- that happens inside raw_pipeline_decode_to_rgba below). This is
    // therefore unattributed time being NAMED, not a subdivision of either
    // existing window (plan §1.6/§6.3, ruled authoritative R-2026-09-11-1).
    out.timing.auto_exposure_ms = out.color_diag.auto_exposure_estimator_ms;
    // The adapter owns the metadata half of RawDevelopParams; the develop knobs
    // stay the caller's. adapter.build() resets `effective` to
    // RawDevelopParams{} internally (libraw_gpu_input_adapter.cpp:333-335), so
    // EVERY caller-input field must be restored here explicitly or it silently
    // reverts to the struct's default -- this bug already ate exif_orientation
    // once (Task 3 fix-cycle 2): the branches always saw the default (1,
    // identity) regardless of what the caller requested, because this list was
    // written before the field existed and nothing re-checks it against the
    // struct's current field set. Any FUTURE RawDevelopParams field will
    // vanish the same way unless it is added here too -- flagged as a
    // parking-lot risk, not fixed structurally in this task.
    // R4-T4 S2: static_assert pinned immediately next to the restoration
    // list -- adding a field? update the list below AND this size, or the
    // build fails until you do. This bug has now bitten TWICE (exif_
    // orientation historically, caller_destination_is_page_aligned this
    // round) with no structural guard between them; this assert is that
    // guard.
    static_assert(sizeof(RawDevelopParams) == 36,
                  "RawDevelopParams changed size: a field was added or "
                  "removed. Update the caller-knob restoration list at "
                  "raw_gpu_pipeline.cpp (this block) to include the new "
                  "field, then update this literal to the new sizeof.");
    effective.max_output_long_edge = develop.max_output_long_edge;
    effective.exposure_ev = develop.exposure_ev;
    effective.tone_curve_strength = develop.tone_curve_strength;
    effective.output_space = develop.output_space;
    effective.exif_orientation = develop.exif_orientation;
    effective.shadows = develop.shadows;
    // R3-T2 gate-17 root cause (real bug #2, this list's own documented
    // failure mode): adapter.build() resets `effective` to
    // RawDevelopParams{} internally, so the §4.3 alignment probe the caller
    // passed in was silently dropping to its false default before
    // runBayerBranch/runXTransBranch/runLinearRgbBranch ever saw it -- every
    // decode through this entry point took the degraded destination path
    // regardless of how aligned the caller's buffer actually was, exactly
    // matching test_zero_copy_capability_paths's gate-13/14/17 symptom
    // (destination_wrap_count delta 0, alignment_degradation delta nonzero).
    effective.caller_destination_is_page_aligned =
        develop.caller_destination_is_page_aligned;
    if (build_rc != kRawSuccess) {
        std::fprintf(stderr, "[RawPipeline] contract FAIL (%s: %s)\n",
                     raw_error_name(build_rc), reason);
        out.error = build_rc;
        out.diag.total_ms = nowMs() - t0;
        return out.error;
    }

    // Poll 4 of 4: the last point at which cancellation is honoured. Once
    // raw_pipeline_decode_to_rgba is entered, cancellation is deliberately NOT
    // observed: the shared Stage4 call blocks until the GPU command completes,
    // and returning earlier would let ctx destruct - freeing the borrowed
    // pixels a Metal command is still reading (spec section 5.2.5). Cancelling
    // mid-dispatch would trade a slow decode for a use-after-free.
    if (cancelRequested(cancel)) {
        out.error = kRawErrCancelled;
        out.diag.total_ms = nowMs() - t0;
        return out.error;
    }

    const RawDecodeDiagnostics unpack_diag = out.diag;
    const RawErrorCode rc = raw_pipeline_decode_to_rgba(input, effective, out);
    out.diag.frontend = unpack_diag.frontend;
    out.diag.unpack_backend = unpack_diag.unpack_backend;
    out.diag.rawspeed_flags = unpack_diag.rawspeed_flags;
    out.diag.rawspeed_warning_bits = unpack_diag.rawspeed_warning_bits;
    out.diag.raw_unpack_ms = unpack_diag.raw_unpack_ms;
    out.diag.raw_repack_bytes = unpack_diag.raw_repack_bytes;
    out.diag.total_ms = nowMs() - t0;

    // ctx destructs here, AFTER Stage4 completed and copied to host. Moving
    // this earlier would free pixels a GPU command may still be reading.
    return rc;
}

}  // namespace

// WP10: metadata-only output-extent probe. Contract on the declaration in
// raw_gpu_pipeline.h. Deliberately reuses the decode's OWN sizing function
// (scaledOutputExtent, the same call the three GPU branches make at :209/:341/
// :467) rather than re-deriving the rule, so probe/decode drift is structural
// rather than test-dependent. No new sizing arithmetic is written here.
RawErrorCode raw_pipeline_probe_output_size(const char* file_path,
                                            uint32_t max_long_edge,
                                            uint32_t* out_width,
                                            uint32_t* out_height) {
    if (out_width) *out_width = 0;
    if (out_height) *out_height = 0;
    if (!out_width || !out_height) return kRawErrNullPath;

    LibRawFrontendContext ctx;
    const RawErrorCode rc = ctx.open_metadata_only(file_path);
    if (rc != kRawSuccess) return rc;

    const LibRawFrontendContext::Extent extent = ctx.visible_extent();
    if (extent.visible_width == 0 || extent.visible_height == 0) {
        return kRawErrMetadataInvalid;
    }
    // The caller-buffer path must be subject to the SAME trust-boundary ceiling
    // as the pool path: a caller-supplied capacity is not a licence to skip it.
    if (!extentWithinCeiling(extent.visible_width, extent.visible_height,
                             "probe")) {
        return kRawErrSizeOverflow;
    }
    scaledOutputExtent(extent.visible_width, extent.visible_height,
                       max_long_edge, out_width, out_height);
    return kRawSuccess;
}

// WP5: the null-destination entries (raw_pipeline_decode_file, _forced and
// _cancellable) are DELETED. They passed dst=nullptr, which since WP3's
// borrow-only collapse is refused by makeRgbaCheckout, so they could not
// succeed on any RAW route. The caller-buffer siblings below are the only
// form that remains.
RawErrorCode raw_pipeline_decode_file_into(const char* file_path,
                                           const RawDevelopParams& develop,
                                           uint8_t* dst, size_t dst_capacity,
                                           RawPipelineResult& out) {
    const RawCancelToken none;
    return decodeFileImpl(file_path, develop, RawForcedBackend::kAuto, none,
                          dst, dst_capacity, out);
}

// WP3: caller-buffer sibling. Contract on the declaration.
RawErrorCode raw_pipeline_decode_file_forced_into(const char* file_path,
                                                  const RawDevelopParams& develop,
                                                  RawForcedBackend forced,
                                                  uint8_t* dst, size_t dst_capacity,
                                                  RawPipelineResult& out) {
    const RawCancelToken none;
    return decodeFileImpl(file_path, develop, forced, none, dst, dst_capacity, out);
}

int raw_pipeline_gpu_available() {
    // Test override first: the GPU-mandatory contract (spec section 2.6) is
    // unreachable on working hardware otherwise, and an untested error branch
    // is an untested error branch. Read once per decode, through the same
    // getenv discipline PipelineConfig already uses, and consulted nowhere
    // else.
    const char* forced = std::getenv("DNG_RAW_FORCE_GPU_UNAVAILABLE");
    if (forced && forced[0] == '1') return 0;
    // One probe, not a second opinion: this is the same capability gate the DNG
    // route's requireGpuBackend already calls.
    return dng_halide_gpu_available() ? 1 : 0;
}

// WP3: caller-buffer sibling. Contract on the declaration.
RawErrorCode raw_pipeline_decode_file_cancellable_into(const char* file_path,
                                                       const RawDevelopParams& develop,
                                                       const RawCancelToken& cancel,
                                                       uint8_t* dst, size_t dst_capacity,
                                                       RawPipelineResult& out) {
    return decodeFileImpl(file_path, develop, RawForcedBackend::kAuto, cancel,
                          dst, dst_capacity, out);
}
