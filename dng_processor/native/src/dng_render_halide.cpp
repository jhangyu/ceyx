/*
---
file_summary: >
  Stage4 render 橋接層。負責把 DNG SDK 的 Stage3 `dng_image` 與 color/tone/profile
  參數整理成 Halide AOT kernel 可用的 Buffer，並在 full Halide / SDK fallback
  兩條路徑間 dispatch。

notes:
  - `render_stage4_halide()` 是唯一正式入口；其餘多為資料萃取、參數建構與 full AOT 包裝。
  - Phase 8.3 後只保留正式 full Stage4 kernel 與 SDK fallback/quality-lock。
  - Round 1 P1 (commit 1d1726e) sweep 移除了 `renderHalideTimingEnabled` /
    `renderHalideTryFullEnabled` / `renderHalideForceFullKernelEnabled` /
    `renderHalideBitExactModeEnabled` 四個 env getter 與對應的 `DNG_RENDER_*`
    env，全部退役；本檔不再直接呼叫 `std::getenv`。env 分類請見
    `include/dng_pipeline_config.h` 頂部的 RouteConfig/DiagnosticConfig/
    ResearchConfig 目錄。
  - Phase 10 Sprint E 加入 RGB pool 版本的 `render_stage4_halide` 多載
    （ptr-based），供 mmap pool zero-copy 路徑直接寫入 caller-owned 記憶體。

structs:
  - name: "RenderParams"
    description: "Stage4 所需矩陣、1D/3D table、encoding table 與 SDK reference 物件快取。"
    lines: "125-163"
  - name: "CachedRenderHost"
    description: "quality-lock SDK Render 專用的 reusable host cache。"
    lines: "175-193"

functions:
  - name: "copyRenderSettings"
    description: "把既有 `dng_render` 參數複製到另一個 renderer（供 host 分離時重建）。"
    lines: "165-173"
  - name: "qualityLockRenderHostCache"
    description: "回傳 quality-lock SDK Render 專用的快取 host。"
    lines: "195-198"
  - name: "extractStage3Interleaved"
    description: "把 Stage3 `dng_image` 抽成 float interleaved buffer。"
    lines: "200-222"
  - name: "extractStage3Interleaved16"
    description: "把 Stage3 `dng_image` 抽成 uint16 interleaved buffer。"
    lines: "224-246"
  - name: "borrowStage3Interleaved16"
    description: "嘗試直接借用 Stage3 tile buffer，避免額外拷貝。"
    lines: "248-287"
  - name: "packBorrowedStage3Interleaved16"
    description: "將 borrowed Stage3 tile 重新整理成緊密 interleaved 版面。"
    lines: "289-322"
  - name: "borrowImageInterleaved8"
    description: "嘗試直接借用最終 8-bit image tile buffer。"
    lines: "324-362"
  - name: "packBorrowedInterleaved8"
    description: "將 borrowed 8-bit tile 重新整理成緊密 interleaved 輸出。"
    lines: "364-397"
  - name: "matrixToRowMajor3x3"
    description: "將 `dng_matrix` 轉成 Halide 端使用的 row-major 3x3 array。"
    lines: "399-405"
  - name: "toIdentityHueSat"
    description: "建立 identity HueSat / Look table。"
    lines: "407-413"
  - name: "toIdentityCurve"
    description: "建立 identity 1D curve。"
    lines: "415-422"
  - name: "copyHueSatMap"
    description: "將 SDK HueSatMap 轉成 Halide 期望的 planar layout。"
    lines: "424-450"
  - name: "buildRenderParams"
    description: "從 `dng_negative`/`dng_render` 與 centralized config 萃取 Stage4 所需矩陣、tone/gamma/table 與 profile map。"
    lines: "452-589"
  - name: "runRenderStage4HalideAot"
    description: "呼叫 full Stage4 Halide AOT kernel（host-side src buffer 路徑）。P15 W2：Android 改 zero-copy wrap SDK interleaved RGB 成 flat-1D src 直餵 kernel（src_rgb gather + src_row_stride_px scalar），刪除 host repack_src。"
    lines: "1286-1588"
  - name: "runRenderStage4HalideAotFromDevice"
    description: "Phase 8.2.2/8.2.3 — Stage4 AOT kernel，src 來自 GPU device buffer。P15 W2：Android 保留 copy_to_host + device_deallocate（W4-4 device-alias 範疇排除），但刪除 host-side crop() 與 dense-planar repack，改把 host-side 3D interleaved buffer 攤平成 flat-1D 直餵 kernel，crop 由 crop_l/crop_t scalar 吸收；其他平台保留原 device handoff。"
    lines: "1590-1888"
  - name: "runHalideFullOrSdkFallback (host src overload)"
    description: "執行正式 full Stage4 kernel（host src），失敗時回退 SDK render。"
    lines: "798-924"
  - name: "runHalideFullOrSdkFallback (device src overload)"
    description: "Stage3-on-device 路徑的 full Stage4 kernel + SDK fallback。"
    lines: "931-1063"
  - name: "renderHalideModeName"
    description: "列舉值轉字串。"
    lines: "1067-1074"
  - name: "render_stage4_halide (host vector overload)"
    description: "Stage4 正式入口；處理 Stage3 取得、kernel dispatch、fallback 與 timing。"
    lines: "1076-1099"
  - name: "render_stage4_halide_from_device_buffer (host vector overload)"
    description: "Phase 8.2.2 device handoff 入口；傳入 Stage3 device buffer，計算 DefaultCropArea 並呼叫 runRenderStage4HalideAotFromDevice。"
    lines: "1101-1158"
  - name: "render_stage4_halide (pool ptr overload)"
    description: "Phase 10 Sprint E RGB pool — 以 caller-supplied (mmap pool) 指標直接寫入；包裝 vector overload 並驗證 data() 未搬家。"
    lines: "1196-1244"
  - name: "render_stage4_halide_from_device_buffer (pool ptr overload)"
    description: "Phase 10 Sprint E RGB pool — device handoff + caller pool ptr 版本。"
    lines: "1246-1303"
  - name: "LazyZeroBuf"
    description: "T9 (Gotcha #62)：Android S4 prewarm dummy buffer 的 mmap MAP_ANON lazy-zero RAII 容器（calloc fallback），消除 ~410MB eager memset。"
    lines: "1632-1655"
  - name: "dng_render_stage4_prewarm_for_size"
    description: "W7-E Android-only：以 actual-size LazyZeroBuf identity-params dummy 預建 Stage4 render Vulkan pipeline；per-size cache、[Warmup] s4 markers。macOS no-op。"
    lines: "1659-1749"
---
*/
#include "dng_render_halide.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#if defined(__ANDROID__)
// W7-E S4 prewarm per-size cache + crash-attribution markers (Android-only).
// T9: mmap MAP_ANON lazy-zero dummy buffers (Gotcha #62).
#include <sys/mman.h>
#include <unordered_set>
#endif

#include "HalideBuffer.h"
#include "ConcurrentDngHost.h"
#include "dng_1d_function.h"
#include "dng_1d_table.h"
#include "dng_camera_profile.h"
#include "dng_color_space.h"
#include "dng_color_spec.h"
#include "dng_hue_sat_map.h"
#include "dng_matrix.h"
#include "dng_pixel_buffer.h"
#include "dng_pipeline_config.h"
#include "dng_rect.h"
#include "dng_render_stage4.h"
#if defined(__ANDROID__)
#include "dng_render_stage4_android.h"
#include <arm_neon.h>
#endif
#include "dng_resample.h"

// NOTE: Android/Vulkan Stage4 avoids multi-dimensional RGB buffers. The Android
// AOT generator declares one dense 1D plane per channel, so runtime code repacks
// the first three RGB channels before dispatch. Other targets keep the original
// interleaved RGB input/output contract for performance.

namespace {

using Halide::Runtime::Buffer;

// Q2 (Round 2 perf diag): runtime gate for this file's per-decode diagnostic
// stderr prints (and the chrono::now() calls that exist only to time them).
// Mirrors the pipelineVerbose() lazy-cache pattern in dng_pipeline_v2.cpp
// (same DNG_PIPELINE_VERBOSE env; a separate TU-local static since that
// function lives in dng_pipeline_v2.cpp's own anonymous namespace and isn't
// exported via a header).
bool pipelineVerbose() {
    static const bool cached = []() {
        const char *v = std::getenv("DNG_PIPELINE_VERBOSE");
        return v && v[0] != '0';
    }();
    return cached;
}

#if defined(__ANDROID__)
// W4-3 (b): the Android Stage4 generator now emits a single 2D planar output
// dst(i, c) — three contiguous channel planes (size = N*3) — dispatched as ONE
// Vulkan kernel (vs the former three dst_r/g/b kernels each recomputing the full
// pipeline). The host still repacks the planar output to interleaved RGB8 via
// the W4-1 multithreaded NEON repack_dst. Plan (a) (single interleaved 1D
// dst_rgb(j), j=i*3+c, no repack_dst) was attempted first but mis-lowered on
// Vulkan (~8 dB border/coverage corruption); (b) keeps the single-dispatch +
// compute-1x win without the interleaved-index codegen hazard.

// W4-1: persistent, non-zero-initialised scratch for the Stage4 repacks. The
// previous `std::vector<T>(N)` re-allocated and zero-filled large buffers on
// every decode (a wasted full-buffer memset plus thousands of fresh page
// faults). Stage4ScratchPool hands out reusable, uninitialised storage and only
// grows when a larger frame arrives. A mutex-guarded free-list keeps this
// race-free under the concurrent-decode convention used elsewhere in this file
// (ConcurrentDngHost). Every consumer writes each element before reading it, so
// skipping zero-init is safe.
template <typename T>
class Stage4ScratchPool {
public:
    // Checked-out lease: owns a buffer for the duration of one decode and
    // returns it to the pool on destruction.
    class Lease {
    public:
        Lease(Stage4ScratchPool* owner, std::unique_ptr<T[]> buf, size_t cap)
            : owner_(owner), buf_(std::move(buf)), cap_(cap) {}
        Lease(Lease&& other) noexcept
            : owner_(other.owner_), buf_(std::move(other.buf_)), cap_(other.cap_) {
            other.owner_ = nullptr;
        }
        Lease& operator=(Lease&&) = delete;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() {
            if (owner_) {
                owner_->release(std::move(buf_), cap_);
            }
        }
        T* data() const { return buf_.get(); }

    private:
        Stage4ScratchPool* owner_;
        std::unique_ptr<T[]> buf_;
        size_t cap_;
    };

    Lease acquire(size_t count) {
        std::unique_ptr<T[]> buf;
        size_t cap = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!free_.empty() && free_.back().cap >= count) {
                buf = std::move(free_.back().buf);
                cap = free_.back().cap;
                free_.pop_back();
            }
        }
        if (!buf || cap < count) {
            buf.reset(new T[count]);  // no value-init -> no memset
            cap = count;
        }
        return Lease(this, std::move(buf), cap);
    }

private:
    struct Slot {
        std::unique_ptr<T[]> buf;
        size_t cap;
    };
    void release(std::unique_ptr<T[]> buf, size_t cap) {
        std::lock_guard<std::mutex> lock(mutex_);
        free_.push_back({std::move(buf), cap});
    }
    std::mutex mutex_;
    std::vector<Slot> free_;
};

using Stage4DstScratch = Stage4ScratchPool<uint8_t>;   // planar dst -> interleaved

Stage4DstScratch& stage4DstScratch() {
    static Stage4DstScratch instance;
    return instance;
}

// Q3b (Round 2 perf diag): persistent worker pool backing the Stage4 repack
// helpers below. Previously each repackPlanarTo*MT() call spawned/joined a
// fresh std::vector<std::thread> (up to 8 pthread_create/join round trips per
// call, twice per decode). RepackThreadPool creates its (fixed, <= kMaxWorkers)
// threads once, lazily, and parks them between decodes; runRanges() hands out
// work each call via a generation counter + condition_variable instead of
// spawning new OS threads. Decodes are already single-flight (see
// pipelineSingleFlightMutex, dng_pipeline_v2.cpp), so no two runRanges() calls
// are ever in flight concurrently against this singleton.
class RepackThreadPool {
public:
    static constexpr int kMaxWorkers = 8;

    static RepackThreadPool& instance() {
        static RepackThreadPool pool;
        return pool;
    }

    // Runs `job(begin, end)` once per entry in `ranges` (ranges.size() <=
    // kMaxWorkers), blocking until every range has completed.
    void runRanges(const std::vector<std::pair<int, int>>& ranges,
                   const std::function<void(int, int)>& job) {
        std::unique_lock<std::mutex> lock(mutex_);
        ranges_ = &ranges;
        job_ = &job;
        pending_ = static_cast<int>(ranges.size());
        ++generation_;
        lock.unlock();
        cv_dispatch_.notify_all();

        lock.lock();
        cv_done_.wait(lock, [this] { return pending_ == 0; });
    }

private:
    RepackThreadPool() {
        workers_.reserve(kMaxWorkers);
        for (int i = 0; i < kMaxWorkers; ++i) {
            workers_.emplace_back([this, i] { workerLoop(i); });
        }
    }

    // Q3b fix (Android FFI FORTIFY abort): this Meyers singleton IS destroyed —
    // exit() runs the __cxa_atexit-registered destructor of the function-local
    // static. Without a shutdown path, member destruction (reverse declaration
    // order) tears down cv_dispatch_/mutex_ while all kMaxWorkers workers are
    // still parked in cv_dispatch_.wait(): bionic wakes the waiters on
    // pthread_cond destruction and each one re-locks the already-destroyed
    // mutex_ ("FORTIFY: pthread_mutex_lock called on a destroyed mutex", one
    // per worker), and ~workers_ then runs ~std::thread on joinable threads ->
    // std::terminate ("libc++abi: terminating"). Observed on-device (cc5bf709)
    // as an exit-time SIGABRT of dng_ffi_harness_android that also discards
    // the harness's buffered stdout. The destructor therefore signals stop_
    // under the mutex, wakes every worker, and joins them all BEFORE the
    // mutex/condition variables are destroyed. Decodes are single-flight and
    // exit() runs after main() returns, so no runRanges() call can race this.
    ~RepackThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_dispatch_.notify_all();
        for (auto& w : workers_) {
            w.join();
        }
    }

    void workerLoop(int idx) {
        uint64_t seen_generation = 0;
        while (true) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_dispatch_.wait(lock, [this, seen_generation] {
                return stop_ || generation_ != seen_generation;
            });
            if (stop_) {
                return;
            }
            seen_generation = generation_;
            const bool active = idx < static_cast<int>(ranges_->size());
            const std::pair<int, int> range = active ? (*ranges_)[idx] : std::pair<int, int>{0, 0};
            const std::function<void(int, int)>* job = job_;
            lock.unlock();

            if (active) {
                (*job)(range.first, range.second);
                std::lock_guard<std::mutex> done_lock(mutex_);
                if (--pending_ == 0) {
                    cv_done_.notify_one();
                }
            }
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cv_dispatch_;
    std::condition_variable cv_done_;
    uint64_t generation_ = 0;
    bool stop_ = false;
    int pending_ = 0;
    const std::vector<std::pair<int, int>>* ranges_ = nullptr;
    const std::function<void(int, int)>* job_ = nullptr;
};

// Splits [0, total_px) into up to RepackThreadPool::kMaxWorkers ranges, each
// chunk-aligned to 16 elements so NEON repack loops keep their fast path and
// write disjoint dst regions. Shared by both repackPlanarTo*MT() helpers.
std::vector<std::pair<int, int>> partitionRepackRanges(int total_px, int align) {
    unsigned hw = std::thread::hardware_concurrency();
    int chunks = static_cast<int>(hw == 0 ? 4u : std::min<unsigned>(hw, static_cast<unsigned>(RepackThreadPool::kMaxWorkers)));
    if (chunks < 1) chunks = 1;
    if (chunks > total_px) chunks = std::max(1, total_px);

    std::vector<std::pair<int, int>> ranges;
    if (chunks <= 1) {
        ranges.emplace_back(0, total_px);
        return ranges;
    }

    int base = (total_px / chunks) & ~(align - 1);
    if (base < align) base = align;
    int start = 0;
    while (start < total_px) {
        int end = std::min(total_px, start + base);
        // Last remaining region folds into the final worker.
        if (total_px - end < base) end = total_px;
        ranges.emplace_back(start, end);
        start = end;
    }
    return ranges;
}

// W4-1: row/element-partitioned multithreaded planar(u8 x3)->interleaved(u8)
// repack of the GPU output. Mirrors the NEON vst3q_u8 fast path. Bit-exact.
// W4-3 (b) keeps this: the kernel writes 3 contiguous u8 planes; this converts
// them to the caller's interleaved RGB8 (pr/pg/pb = plane0/1/2 base pointers).
void repackPlanarToInterleavedMT(const uint8_t* pr,
                                 const uint8_t* pg,
                                 const uint8_t* pb,
                                 uint8_t* dst,
                                 int total_px) {
    std::function<void(int, int)> repack_range = [pr, pg, pb, dst](int i_begin, int i_end) {
        int i = i_begin;
        for (; i + 16 <= i_end; i += 16) {
            uint8x16_t r = vld1q_u8(pr + i);
            uint8x16_t g = vld1q_u8(pg + i);
            uint8x16_t b = vld1q_u8(pb + i);
            uint8x16x3_t rgb = {r, g, b};
            vst3q_u8(dst + i * 3, rgb);
        }
        for (; i < i_end; ++i) {
            dst[i * 3 + 0] = pr[i];
            dst[i * 3 + 1] = pg[i];
            dst[i * 3 + 2] = pb[i];
        }
    };

    const std::vector<std::pair<int, int>> ranges = partitionRepackRanges(total_px, 16);
    if (ranges.size() <= 1) {
        repack_range(0, total_px);
        return;
    }
    RepackThreadPool::instance().runRanges(ranges, repack_range);
}

// W7-B: fused planar(u8 x3) -> interleaved RGBA8 (alpha=255) repack of the GPU
// output. Replaces repackPlanarToInterleavedMT + the FFI rgb_to_rgba_neon two
// passes with a single planar->RGBA pass (one fewer ~72MB read/write of the RGB
// intermediate). NEON vld1q_u8 x3 + vst4q_u8 (16 px/iter); bit-exact with the
// two-step path (same channel bytes, alpha hard 255). Used only when the caller
// supplies an RGBA8 (W*H*4) dst (Android fused path); the dst-side stays a
// host buffer, so this does NOT touch the Vulkan generator output (W4-1
// dead-end / Gotcha #93 unaffected).
void repackPlanarToRGBAMT(const uint8_t* pr,
                          const uint8_t* pg,
                          const uint8_t* pb,
                          uint8_t* dst,
                          int total_px) {
    std::function<void(int, int)> repack_range = [pr, pg, pb, dst](int i_begin, int i_end) {
        const uint8x16_t alpha = vdupq_n_u8(255);
        int i = i_begin;
        for (; i + 16 <= i_end; i += 16) {
            uint8x16x4_t rgba;
            rgba.val[0] = vld1q_u8(pr + i);
            rgba.val[1] = vld1q_u8(pg + i);
            rgba.val[2] = vld1q_u8(pb + i);
            rgba.val[3] = alpha;
            vst4q_u8(dst + i * 4, rgba);
        }
        for (; i < i_end; ++i) {
            dst[i * 4 + 0] = pr[i];
            dst[i * 4 + 1] = pg[i];
            dst[i * 4 + 2] = pb[i];
            dst[i * 4 + 3] = 255;
        }
    };

    // Align chunk boundaries to 16 so each worker keeps the vst4q_u8 fast path
    // and writes disjoint 64-byte-aligned dst regions.
    const std::vector<std::pair<int, int>> ranges = partitionRepackRanges(total_px, 16);
    if (ranges.size() <= 1) {
        repack_range(0, total_px);
        return;
    }
    RepackThreadPool::instance().runRanges(ranges, repack_range);
}
#endif

#if !defined(__ANDROID__)
// W7 (M-11): persistent scratch for the macOS RGBA→RGB strip path (legacy
// vector callers). Avoids a fresh 96MB allocation + first-touch page-fault per
// decode (same Gotcha #62 pattern). Grow-only; safe because all decodes are
// serialized by pipelineSingleFlightMutex in dng_pipeline_v2.cpp.
// ponytail: simple static, upgrade to Stage4ScratchPool pattern if concurrent decode needed.
static std::unique_ptr<uint8_t[]> s_rgba_strip_scratch;
static size_t s_rgba_strip_scratch_cap = 0;

static uint8_t* acquireRgbaStripScratch(size_t bytes) {
    if (bytes > s_rgba_strip_scratch_cap) {
        s_rgba_strip_scratch.reset(new (std::nothrow) uint8_t[bytes]);
        s_rgba_strip_scratch_cap = s_rgba_strip_scratch ? bytes : 0;
    }
    return s_rgba_strip_scratch.get();
}
#endif

// W6-2 / TD-20: shared output size computation used by all four
// render_stage4_halide overloads.  Mirrors the MaximumSize / AspectRatio
// clamp logic that was previously duplicated verbatim at four call sites.
// Pure refactor — must remain bit-exact.
dng_point computeOutputSize(const dng_negative& negative,
                            const dng_render& renderer) {
    dng_point dst_size;
    dst_size.h = static_cast<int32>(negative.DefaultFinalWidth());
    dst_size.v = static_cast<int32>(negative.DefaultFinalHeight());
    if (renderer.MaximumSize()) {
        if (Max_uint32(static_cast<uint32>(dst_size.h),
                       static_cast<uint32>(dst_size.v)) >
            renderer.MaximumSize()) {
            const real64 ratio = negative.AspectRatio();
            if (ratio >= 1.0) {
                dst_size.h = static_cast<int32>(renderer.MaximumSize());
                dst_size.v = static_cast<int32>(Max_uint32(1,
                    Round_uint32(static_cast<real64>(dst_size.h) / ratio)));
            } else {
                dst_size.v = static_cast<int32>(renderer.MaximumSize());
                dst_size.h = static_cast<int32>(Max_uint32(1,
                    Round_uint32(static_cast<real64>(dst_size.v) * ratio)));
            }
        }
    }
    return dst_size;
}

struct RenderParams {
    dng_vector camera_white_vec;
    dng_matrix camera_to_rgb_mat;
    dng_matrix rgb_to_final_mat;
    dng_1d_table exp_table_ref;
    dng_1d_table tone_table_ref;
    dng_1d_table gamma_table_ref;
    AutoPtr<dng_hue_sat_map> huesat_map_ref;
    AutoPtr<dng_hue_sat_map> look_map_ref;
    AutoPtr<dng_1d_table> huesat_encode_ref;
    AutoPtr<dng_1d_table> huesat_decode_ref;
    AutoPtr<dng_1d_table> look_encode_ref;
    AutoPtr<dng_1d_table> look_decode_ref;

    float camera_white[3] = {1.0f, 1.0f, 1.0f};
    float camera_to_rgb[9] = {};
    float rgb_to_final[9] = {};
    std::vector<float> exp_ramp;
    std::vector<float> tone_curve;
    std::vector<float> encode_gamma;

    std::vector<float> huesat_table;
    std::vector<float> huesat_encode;
    std::vector<float> huesat_decode;
    int32_t huesat_hue_div = 0;
    int32_t huesat_sat_div = 0;
    int32_t huesat_val_div = 0;
    int32_t huesat_has_table = 0;
    int32_t huesat_has_encoding = 0;

    std::vector<float> look_table;
    std::vector<float> look_encode;
    std::vector<float> look_decode;
    int32_t look_hue_div = 0;
    int32_t look_sat_div = 0;
    int32_t look_val_div = 0;
    int32_t look_has_table = 0;
    int32_t look_has_encoding = 0;
};

void copyRenderSettings(const dng_render& src, dng_render& dst) {
    dst.SetWhiteXY(src.WhiteXY());
    dst.SetExposure(src.Exposure());
    dst.SetShadows(src.Shadows());
    dst.SetToneCurve(src.ToneCurve());
    dst.SetFinalSpace(src.FinalSpace());
    dst.SetFinalPixelType(src.FinalPixelType());
    dst.SetMaximumSize(src.MaximumSize());
}

class CachedRenderHost {
public:
    dng_host* acquire(uint32_t requested_threads) {
        if (requested_threads == 0) {
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!host_ || threads_ != requested_threads) {
            host_ = std::make_unique<ConcurrentDngHost>(requested_threads);
            threads_ = requested_threads;
        }
        return host_.get();
    }

private:
    std::mutex mutex_;
    std::unique_ptr<ConcurrentDngHost> host_;
    uint32_t threads_ = 0;
};

CachedRenderHost& qualityLockRenderHostCache() {
    static CachedRenderHost cache;
    return cache;
}

void extractStage3Interleaved(dng_image* image,
                              const dng_rect& area,
                              std::vector<float>& out,
                              uint32_t& w,
                              uint32_t& h,
                              uint32_t& p) {
    w = area.W();
    h = area.H();
    p = image->Planes();
    out.resize(static_cast<size_t>(w) * h * p);

    dng_pixel_buffer buffer;
    buffer.fArea = area;
    buffer.fPlane = 0;
    buffer.fPlanes = p;
    buffer.fPixelType = ttFloat;
    buffer.fPixelSize = sizeof(float);
    buffer.fData = out.data();
    buffer.fRowStep = static_cast<int32>(w * p);
    buffer.fColStep = static_cast<int32>(p);
    buffer.fPlaneStep = 1;
    image->Get(buffer);
}

void extractStage3Interleaved16(dng_image* image,
                                const dng_rect& area,
                                std::vector<uint16_t>& out,
                                uint32_t& w,
                                uint32_t& h,
                                uint32_t& p) {
    w = area.W();
    h = area.H();
    p = image->Planes();
    out.resize(static_cast<size_t>(w) * h * p);

    dng_pixel_buffer buffer;
    buffer.fArea = area;
    buffer.fPlane = 0;
    buffer.fPlanes = p;
    buffer.fPixelType = ttShort;
    buffer.fPixelSize = sizeof(uint16_t);
    buffer.fData = out.data();
    buffer.fRowStep = static_cast<int32>(w * p);
    buffer.fColStep = static_cast<int32>(p);
    buffer.fPlaneStep = 1;
    image->Get(buffer);
}

bool borrowStage3Interleaved16(dng_image* image,
                               const dng_rect& area,
                               std::unique_ptr<dng_const_tile_buffer>& borrowedTile,
                               const uint16_t*& borrowedPtr,
                               uint32_t& w,
                               uint32_t& h,
                               uint32_t& p,
                               int32_t& rowStep,
                               int32_t& colStep,
                               int32_t& planeStep) {
    borrowedTile.reset();
    borrowedPtr = nullptr;
    rowStep = 0;
    colStep = 0;
    planeStep = 0;

    if (!image || area.IsEmpty() || image->PixelType() != ttShort) {
        return false;
    }

    w = area.W();
    h = area.H();
    p = image->Planes();
    auto tile = std::make_unique<dng_const_tile_buffer>(*image, area);

    if (tile->fPixelType != ttShort || tile->fPlanes != static_cast<int32>(p)) {
        return false;
    }

    borrowedPtr = tile->ConstPixel_uint16(area.t, area.l, 0);
    if (!borrowedPtr) {
        return false;
    }

    rowStep = tile->fRowStep;
    colStep = tile->fColStep;
    planeStep = tile->fPlaneStep;
    borrowedTile = std::move(tile);
    return true;
}

void packBorrowedStage3Interleaved16(const uint16_t* srcBase,
                                     uint32_t w,
                                     uint32_t h,
                                     uint32_t p,
                                     int32_t rowStep,
                                     int32_t colStep,
                                     int32_t planeStep,
                                     std::vector<uint16_t>& out) {
    out.resize(static_cast<size_t>(w) * h * p);
    if (!srcBase) {
        return;
    }

    const size_t rowElements = static_cast<size_t>(w) * p;
    if (colStep == static_cast<int32>(p) && planeStep == 1) {
        for (uint32_t y = 0; y < h; ++y) {
            const uint16_t* srcRow = srcBase + static_cast<int64_t>(y) * rowStep;
            uint16_t* dstRow = out.data() + static_cast<size_t>(y) * rowElements;
            std::memcpy(dstRow, srcRow, rowElements * sizeof(uint16_t));
        }
        return;
    }

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t dstBase = (static_cast<size_t>(y) * w + x) * p;
            const int64_t srcBaseIdx = static_cast<int64_t>(y) * rowStep +
                                       static_cast<int64_t>(x) * colStep;
            for (uint32_t c = 0; c < p; ++c) {
                out[dstBase + c] = srcBase[srcBaseIdx + static_cast<int64_t>(c) * planeStep];
            }
        }
    }
}

bool borrowImageInterleaved8(dng_image* image,
                             const dng_rect& area,
                             std::unique_ptr<dng_const_tile_buffer>& borrowedTile,
                             const uint8_t*& borrowedPtr,
                             uint32_t& w,
                             uint32_t& h,
                             uint32_t& p,
                             int32_t& rowStep,
                             int32_t& colStep,
                             int32_t& planeStep) {
    borrowedTile.reset();
    borrowedPtr = nullptr;
    rowStep = 0;
    colStep = 0;
    planeStep = 0;

    if (!image || area.IsEmpty() || image->PixelType() != ttByte) {
        return false;
    }

    w = area.W();
    h = area.H();
    p = image->Planes();
    auto tile = std::make_unique<dng_const_tile_buffer>(*image, area);
    if (tile->fPixelType != ttByte || tile->fPlanes != static_cast<int32>(p)) {
        return false;
    }

    borrowedPtr = tile->ConstPixel_uint8(area.t, area.l, 0);
    if (!borrowedPtr) {
        return false;
    }

    rowStep = tile->fRowStep;
    colStep = tile->fColStep;
    planeStep = tile->fPlaneStep;
    borrowedTile = std::move(tile);
    return true;
}

void packBorrowedInterleaved8(const uint8_t* srcBase,
                              uint32_t w,
                              uint32_t h,
                              uint32_t p,
                              int32_t rowStep,
                              int32_t colStep,
                              int32_t planeStep,
                              std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(w) * h * p);
    if (!srcBase) {
        return;
    }

    const size_t rowElements = static_cast<size_t>(w) * p;
    if (colStep == static_cast<int32_t>(p) && planeStep == 1) {
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* srcRow = srcBase + static_cast<int64_t>(y) * rowStep;
            uint8_t* dstRow = out.data() + static_cast<size_t>(y) * rowElements;
            std::memcpy(dstRow, srcRow, rowElements * sizeof(uint8_t));
        }
        return;
    }

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t dstBase = (static_cast<size_t>(y) * w + x) * p;
            const int64_t srcBaseIdx = static_cast<int64_t>(y) * rowStep +
                                       static_cast<int64_t>(x) * colStep;
            for (uint32_t c = 0; c < p; ++c) {
                out[dstBase + c] = srcBase[srcBaseIdx + static_cast<int64_t>(c) * planeStep];
            }
        }
    }
}

void matrixToRowMajor3x3(const dng_matrix& m, float out9[9]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out9[r * 3 + c] = static_cast<float>(m[r][c]);
        }
    }
}

void toIdentityHueSat(std::vector<float>& table) {
    // Halide Buffer(width=count, height=3) expects planar-by-component:
    // table(entry, comp) == data[entry + comp * count].
    // Use the minimum safe 2x2x2 map shape. Android Vulkan Stage4 avoids
    // runtime 2D/3D selection and always samples the 3D path.
    table = {
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    };
}

void toIdentityHueSatMap(std::vector<float>& table,
                         int32_t& hue_div,
                         int32_t& sat_div,
                         int32_t& val_div,
                         int32_t& has_table) {
    toIdentityHueSat(table);
    hue_div = 2;
    sat_div = 2;
    val_div = 2;
    has_table = 0;
}

void promoteHueSatMapTo3D(std::vector<float>& table,
                          int32_t& val_div) {
    if (val_div >= 2) {
        return;
    }

    const size_t entry_count = table.size() / 3;
    std::vector<float> promoted(entry_count * 2u * 3u);
    for (int comp = 0; comp < 3; ++comp) {
        const size_t old_base = static_cast<size_t>(comp) * entry_count;
        const size_t new_base = static_cast<size_t>(comp) * entry_count * 2u;
        for (size_t i = 0; i < entry_count; ++i) {
            const float value = table[old_base + i];
            promoted[new_base + i] = value;
            promoted[new_base + entry_count + i] = value;
        }
    }
    table.swap(promoted);
    val_div = 2;
}

bool isSafeHueSatMap(const std::vector<float>& table,
                     int32_t hue_div,
                     int32_t sat_div,
                     int32_t val_div,
                     int32_t has_table) {
    if (table.size() < 3 || (table.size() % 3) != 0) {
        return false;
    }
    if (has_table == 0) {
        return true;
    }

    if (hue_div <= 0 || sat_div <= 0 || val_div <= 0) {
        return false;
    }

    const size_t entry_count = table.size() / 3;
    const size_t min_entries =
        static_cast<size_t>(hue_div) *
        static_cast<size_t>(sat_div) *
        static_cast<size_t>(val_div);
    return entry_count >= min_entries;
}

void ensureSafeHueSatMap(std::vector<float>& table,
                         int32_t& hue_div,
                         int32_t& sat_div,
                         int32_t& val_div,
                         int32_t& has_table) {
    if (!isSafeHueSatMap(table, hue_div, sat_div, val_div, has_table)) {
        toIdentityHueSatMap(table, hue_div, sat_div, val_div, has_table);
    }
    promoteHueSatMapTo3D(table, val_div);
}

void toIdentityCurve(std::vector<float>& table) {
    table.resize(dng_1d_table::kTableSize + 2);
    for (int i = 0; i <= dng_1d_table::kTableSize; ++i) {
        table[static_cast<size_t>(i)] =
            static_cast<float>(i) / static_cast<float>(dng_1d_table::kTableSize);
    }
    table[static_cast<size_t>(dng_1d_table::kTableSize + 1)] = 1.0f;
}

void copyHueSatMap(const dng_hue_sat_map& map,
                   std::vector<float>& out,
                   int32_t& hue_div,
                   int32_t& sat_div,
                   int32_t& val_div) {
    uint32_t h = 0, s = 0, v = 0;
    map.GetDivisions(h, s, v);
    hue_div = static_cast<int32_t>(h);
    sat_div = static_cast<int32_t>(s);
    val_div = static_cast<int32_t>(v);

    const uint32_t count = map.DeltasCount();
    out.resize(static_cast<size_t>(count) * 3u);
    if (count == 0) {
        return;
    }

    const auto* deltas = map.GetConstDeltas();
    // Convert SDK interleaved HSBModify array into planar layout expected by
    // Halide Buffer(count, 3): [all hue][all sat][all val].
    const size_t n = static_cast<size_t>(count);
    for (size_t i = 0; i < n; ++i) {
        out[i] = deltas[i].fHueShift;
        out[n + i] = deltas[i].fSatScale;
        out[2 * n + i] = deltas[i].fValScale;
    }
}

bool buildRenderParams(dng_host& host,
                       dng_negative& negative,
                       const dng_render& renderer,
                       const PipelineConfig& config,
                       RenderParams& params) {
    dng_camera_profile_id profileID;
    AutoPtr<dng_color_spec> spec(negative.MakeColorSpec(profileID));
    if (!spec.Get()) {
        return false;
    }

    if (renderer.WhiteXY().IsValid()) {
        spec->SetWhiteXY(renderer.WhiteXY());
    } else if (negative.HasCameraNeutral()) {
        spec->SetWhiteXY(spec->NeutralToXY(negative.CameraNeutral()));
    } else if (negative.HasCameraWhiteXY()) {
        spec->SetWhiteXY(negative.CameraWhiteXY());
    } else {
        spec->SetWhiteXY(D55_xy_coord());
    }

    const dng_matrix camera_to_rgb =
        dng_space_ProPhoto::Get().MatrixFromPCS() * spec->CameraToPCS();
    const dng_matrix rgb_to_final =
        renderer.FinalSpace().MatrixFromPCS() * dng_space_ProPhoto::Get().MatrixToPCS();
    params.camera_to_rgb_mat = camera_to_rgb;
    params.rgb_to_final_mat = rgb_to_final;
    params.camera_white_vec = spec->CameraWhite();

    matrixToRowMajor3x3(camera_to_rgb, params.camera_to_rgb);
    matrixToRowMajor3x3(rgb_to_final, params.rgb_to_final);
    const dng_vector& cw = spec->CameraWhite();
    if (cw.Count() >= 3) {
        params.camera_white[0] = static_cast<float>(cw[0]);
        params.camera_white[1] = static_cast<float>(cw[1]);
        params.camera_white[2] = static_cast<float>(cw[2]);
    }

    const real64 exposure =
        renderer.Exposure() +
        negative.TotalBaselineExposure(profileID) -
        (std::log(negative.Stage3Gain()) / std::log(2.0));

    const real64 white = 1.0 / std::pow(2.0, std::max<real64>(0.0, exposure));
    real64 black =
        renderer.Shadows() * negative.ShadowScale() * negative.Stage3Gain() * 0.001;
    black = std::min<real64>(black, 0.99 * white);

    dng_function_exposure_ramp ramp_fn(white, black, black);
    dng_1d_table exp_table;
    exp_table.Initialize(host.Allocator(), ramp_fn);
    params.exp_table_ref.Initialize(host.Allocator(), ramp_fn);

    dng_function_exposure_tone exposure_tone(exposure);
    dng_1d_concatenate total_tone(exposure_tone, renderer.ToneCurve());
    dng_1d_table tone_table;
    tone_table.Initialize(host.Allocator(), total_tone);
    params.tone_table_ref.Initialize(host.Allocator(), total_tone);

    dng_1d_table gamma_table;
    gamma_table.Initialize(host.Allocator(), renderer.FinalSpace().GammaFunction());
    params.gamma_table_ref.Initialize(host.Allocator(), renderer.FinalSpace().GammaFunction());

    params.exp_ramp.assign(exp_table.Table(), exp_table.Table() + dng_1d_table::kTableSize + 2);
    params.tone_curve.assign(tone_table.Table(), tone_table.Table() + dng_1d_table::kTableSize + 2);
    params.encode_gamma.assign(gamma_table.Table(), gamma_table.Table() + dng_1d_table::kTableSize + 2);

    toIdentityHueSatMap(params.huesat_table,
                        params.huesat_hue_div,
                        params.huesat_sat_div,
                        params.huesat_val_div,
                        params.huesat_has_table);
    toIdentityHueSatMap(params.look_table,
                        params.look_hue_div,
                        params.look_sat_div,
                        params.look_val_div,
                        params.look_has_table);
    toIdentityCurve(params.huesat_encode);
    toIdentityCurve(params.huesat_decode);
    toIdentityCurve(params.look_encode);
    toIdentityCurve(params.look_decode);

    const dng_camera_profile* profile = negative.ProfileByID(profileID);
    if (profile) {
        AutoPtr<dng_hue_sat_map> hs_map(profile->HueSatMapForWhite(spec->WhiteXY()));
        if (hs_map.Get() && hs_map->IsValid()) {
            params.huesat_map_ref.Reset(new dng_hue_sat_map(*hs_map.Get()));
            copyHueSatMap(*hs_map.Get(),
                          params.huesat_table,
                          params.huesat_hue_div,
                          params.huesat_sat_div,
                          params.huesat_val_div);
            params.huesat_has_table = 1;
        }

        if (profile->HasLookTable() && profile->LookTable().IsValid()) {
            params.look_map_ref.Reset(new dng_hue_sat_map(profile->LookTable()));
            copyHueSatMap(profile->LookTable(),
                          params.look_table,
                          params.look_hue_div,
                          params.look_sat_div,
                          params.look_val_div);
            params.look_has_table = 1;
        }

        if (profile->HueSatMapEncoding() != encoding_Linear) {
            AutoPtr<dng_1d_table> encode_table;
            AutoPtr<dng_1d_table> decode_table;
            BuildHueSatMapEncodingTable(host.Allocator(),
                                        profile->HueSatMapEncoding(),
                                        encode_table,
                                        decode_table,
                                        false);
            if (encode_table.Get() && decode_table.Get()) {
                params.huesat_encode.assign(encode_table->Table(),
                                            encode_table->Table() + dng_1d_table::kTableSize + 2);
                params.huesat_decode.assign(decode_table->Table(),
                                            decode_table->Table() + dng_1d_table::kTableSize + 2);
                params.huesat_encode_ref.Reset(encode_table.Release());
                params.huesat_decode_ref.Reset(decode_table.Release());
                params.huesat_has_encoding = 1;
            }
        }

        if (profile->LookTableEncoding() != encoding_Linear) {
            AutoPtr<dng_1d_table> encode_table;
            AutoPtr<dng_1d_table> decode_table;
            BuildHueSatMapEncodingTable(host.Allocator(),
                                        profile->LookTableEncoding(),
                                        encode_table,
                                        decode_table,
                                        false);
            if (encode_table.Get() && decode_table.Get()) {
                params.look_encode.assign(encode_table->Table(),
                                          encode_table->Table() + dng_1d_table::kTableSize + 2);
                params.look_decode.assign(decode_table->Table(),
                                          decode_table->Table() + dng_1d_table::kTableSize + 2);
                params.look_encode_ref.Reset(encode_table.Release());
                params.look_decode_ref.Reset(decode_table.Release());
                params.look_has_encoding = 1;
            }
        }
    }

#if defined(__ANDROID__)
    ensureSafeHueSatMap(params.huesat_table,
                        params.huesat_hue_div,
                        params.huesat_sat_div,
                        params.huesat_val_div,
                        params.huesat_has_table);
    ensureSafeHueSatMap(params.look_table,
                        params.look_hue_div,
                        params.look_sat_div,
                        params.look_val_div,
                        params.look_has_table);
#endif

    return true;
}

bool runRenderStage4HalideAot(const uint16_t* src,
                              int src_w,
                              int src_h,
                              int src_p,
                              int src_row_step,
                              int src_col_step,
                              int src_plane_step,
                              float src_scale,
                              int dst_w,
                              int dst_h,
                              const RenderParams& params,
                              uint8_t* dst,
                              bool fuse_rgba = false) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || src_p < 3) {
        return false;
    }

#if defined(__ANDROID__)
    // W2: zero-copy wrap the SDK interleaved RGB buffer as one flat 1D plane and
    // let the GPU gather src_rgb(base + c) directly (Gotcha #95, probe GO). The
    // former host repack_src (interleaved->3 dense planar) is eliminated — the
    // bytes uploaded are identical, only the host CPU repack is removed.
    // Q2: these timepoints only feed the verbose-gated [Stage4-Perf] print
    // below; skip the clock reads entirely when not verbose.
    const bool verbose_timing = pipelineVerbose();
    auto t0 = verbose_timing ? std::chrono::high_resolution_clock::now()
                             : std::chrono::high_resolution_clock::time_point{};
    // src_row_step is the interleaved row stride in u16 elements (= pixels*3
    // incl. any SDK tile padding). Per-pixel row stride therefore = /3.
    const int src_row_stride_px = src_row_step / 3;
    const int flat_len = src_row_step * src_h;  // elements in the flat plane
    Buffer<uint16_t> src_rgb_buf(const_cast<uint16_t*>(src), flat_len);
    auto t1 = verbose_timing ? std::chrono::high_resolution_clock::now()
                             : std::chrono::high_resolution_clock::time_point{};  // repack_src now ~0
#else
    if (pipelineVerbose()) {
        fprintf(stderr, "[Stage4-Diag] NON-ANDROID interleaved path ACTIVE (runRenderStage4HalideAot)\n");
    }
    halide_dimension_t src_shape[3] = {
        {0, src_w, src_col_step, 0},
        {0, src_h, src_row_step, 0},
        {0, src_p, src_plane_step, 0},
    };
    Buffer<uint16_t> src_buf(const_cast<uint16_t*>(src), 3, src_shape);
#endif
    Buffer<float> exp_buf(const_cast<float*>(params.exp_ramp.data()),
                          static_cast<int>(params.exp_ramp.size()));
    Buffer<float> tone_buf(const_cast<float*>(params.tone_curve.data()),
                           static_cast<int>(params.tone_curve.size()));
    Buffer<float> gamma_buf(const_cast<float*>(params.encode_gamma.data()),
                            static_cast<int>(params.encode_gamma.size()));
    Buffer<float> cw_buf(const_cast<float*>(params.camera_white), 3);
#if defined(__ANDROID__)
    Buffer<float> c2r_buf(const_cast<float*>(params.camera_to_rgb), 9);
    Buffer<float> r2f_buf(const_cast<float*>(params.rgb_to_final), 9);
    Buffer<float> hs_table_buf(const_cast<float*>(params.huesat_table.data()),
                               static_cast<int>(params.huesat_table.size()));
    Buffer<float> look_table_buf(const_cast<float*>(params.look_table.data()),
                                 static_cast<int>(params.look_table.size()));
#else
    Buffer<float> c2r_buf(const_cast<float*>(params.camera_to_rgb), 3, 3);
    Buffer<float> r2f_buf(const_cast<float*>(params.rgb_to_final), 3, 3);
    Buffer<float> hs_table_buf(const_cast<float*>(params.huesat_table.data()),
                               static_cast<int>(params.huesat_table.size() / 3), 3);
    Buffer<float> look_table_buf(const_cast<float*>(params.look_table.data()),
                                 static_cast<int>(params.look_table.size() / 3), 3);
#endif
    Buffer<float> hs_encode_buf(const_cast<float*>(params.huesat_encode.data()),
                                static_cast<int>(params.huesat_encode.size()));
    Buffer<float> hs_decode_buf(const_cast<float*>(params.huesat_decode.data()),
                                static_cast<int>(params.huesat_decode.size()));
    Buffer<float> look_encode_buf(const_cast<float*>(params.look_encode.data()),
                                  static_cast<int>(params.look_encode.size()));
    Buffer<float> look_decode_buf(const_cast<float*>(params.look_decode.data()),
                                  static_cast<int>(params.look_decode.size()));
#if defined(__ANDROID__)
    // W4-3 (b): single 2D planar dst output (dim0=i stride1, dim1=c stride N).
    // Persistent non-zero-init scratch holds the 3 contiguous channel planes;
    // the host repacks them to the caller's interleaved RGB8 below.
    const int dst_pixel_count = dst_w * dst_h;
    Stage4DstScratch::Lease dst_lease =
        stage4DstScratch().acquire(static_cast<size_t>(dst_pixel_count) * 3);
    halide_dimension_t dst_shape[2] = {
        {0, dst_pixel_count, 1, 0},
        {0, 3, dst_pixel_count, 0},
    };
    Buffer<uint8_t> dst_planar_buf(dst_lease.data(), 2, dst_shape);
#else
    // W7 (M-11): macOS generator outputs RGBA8 (4 channels, alpha=255 in-kernel).
    // When fuse_rgba, the caller's buffer is already RGBA8 (W*H*4) — write directly.
    // When !fuse_rgba (legacy vector callers), use persistent scratch for the
    // 4-channel kernel output, then strip alpha to RGB8 in the caller's buffer.
    uint8_t* dst_rgba = dst;
    if (!fuse_rgba) {
        dst_rgba = acquireRgbaStripScratch(static_cast<size_t>(dst_w) * dst_h * 4);
        if (!dst_rgba) return false;
    }
    Buffer<uint8_t> dst_buf = Buffer<uint8_t>::make_interleaved(dst_rgba, dst_w, dst_h, 4);
#endif

#if defined(__ANDROID__)
    src_rgb_buf.set_host_dirty();
#else
    src_buf.set_host_dirty();
#endif
    exp_buf.set_host_dirty();
    tone_buf.set_host_dirty();
    gamma_buf.set_host_dirty();
    cw_buf.set_host_dirty();
    c2r_buf.set_host_dirty();
    r2f_buf.set_host_dirty();
    hs_table_buf.set_host_dirty();
    hs_encode_buf.set_host_dirty();
    hs_decode_buf.set_host_dirty();
    look_table_buf.set_host_dirty();
    look_encode_buf.set_host_dirty();
    look_decode_buf.set_host_dirty();
#if defined(__ANDROID__)
    dst_planar_buf.set_host_dirty(false);
#else
    dst_buf.set_host_dirty(false);
#endif

#if defined(__ANDROID__)
    const int32_t huesat_entry_count = static_cast<int32_t>(params.huesat_table.size() / 3);
    const int32_t look_entry_count = static_cast<int32_t>(params.look_table.size() / 3);
    const int result = dng_render_stage4_android(
        src_rgb_buf.raw_buffer(),
        src_w,
        src_h,
        dst_w,
        src_row_stride_px,
        /*crop_l=*/0,
        /*crop_t=*/0,
        src_scale,
        exp_buf.raw_buffer(),
        tone_buf.raw_buffer(),
        gamma_buf.raw_buffer(),
        cw_buf.raw_buffer(),
        c2r_buf.raw_buffer(),
        r2f_buf.raw_buffer(),
        hs_table_buf.raw_buffer(),
        hs_encode_buf.raw_buffer(),
        hs_decode_buf.raw_buffer(),
        huesat_entry_count,
        params.huesat_hue_div,
        params.huesat_sat_div,
        params.huesat_val_div,
        params.huesat_has_table,
        params.huesat_has_encoding,
        look_table_buf.raw_buffer(),
        look_encode_buf.raw_buffer(),
        look_decode_buf.raw_buffer(),
        look_entry_count,
        params.look_hue_div,
        params.look_sat_div,
        params.look_val_div,
        params.look_has_table,
        params.look_has_encoding,
        dst_planar_buf.raw_buffer());
    auto t2 = verbose_timing ? std::chrono::high_resolution_clock::now()
                             : std::chrono::high_resolution_clock::time_point{};
#else
    const int result = dng_render_stage4(src_buf.raw_buffer(),
                                         src_scale,
                                         exp_buf.raw_buffer(),
                                         tone_buf.raw_buffer(),
                                         gamma_buf.raw_buffer(),
                                         cw_buf.raw_buffer(),
                                         c2r_buf.raw_buffer(),
                                         r2f_buf.raw_buffer(),
                                         hs_table_buf.raw_buffer(),
                                         hs_encode_buf.raw_buffer(),
                                         hs_decode_buf.raw_buffer(),
                                         params.huesat_hue_div,
                                         params.huesat_sat_div,
                                         params.huesat_val_div,
                                         params.huesat_has_table,
                                         params.huesat_has_encoding,
                                         look_table_buf.raw_buffer(),
                                         look_encode_buf.raw_buffer(),
                                         look_decode_buf.raw_buffer(),
                                         params.look_hue_div,
                                         params.look_sat_div,
                                         params.look_val_div,
                                         params.look_has_table,
                                         params.look_has_encoding,
                                         dst_buf.raw_buffer());
#endif
    if (pipelineVerbose()) {
        fprintf(stderr, "[Stage4-Diag] kernel result=%d (runRenderStage4HalideAot)\n", result);
    }
    if (result != 0) {
        return false;
    }

#if defined(__ANDROID__)
    // W4-3 (b): one D2H copy of the planar output, then MT repack to interleaved.
    if (dst_planar_buf.copy_to_host() != 0) return false;
    auto t3 = verbose_timing ? std::chrono::high_resolution_clock::now()
                             : std::chrono::high_resolution_clock::time_point{};

    // The 3 channel planes are contiguous: plane0=base, plane1=base+N, plane2=base+2N.
    // W4-1: multithreaded NEON vld1q_u8 + vst3q_u8 (16 px/iter).
    // W7-B: when fuse_rgba, write interleaved RGBA8 (alpha=255) directly into the
    // caller's RGBA buffer, fusing the former repack_dst + FFI rgb_to_rgba.
    {
        const uint8_t* planar = dst_planar_buf.data();
        const int total_px = dst_w * dst_h;
        if (fuse_rgba) {
            repackPlanarToRGBAMT(planar, planar + total_px, planar + 2 * total_px,
                                 dst, total_px);
        } else {
            repackPlanarToInterleavedMT(planar, planar + total_px, planar + 2 * total_px,
                                        dst, total_px);
        }
    }
    auto t4 = verbose_timing ? std::chrono::high_resolution_clock::now()
                             : std::chrono::high_resolution_clock::time_point{};
    if (verbose_timing) {
        fprintf(stderr, "[Stage4-Perf] repack_src=%.1f ms dispatch=%.1f ms copy_host=%.1f ms repack_dst=%.1f ms total=%.1f ms\n",
            std::chrono::duration<double, std::milli>(t1 - t0).count(),
            std::chrono::duration<double, std::milli>(t2 - t1).count(),
            std::chrono::duration<double, std::milli>(t3 - t2).count(),
            std::chrono::duration<double, std::milli>(t4 - t3).count(),
            std::chrono::duration<double, std::milli>(t4 - t0).count());
    }
#else
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
    // W7 (M-11): strip alpha when legacy caller expects RGB8.
    if (!fuse_rgba) {
        const size_t total_px = static_cast<size_t>(dst_w) * dst_h;
        for (size_t i = 0; i < total_px; ++i) {
            dst[i * 3 + 0] = dst_rgba[i * 4 + 0];
            dst[i * 3 + 1] = dst_rgba[i * 4 + 1];
            dst[i * 3 + 2] = dst_rgba[i * 4 + 2];
        }
    }
#endif

    return true;
}

// Phase 8.2.2 - Stage3->Stage4 device handoff.
// Android/Vulkan uses a host-side planar repack before Stage4. Other targets
// keep the original device buffer handoff path.
bool runRenderStage4HalideAotFromDevice(halide_buffer_t* stage3_device_buf,
                                         float src_scale,
                                         int crop_l,
                                         int crop_t,
                                         int dst_w,
                                         int dst_h,
                                         const RenderParams& params,
                                         uint8_t* dst,
                                         bool fuse_rgba = false) {
    if (!stage3_device_buf || stage3_device_buf->dimensions < 3 ||
        !dst || dst_w <= 0 || dst_h <= 0) {
        return false;
    }

#if defined(__ANDROID__)
    // Q2: these timepoints only feed the verbose-gated [Stage4-Perf] print
    // below; skip the clock reads entirely when not verbose. The real work
    // (copy_to_host / device_deallocate below) always runs regardless.
    const bool verbose_timing_fd = pipelineVerbose();
    auto t0_fd = verbose_timing_fd ? std::chrono::high_resolution_clock::now()
                                   : std::chrono::high_resolution_clock::time_point{};
    // Non-owning wrapper of the 3D device buffer.
    Buffer<uint16_t> src3d(*stage3_device_buf);

    // We still copy the Stage3 result to host (device-alias = W4-4 scope, kept
    // out of W2). On Metal, the serial command queue guarantees Stage3 is done.
    if (src3d.copy_to_host() != 0) {
        return false;
    }
    src3d.device_deallocate();
    auto tcopy_fd = verbose_timing_fd ? std::chrono::high_resolution_clock::now()
                                      : std::chrono::high_resolution_clock::time_point{};  // end of D2H copy

    // W2: no host repack and no host-side crop(). The host-side 3D buffer is
    // already interleaved (x, y, c) with channel stride 1; flatten it to a 1D
    // plane and feed the kernel directly. crop_l/crop_t are absorbed by the
    // kernel's index arithmetic (src_row_stride_px = dim(1).stride()/3).
    const int sw = src3d.dim(0).extent();
    const int sh = src3d.dim(1).extent();
    const int sp = src3d.dim(2).extent();
    if (sp < 3) {
        return false;
    }
    const int s_row = src3d.dim(1).stride();  // = row_stride_px * 3
    const int src_row_stride_px = s_row / 3;
    const int flat_len = s_row * sh;          // elements in the flat plane
    uint16_t* src_data = reinterpret_cast<uint16_t*>(src3d.data());
    Buffer<uint16_t> src_rgb_buf(src_data, flat_len);
    src_rgb_buf.set_host_dirty();
    auto t1_fd = verbose_timing_fd ? std::chrono::high_resolution_clock::now()
                                   : std::chrono::high_resolution_clock::time_point{};  // repack_src now ~0
#else
    if (pipelineVerbose()) {
        fprintf(stderr, "[Stage4-Diag] NON-ANDROID interleaved path ACTIVE (runRenderStage4HalideAotFromDevice)\n");
    }
    // Non-owning wrapper — do NOT call set_host_dirty; data is on the GPU.
    Buffer<uint16_t> src_buf(*stage3_device_buf);
    // 8.2.3 fix: physically shift the read pointer via crop(), then mutate
    // dim[i].min back to 0 so src/dst share the [0..dst_w-1] coordinate system.
    if (crop_l > 0 || crop_t > 0) {
        src_buf.crop(0, crop_l, dst_w);
        src_buf.crop(1, crop_t, dst_h);
        halide_buffer_t* raw = src_buf.raw_buffer();
        raw->dim[0].min = 0;
        raw->dim[1].min = 0;
    }
#endif

    Buffer<float> exp_buf(const_cast<float*>(params.exp_ramp.data()),
                          static_cast<int>(params.exp_ramp.size()));
    Buffer<float> tone_buf(const_cast<float*>(params.tone_curve.data()),
                           static_cast<int>(params.tone_curve.size()));
    Buffer<float> gamma_buf(const_cast<float*>(params.encode_gamma.data()),
                            static_cast<int>(params.encode_gamma.size()));
    Buffer<float> cw_buf(const_cast<float*>(params.camera_white), 3);
#if defined(__ANDROID__)
    Buffer<float> c2r_buf(const_cast<float*>(params.camera_to_rgb), 9);
    Buffer<float> r2f_buf(const_cast<float*>(params.rgb_to_final), 9);
    Buffer<float> hs_table_buf(const_cast<float*>(params.huesat_table.data()),
                               static_cast<int>(params.huesat_table.size()));
    Buffer<float> look_table_buf(const_cast<float*>(params.look_table.data()),
                                 static_cast<int>(params.look_table.size()));
#else
    Buffer<float> c2r_buf(const_cast<float*>(params.camera_to_rgb), 3, 3);
    Buffer<float> r2f_buf(const_cast<float*>(params.rgb_to_final), 3, 3);
    Buffer<float> hs_table_buf(const_cast<float*>(params.huesat_table.data()),
                               static_cast<int>(params.huesat_table.size() / 3), 3);
    Buffer<float> look_table_buf(const_cast<float*>(params.look_table.data()),
                                 static_cast<int>(params.look_table.size() / 3), 3);
#endif
    Buffer<float> hs_encode_buf(const_cast<float*>(params.huesat_encode.data()),
                                static_cast<int>(params.huesat_encode.size()));
    Buffer<float> hs_decode_buf(const_cast<float*>(params.huesat_decode.data()),
                                static_cast<int>(params.huesat_decode.size()));
    Buffer<float> look_encode_buf(const_cast<float*>(params.look_encode.data()),
                                  static_cast<int>(params.look_encode.size()));
    Buffer<float> look_decode_buf(const_cast<float*>(params.look_decode.data()),
                                  static_cast<int>(params.look_decode.size()));
#if defined(__ANDROID__)
    // W4-3 (b): single 2D planar dst output (dim0=i stride1, dim1=c stride N).
    const int dst_pixel_count = dst_w * dst_h;
    Stage4DstScratch::Lease dst_lease =
        stage4DstScratch().acquire(static_cast<size_t>(dst_pixel_count) * 3);
    halide_dimension_t dst_shape[2] = {
        {0, dst_pixel_count, 1, 0},
        {0, 3, dst_pixel_count, 0},
    };
    Buffer<uint8_t> dst_planar_buf(dst_lease.data(), 2, dst_shape);
#else
    // W7 (M-11): macOS generator outputs RGBA8. Persistent scratch for strip path.
    uint8_t* dst_rgba_fd = dst;
    if (!fuse_rgba) {
        dst_rgba_fd = acquireRgbaStripScratch(static_cast<size_t>(dst_w) * dst_h * 4);
        if (!dst_rgba_fd) return false;
    }
    Buffer<uint8_t> dst_buf = Buffer<uint8_t>::make_interleaved(dst_rgba_fd, dst_w, dst_h, 4);
#endif

    exp_buf.set_host_dirty();
    tone_buf.set_host_dirty();
    gamma_buf.set_host_dirty();
    cw_buf.set_host_dirty();
    c2r_buf.set_host_dirty();
    r2f_buf.set_host_dirty();
    hs_table_buf.set_host_dirty();
    hs_encode_buf.set_host_dirty();
    hs_decode_buf.set_host_dirty();
    look_table_buf.set_host_dirty();
    look_encode_buf.set_host_dirty();
    look_decode_buf.set_host_dirty();
#if defined(__ANDROID__)
    dst_planar_buf.set_host_dirty(false);
#else
    dst_buf.set_host_dirty(false);
#endif

#if defined(__ANDROID__)
    const int32_t huesat_entry_count = static_cast<int32_t>(params.huesat_table.size() / 3);
    const int32_t look_entry_count = static_cast<int32_t>(params.look_table.size() / 3);
    const int result = dng_render_stage4_android(
        src_rgb_buf.raw_buffer(),
        sw,
        sh,
        dst_w,
        src_row_stride_px,
        crop_l,
        crop_t,
        src_scale,
        exp_buf.raw_buffer(),
        tone_buf.raw_buffer(),
        gamma_buf.raw_buffer(),
        cw_buf.raw_buffer(),
        c2r_buf.raw_buffer(),
        r2f_buf.raw_buffer(),
        hs_table_buf.raw_buffer(),
        hs_encode_buf.raw_buffer(),
        hs_decode_buf.raw_buffer(),
        huesat_entry_count,
        params.huesat_hue_div,
        params.huesat_sat_div,
        params.huesat_val_div,
        params.huesat_has_table,
        params.huesat_has_encoding,
        look_table_buf.raw_buffer(),
        look_encode_buf.raw_buffer(),
        look_decode_buf.raw_buffer(),
        look_entry_count,
        params.look_hue_div,
        params.look_sat_div,
        params.look_val_div,
        params.look_has_table,
        params.look_has_encoding,
        dst_planar_buf.raw_buffer());
    auto t2_fd = verbose_timing_fd ? std::chrono::high_resolution_clock::now()
                                   : std::chrono::high_resolution_clock::time_point{};
#else
    const int result = dng_render_stage4(src_buf.raw_buffer(),
                                         src_scale,
                                         exp_buf.raw_buffer(),
                                         tone_buf.raw_buffer(),
                                         gamma_buf.raw_buffer(),
                                         cw_buf.raw_buffer(),
                                         c2r_buf.raw_buffer(),
                                         r2f_buf.raw_buffer(),
                                         hs_table_buf.raw_buffer(),
                                         hs_encode_buf.raw_buffer(),
                                         hs_decode_buf.raw_buffer(),
                                         params.huesat_hue_div,
                                         params.huesat_sat_div,
                                         params.huesat_val_div,
                                         params.huesat_has_table,
                                         params.huesat_has_encoding,
                                         look_table_buf.raw_buffer(),
                                         look_encode_buf.raw_buffer(),
                                         look_decode_buf.raw_buffer(),
                                         params.look_hue_div,
                                         params.look_sat_div,
                                         params.look_val_div,
                                         params.look_has_table,
                                         params.look_has_encoding,
                                         dst_buf.raw_buffer());
#endif
    if (pipelineVerbose()) {
        fprintf(stderr, "[Stage4-Diag] kernel result=%d (runRenderStage4HalideAotFromDevice)\n", result);
    }
    if (result != 0) {
        return false;
    }

#if defined(__ANDROID__)
    // W4-3 (b): one D2H copy of the planar output, then MT repack to interleaved.
    if (dst_planar_buf.copy_to_host() != 0) return false;
    auto t3_fd = verbose_timing_fd ? std::chrono::high_resolution_clock::now()
                                   : std::chrono::high_resolution_clock::time_point{};

    {
        const uint8_t* planar = dst_planar_buf.data();
        const int total_px = dst_w * dst_h;
        // W7-B: fuse planar->RGBA8 directly into the caller's RGBA buffer when
        // requested (eliminates the separate FFI rgb_to_rgba pass).
        if (fuse_rgba) {
            repackPlanarToRGBAMT(planar, planar + total_px, planar + 2 * total_px,
                                 dst, total_px);
        } else {
            repackPlanarToInterleavedMT(planar, planar + total_px, planar + 2 * total_px,
                                        dst, total_px);
        }
    }
    auto t4_fd = verbose_timing_fd ? std::chrono::high_resolution_clock::now()
                                   : std::chrono::high_resolution_clock::time_point{};
    // W2: repack_src now measures only the (eliminated) host repack — the flatten
    // wrap is O(1), so it prints ~0. The kept Stage3->host D2H copy (W4-4 scope,
    // not removed in W2) is broken out separately as d2h_src so the parser's
    // repack_src= key still resolves and the total stays comparable to baseline.
    if (verbose_timing_fd) {
        fprintf(stderr, "[Stage4-Perf] FromDevice: repack_src=%.1f ms d2h_src=%.1f ms dispatch=%.1f ms copy_host=%.1f ms repack_dst=%.1f ms total=%.1f ms\n",
            std::chrono::duration<double, std::milli>(t1_fd - tcopy_fd).count(),
            std::chrono::duration<double, std::milli>(tcopy_fd - t0_fd).count(),
            std::chrono::duration<double, std::milli>(t2_fd - t1_fd).count(),
            std::chrono::duration<double, std::milli>(t3_fd - t2_fd).count(),
            std::chrono::duration<double, std::milli>(t4_fd - t3_fd).count(),
            std::chrono::duration<double, std::milli>(t4_fd - t0_fd).count());
    }
#else
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
    // W7 (M-11): strip alpha when legacy caller expects RGB8.
    if (!fuse_rgba) {
        const size_t total_px = static_cast<size_t>(dst_w) * dst_h;
        for (size_t i = 0; i < total_px; ++i) {
            dst[i * 3 + 0] = dst_rgba_fd[i * 4 + 0];
            dst[i * 3 + 1] = dst_rgba_fd[i * 4 + 1];
            dst[i * 3 + 2] = dst_rgba_fd[i * 4 + 2];
        }
    }
#endif

    return true;
}

bool runHalideFullOrSdkFallback(dng_host& host,
                                dng_negative& negative,
                                dng_image* stage3,
                                const dng_render& renderer,
                                const PipelineConfig& config,
                                std::vector<uint8_t>& out_rgb,
                                uint32_t& out_w,
                                uint32_t& out_h) {
    const dng_point dst_size = computeOutputSize(negative, renderer);
    out_w = static_cast<uint32_t>(dst_size.h);
    out_h = static_cast<uint32_t>(dst_size.v);

    // Use resize instead of assign(N, 0): the Halide kernel overwrites every byte,
    // so the zero-fill is wasted work. resize() is a no-op when out_rgb is already sized
    // by the caller (which avoids the 250ms first-touch page-fault cost on a 72MB buffer).
    // W7 (M-11): macOS generator outputs RGBA8 but the caller's vector stays RGB (W*H*3).
    // The kernel writes into the persistent RGBA scratch (via runRenderStage4HalideAot's
    // fuse_rgba=false path), which strips to RGB in the caller's vector — no 4-channel
    // resize of the caller's vector (that realloc + page-fault was costing ~150ms).
    const size_t needed_out_size = static_cast<size_t>(out_w) * out_h * 3;
    if (out_rgb.size() != needed_out_size) {
        out_rgb.resize(needed_out_size);
    }

    dng_rect src_area = negative.DefaultCropArea();

    dng_image* source_image = stage3;
    dng_rect source_area = src_area;
    AutoPtr<dng_image> resized_stage3;
    const bool need_resample = src_area.Size() != dst_size;
    if (need_resample) {
        resized_stage3.Reset(host.Make_dng_image(dst_size, stage3->Planes(), stage3->PixelType()));
        if (!resized_stage3.Get()) {
            return false;
        }
        ResampleImage(host,
                      *stage3,
                      *resized_stage3.Get(),
                      src_area,
                      resized_stage3->Bounds(),
                      dng_resample_bicubic::Get());
        source_image = resized_stage3.Get();
        source_area = resized_stage3->Bounds();
    }

    uint32_t src_w = source_area.W();
    uint32_t src_h = source_area.H();
    uint32_t src_p = source_image->Planes();
    std::vector<uint16_t> stage3_data16;
    RenderParams params;
    if (!buildRenderParams(host, negative, renderer, config, params)) {
        return false;
    }

    const bool can_use_u16_stage3 = source_image->PixelType() == ttShort &&
                                    source_image->PixelRange() != 0;
    if (can_use_u16_stage3) {
        const uint16_t* stage3_u16_ptr = nullptr;
        std::unique_ptr<dng_const_tile_buffer> stage3_borrowed_tile;
        int32_t stage3_row_step = static_cast<int32_t>(src_w * src_p);
        int32_t stage3_col_step = static_cast<int32_t>(src_p);
        int32_t stage3_plane_step = 1;
        if (borrowStage3Interleaved16(source_image,
                                      source_area,
                                      stage3_borrowed_tile,
                                      stage3_u16_ptr,
                                      src_w,
                                      src_h,
                                      src_p,
                                      stage3_row_step,
                                      stage3_col_step,
                                      stage3_plane_step)) {
            // Borrowed path keeps source strides and avoids O(WxH) repack.
        } else {
            extractStage3Interleaved16(source_image, source_area, stage3_data16, src_w, src_h, src_p);
            stage3_u16_ptr = stage3_data16.data();
        }
        const float src_scale = 1.0f / static_cast<float>(source_image->PixelRange());
        const bool render_ok = runRenderStage4HalideAot(stage3_u16_ptr,
                                                         static_cast<int>(src_w),
                                                         static_cast<int>(src_h),
                                                         static_cast<int>(src_p),
                                                         stage3_row_step,
                                                         stage3_col_step,
                                                         stage3_plane_step,
                                                         src_scale,
                                                         static_cast<int>(out_w),
                                                         static_cast<int>(out_h),
                                                         params,
                                                         out_rgb.data());
        if (render_ok) {
            // W7 (M-11): on macOS, runRenderStage4HalideAot already stripped
            // RGBA→RGB via the persistent scratch (fuse_rgba=false). The caller's
            // vector is W*H*3 throughout — no resize needed.
            return true;
        }
    }

    AutoPtr<dng_image> final_image(const_cast<dng_render&>(renderer).Render());
    if (!final_image.Get()) {
        return false;
    }
    out_w = final_image->Width();
    out_h = final_image->Height();
    out_rgb.resize(static_cast<size_t>(out_w) * out_h * 3);
    dng_pixel_buffer buffer;
    buffer.fArea = final_image->Bounds();
    buffer.fPlane = 0;
    buffer.fPlanes = 3;
    buffer.fPixelType = ttByte;
    buffer.fPixelSize = 1;
    buffer.fData = out_rgb.data();
    buffer.fRowStep = static_cast<int32>(out_w * 3);
    buffer.fColStep = 3;
    buffer.fPlaneStep = 1;
    final_image->Get(buffer);
    return true;
}

// Pool-backed overload: out_rgb_ptr/out_rgb_size supplied by RgbOutputPool.
// No resize, no page-fault cost.  SDK fallback path (renderer.Render()) is
// not available here since we cannot safely write into an arbitrary pointer
// whose capacity may not match the resized output; return false in that case
// so the caller falls back to the vector overload.
bool runHalideFullOrSdkFallback(dng_host& host,
                                dng_negative& negative,
                                dng_image* stage3,
                                const dng_render& renderer,
                                const PipelineConfig& config,
                                uint8_t* out_rgb_ptr,
                                size_t out_rgb_size,
                                uint32_t& out_w,
                                uint32_t& out_h) {
    const dng_point dst_size = computeOutputSize(negative, renderer);
    out_w = static_cast<uint32_t>(dst_size.h);
    out_h = static_cast<uint32_t>(dst_size.v);

    // Pool path: no resize, no page fault. W7-B: fused path outputs RGBA8.
    const size_t needed_out_size =
        static_cast<size_t>(out_w) * out_h * (config.fuse_rgba_output ? 4 : 3);
    if (out_rgb_size < needed_out_size || !out_rgb_ptr) {
        return false;
    }

    dng_rect src_area = negative.DefaultCropArea();

    dng_image* source_image = stage3;
    dng_rect source_area = src_area;
    AutoPtr<dng_image> resized_stage3;
    const bool need_resample = src_area.Size() != dst_size;
    if (need_resample) {
        resized_stage3.Reset(host.Make_dng_image(dst_size, stage3->Planes(), stage3->PixelType()));
        if (!resized_stage3.Get()) {
            return false;
        }
        ResampleImage(host,
                      *stage3,
                      *resized_stage3.Get(),
                      src_area,
                      resized_stage3->Bounds(),
                      dng_resample_bicubic::Get());
        source_image = resized_stage3.Get();
        source_area = resized_stage3->Bounds();
    }

    uint32_t src_w = source_area.W();
    uint32_t src_h = source_area.H();
    uint32_t src_p = source_image->Planes();
    std::vector<uint16_t> stage3_data16;
    RenderParams params;
    if (!buildRenderParams(host, negative, renderer, config, params)) {
        return false;
    }

    const bool can_use_u16_stage3 = source_image->PixelType() == ttShort &&
                                    source_image->PixelRange() != 0;
    if (can_use_u16_stage3) {
        const uint16_t* stage3_u16_ptr = nullptr;
        std::unique_ptr<dng_const_tile_buffer> stage3_borrowed_tile;
        int32_t stage3_row_step   = static_cast<int32_t>(src_w * src_p);
        int32_t stage3_col_step   = static_cast<int32_t>(src_p);
        int32_t stage3_plane_step = 1;
        if (borrowStage3Interleaved16(source_image,
                                      source_area,
                                      stage3_borrowed_tile,
                                      stage3_u16_ptr,
                                      src_w,
                                      src_h,
                                      src_p,
                                      stage3_row_step,
                                      stage3_col_step,
                                      stage3_plane_step)) {
            // Borrowed path.
        } else {
            extractStage3Interleaved16(source_image, source_area, stage3_data16, src_w, src_h, src_p);
            stage3_u16_ptr = stage3_data16.data();
        }
        const float src_scale = 1.0f / static_cast<float>(source_image->PixelRange());
        const bool render_ok = runRenderStage4HalideAot(stage3_u16_ptr,
                                                         static_cast<int>(src_w),
                                                         static_cast<int>(src_h),
                                                         static_cast<int>(src_p),
                                                         stage3_row_step,
                                                         stage3_col_step,
                                                         stage3_plane_step,
                                                         src_scale,
                                                         static_cast<int>(out_w),
                                                         static_cast<int>(out_h),
                                                         params,
                                                         out_rgb_ptr,
                                                         config.fuse_rgba_output);
        if (render_ok) {
            return true;
        }
    }

    // W7-B: the SDK fallback below writes interleaved RGB8; it cannot satisfy an
    // RGBA8 fused buffer. On the fused path the GPU render is mandatory (callers
    // already refuse SDK CPU fallback), so fail rather than emit RGB-in-RGBA.
    if (config.fuse_rgba_output) {
        return false;
    }

    // SDK fallback with pool pointer: use fData directly.
    AutoPtr<dng_image> final_image(const_cast<dng_render&>(renderer).Render());
    if (!final_image.Get()) {
        return false;
    }
    const uint32_t fw = static_cast<uint32_t>(final_image->Width());
    const uint32_t fh = static_cast<uint32_t>(final_image->Height());
    const size_t fallback_size = static_cast<size_t>(fw) * fh * 3;
    if (fallback_size > out_rgb_size) {
        // Pool buffer too small for fallback size — should not happen in practice.
        return false;
    }
    out_w = fw;
    out_h = fh;
    dng_pixel_buffer buffer;
    buffer.fArea = final_image->Bounds();
    buffer.fPlane = 0;
    buffer.fPlanes = 3;
    buffer.fPixelType = ttByte;
    buffer.fPixelSize = 1;
    buffer.fData = out_rgb_ptr;
    buffer.fRowStep = static_cast<int32>(out_w * 3);
    buffer.fColStep = 3;
    buffer.fPlaneStep = 1;
    final_image->Get(buffer);
    return true;
}

}  // namespace

const char* renderHalideModeName(RenderHalideMode mode) {
    switch (mode) {
        case RenderHalideMode::SDK: return "sdk";
        case RenderHalideMode::HALIDE_METAL: return "halide-metal";
        case RenderHalideMode::HALIDE_GPU: return "HALIDE_GPU";
        case RenderHalideMode::AUTO: return "auto";
    }
    return "unknown";
}

#if defined(__ANDROID__)
namespace {
// T9 (Gotcha #62): lazy-zero scratch for warmup dummy buffers. mmap MAP_ANON
// hands back zero-pages committed only on first touch (the Halide H2D upload),
// avoiding std::vector(N,0)'s eager full-buffer memset (~2s for the ~410MB of
// S4 dummy src+dst at 6048x4024). munmap (or free for the calloc fallback)
// releases on scope exit.
struct LazyZeroBuf {
    void* ptr = nullptr;
    size_t bytes = 0;
    bool mmaped = false;
    explicit LazyZeroBuf(size_t n) : bytes(n) {
        ptr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
        if (ptr == MAP_FAILED) {
            ptr = std::calloc(1, bytes);  // OS-zeroed fallback
            mmaped = false;
        } else {
            mmaped = true;
        }
    }
    ~LazyZeroBuf() {
        if (mmaped && ptr != nullptr) {
            ::munmap(ptr, bytes);
        } else if (ptr != nullptr) {
            std::free(ptr);
        }
    }
    LazyZeroBuf(const LazyZeroBuf&) = delete;
    LazyZeroBuf& operator=(const LazyZeroBuf&) = delete;
};
}  // namespace
#endif  // __ANDROID__

void dng_render_stage4_prewarm_for_size(int width, int height) {
#if defined(__ANDROID__)
    // W7-E: build the Stage4 render GPU pipeline at the actual image size so the
    // first real decode skips the Vulkan pipeline-creation + first large
    // dispatch cold tax (matrix S4 ~440ms vs warm ~192ms). Mirrors the S3
    // prewarm: per-size cache + one identity-params dispatch on zeroed dummy
    // buffers, result discarded. Drives the same dng_render_stage4_android AOT
    // entry production uses (host-src vs device-src do not change the compiled
    // kernel / pipeline state).
    if (width <= 0 || height <= 0) {
        return;
    }
    static std::mutex cache_mu;
    static std::unordered_set<uint64_t> warmed;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(width)) << 32) |
                         static_cast<uint64_t>(static_cast<uint32_t>(height));
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        if (warmed.count(key)) {
            return;
        }
    }

    // Identity RenderParams via the exact toIdentity*/ensureSafeHueSatMap path
    // buildRenderParams uses for the linear/no-profile case (Gotcha #92 shape
    // sanitization). The AOT kernel only reads the float members + dims/flags;
    // the SDK-object scaffolding is unused. Values are irrelevant (output
    // discarded) — only buffer shapes drive the Vulkan pipeline specialization.
    RenderParams params;
    toIdentityCurve(params.exp_ramp);
    toIdentityCurve(params.tone_curve);
    toIdentityCurve(params.encode_gamma);
    params.camera_white[0] = params.camera_white[1] = params.camera_white[2] = 1.0f;
    params.camera_to_rgb[0] = params.camera_to_rgb[4] = params.camera_to_rgb[8] = 1.0f;
    params.rgb_to_final[0] = params.rgb_to_final[4] = params.rgb_to_final[8] = 1.0f;
    toIdentityHueSatMap(params.huesat_table, params.huesat_hue_div,
                        params.huesat_sat_div, params.huesat_val_div,
                        params.huesat_has_table);
    toIdentityHueSatMap(params.look_table, params.look_hue_div,
                        params.look_sat_div, params.look_val_div,
                        params.look_has_table);
    toIdentityCurve(params.huesat_encode);
    toIdentityCurve(params.huesat_decode);
    toIdentityCurve(params.look_encode);
    toIdentityCurve(params.look_decode);
    ensureSafeHueSatMap(params.huesat_table, params.huesat_hue_div,
                        params.huesat_sat_div, params.huesat_val_div,
                        params.huesat_has_table);
    ensureSafeHueSatMap(params.look_table, params.look_hue_div,
                        params.look_sat_div, params.look_val_div,
                        params.look_has_table);

    // Dense interleaved RGB16 dummy src + RGB8 dummy dst at actual size.
    const size_t srcElems =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    const size_t dstBytes =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    LazyZeroBuf dummy_src(srcElems * sizeof(uint16_t));
    LazyZeroBuf dummy_dst(dstBytes * sizeof(uint8_t));

    std::fprintf(stderr, "[Warmup] s4 begin %dx%d\n", width, height);
    std::fflush(stderr);
    if (dummy_src.ptr != nullptr && dummy_dst.ptr != nullptr) {
        (void)runRenderStage4HalideAot(static_cast<uint16_t*>(dummy_src.ptr),
                                       width, height, /*src_p=*/3,
                                       /*src_row_step=*/width * 3,
                                       /*src_col_step=*/3,
                                       /*src_plane_step=*/1,
                                       /*src_scale=*/1.0f / 65535.0f,
                                       /*dst_w=*/width, /*dst_h=*/height,
                                       params, static_cast<uint8_t*>(dummy_dst.ptr),
                                       /*fuse_rgba=*/false);
    }
    std::fprintf(stderr, "[Warmup] s4 done\n");
    std::fflush(stderr);

    {
        std::lock_guard<std::mutex> lock(cache_mu);
        warmed.insert(key);
    }
#else
    (void)width;
    (void)height;
#endif
}

bool render_stage4_halide(dng_host& host,
                          dng_negative& negative,
                          const dng_render& renderer,
                          RenderHalideMode mode,
                          const PipelineConfig& config,
                          std::vector<uint8_t>& out_rgb,
                          uint32_t& out_w,
                          uint32_t& out_h) {
    if (mode == RenderHalideMode::SDK) {
        return false;
    }

    dng_image* stage3 = const_cast<dng_image*>(negative.Stage3Image());
    if (!stage3 || stage3->Planes() < 3) {
        return false;
    }

    return runHalideFullOrSdkFallback(host,
                                      negative,
                                      stage3,
                                      renderer,
                                      config,
                                      out_rgb,
                                      out_w,
                                      out_h);
}

bool render_stage4_halide_from_device_buffer(dng_host& host,
                                              dng_negative& negative,
                                              const dng_render& renderer,
                                              halide_buffer_t* stage3_device_buf,
                                              float src_scale,
                                              const PipelineConfig& config,
                                              std::vector<uint8_t>& out_rgb,
                                              uint32_t& out_w,
                                              uint32_t& out_h) {
    if (!stage3_device_buf) {
        return false;
    }

    // W6-2 / TD-20: shared dst_size helper (mirrors runHalideFullOrSdkFallback).
    const dng_point dst_size = computeOutputSize(negative, renderer);
    out_w = static_cast<uint32_t>(dst_size.h);
    out_h = static_cast<uint32_t>(dst_size.v);

    // Device handoff only works without resample.
    const dng_rect src_area = negative.DefaultCropArea();
    if (src_area.W() != out_w || src_area.H() != out_h) {
        return false;
    }

    // W7 (M-11): caller's vector stays RGB (W*H*3). macOS kernel writes RGBA into
    // persistent scratch; runRenderStage4HalideAotFromDevice strips to the caller's
    // buffer (fuse_rgba=false default).
    const size_t needed = static_cast<size_t>(out_w) * out_h * 3;
    if (out_rgb.size() != needed) {
        out_rgb.resize(needed);
    }

    RenderParams params;
    if (!buildRenderParams(host, negative, renderer, config, params)) {
        return false;
    }

    return runRenderStage4HalideAotFromDevice(
        stage3_device_buf, src_scale,
        static_cast<int>(src_area.l), static_cast<int>(src_area.t),
        static_cast<int>(out_w), static_cast<int>(out_h),
        params, out_rgb.data());
}

// ---------------------------------------------------------------------------
// Pool-backed overloads (Phase 10 Sprint E-rgb-pool)
// These wrap the vector overloads using a non-owning std::vector that points
// at caller-allocated (mmap pool) memory.  The vector's allocator is bypassed
// by constructing it from an existing pointer; instead we build a minimal
// wrapper that satisfies the internal API without re-allocating.
//
// Implementation note: we cannot construct a std::vector from a raw pointer
// without ownership.  The cleanest zero-copy path is to delegate to the
// internal helpers directly.  For render_stage4_halide we call
// runHalideFullOrSdkFallback via a local vector that we swap-in after the
// render call.  But that still page-faults on resize.
//
// Instead, we use a thin shim std::vector that wraps the pool pointer:
// since std::vector does not support non-owning pointers, we reuse the
// existing vector overloads by passing a vector whose .data() already points
// to the pool memory AND whose .size() already equals the needed bytes.
// We achieve this by assigning into a local vector<uint8_t> that is
// move-constructed from the pool-sized data, then after the call we confirm
// the pointer has not changed (render path only ever writes, never reallocates
// when size is correct).
//
// Actually the simplest correct approach: build a local std::vector backed by
// a custom allocator.  That's too complex.  The real simplest approach:
// replicate the render body but pass out_rgb_ptr directly.  We factor out the
// core logic via an internal ptr-based helper defined in the same TU, which
// the vector overloads already call indirectly.  We add ptr-based internal
// variants of runHalideFullOrSdkFallback and the device-handoff path.
//
// For this sprint we use the straightforward solution: allocate a
// std::vector<uint8_t> that is already pre-sized to exactly the pool buffer
// size, call the existing overload, then verify data() has not moved (it
// won't as long as size == needed on entry).  The vector resize inside the
// callee is a no-op (size already correct), so there is zero page-fault cost.
// ---------------------------------------------------------------------------

bool render_stage4_halide(dng_host& host,
                          dng_negative& negative,
                          const dng_render& renderer,
                          RenderHalideMode mode,
                          const PipelineConfig& config,
                          uint8_t* out_rgb_ptr,
                          size_t out_rgb_size,
                          uint32_t& out_w,
                          uint32_t& out_h) {
    if (mode == RenderHalideMode::SDK) {
        return false;
    }

    dng_image* stage3 = const_cast<dng_image*>(negative.Stage3Image());
    if (!stage3 || stage3->Planes() < 3) {
        return false;
    }

    // Wrap the pool pointer in a non-owning vector alias.
    // We build a std::vector by aliasing the pool memory: since the pool
    // buffer is already exactly out_rgb_size bytes, the resize() inside
    // runHalideFullOrSdkFallback is a no-op (no realloc, no page fault).
    // We use the assign-from-pointer trick: default-construct, then
    // swap with a vector that owns a copy — but that still copies.
    //
    // Correct zero-copy path: use a std::vector whose internal buffer IS the
    // pool pointer.  This requires calling runHalideFullOrSdkFallback with a
    // vector whose .data() == out_rgb_ptr.  We achieve this by calling the
    // existing vector overload with a vector pre-sized to out_rgb_size that
    // wraps the pool memory via placement:
    //   1. Construct a vector of size out_rgb_size (may page-fault on first
    //      call — but on the first FFI call the pool has already been
    //      committed by the caller before passing here, so this is a no-op
    //      resize).
    //   2. memcpy of 72 MB defeats the purpose.
    //
    // The ONLY zero-copy approach without changing the internal helper
    // signature is to use a custom allocator or to duplicate the helper.
    // We duplicate the minimal part: re-use the existing implementation path
    // by routing through runHalideFullOrSdkFallback directly.
    // Since it lives in the same TU (anonymous namespace) we call it here.
    return runHalideFullOrSdkFallback(host,
                                      negative,
                                      stage3,
                                      renderer,
                                      config,
                                      out_rgb_ptr,
                                      out_rgb_size,
                                      out_w,
                                      out_h);
}

bool render_stage4_halide_from_device_buffer(dng_host& host,
                                              dng_negative& negative,
                                              const dng_render& renderer,
                                              halide_buffer_t* stage3_device_buf,
                                              float src_scale,
                                              const PipelineConfig& config,
                                              uint8_t* out_rgb_ptr,
                                              size_t out_rgb_size,
                                              uint32_t& out_w,
                                              uint32_t& out_h) {
    if (!stage3_device_buf) {
        return false;
    }

    // W6-2 / TD-20: shared dst_size helper.
    const dng_point dst_size = computeOutputSize(negative, renderer);
    out_w = static_cast<uint32_t>(dst_size.h);
    out_h = static_cast<uint32_t>(dst_size.v);

    const dng_rect src_area = negative.DefaultCropArea();
    if (src_area.W() != out_w || src_area.H() != out_h) {
        return false;
    }

    // Pool path: buffer is already committed — no resize, no page fault.
    // W7-B: fused path outputs RGBA8.
    const size_t needed =
        static_cast<size_t>(out_w) * out_h * (config.fuse_rgba_output ? 4 : 3);
    if (out_rgb_size < needed || !out_rgb_ptr) {
        return false;
    }

    RenderParams params;
    if (!buildRenderParams(host, negative, renderer, config, params)) {
        return false;
    }

    return runRenderStage4HalideAotFromDevice(
        stage3_device_buf, src_scale,
        static_cast<int>(src_area.l), static_cast<int>(src_area.t),
        static_cast<int>(out_w), static_cast<int>(out_h),
        params, out_rgb_ptr, config.fuse_rgba_output);
}
