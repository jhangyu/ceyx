#pragma once

#include <dng_host.h>
#include <dng_area_task.h>
#include <dng_exceptions.h>
#include <dng_rect.h>
#include <thread>
#include <vector>
#include <future>
#include <iostream>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "dng_pipeline_config.h"

// Mutex rework (plan Task 4): forward declaration only, so the include graph
// stays acyclic — decode_context.h forward-declares dng_host in turn.
struct DecodeContext;

class ConcurrentDngHost : public dng_host {
public:
    // W6-3 / TD-21: cache PipelineConfig at construction time so
    // PerformAreaTaskThreads() doesn't re-read DNG_AREA_THREADS on every
    // area-task entry (Stage1 can hit this dozens of times per decode).
    explicit ConcurrentDngHost(uint32_t requestedThreads = 0)
        : dng_host(),
          requestedThreads_(requestedThreads),
          cachedConfig_(PipelineConfig::loadFromEnv()) {}
    virtual ~ConcurrentDngHost() {}

    virtual uint32 PerformAreaTaskThreads() override {
        // Priority: explicit constructor override > env var > hardware concurrency.
        uint32_t threads;
        if (requestedThreads_ > 0) {
            threads = requestedThreads_;
        } else {
            const uint32_t configuredThreads = cachedConfig_.threads.area_threads;
            threads = configuredThreads > 0 ? configuredThreads
                                             : std::thread::hardware_concurrency();
        }
        // Q3a (Round 2 perf diag): whichever source resolved `threads` above,
        // never exceed hardware_concurrency(). kDefaultAreaThreads (20, see
        // dng_pipeline_config.h) is a fixed upper bound sized for the largest
        // supported device tier; on machines with fewer logical cores it
        // over-provisioned std::async workers for every decode (extra
        // create/join overhead with no added parallelism, since only
        // hardware_concurrency() of them can run simultaneously anyway).
        // DNG_AREA_THREADS still resolves `threads` above (still honored as
        // the configured value) — it is simply subject to the same ceiling.
        const uint32_t hw = std::thread::hardware_concurrency();
        if (hw > 0 && threads > hw) {
            threads = hw;
        }
        return threads > 0 ? threads : 1;
    }

    // Mutex rework (plan Task 4): per-decode state, set by
    // dng_pipeline_decode_to_rgb_sized immediately after construction.
    // Non-owning — the context outlives the host within one decode frame.
    void setDecodeContext(DecodeContext* ctx) { decodeContext_ = ctx; }
    DecodeContext* decodeContext() const { return decodeContext_; }

    virtual void PerformAreaTask(dng_area_task &task, const dng_rect &area) override {
        uint32 threads = PerformAreaTaskThreads();
        uint32 maxThreads = task.MaxThreads();
        // DNG_STAGE1_TIMING — DiagnosticConfig (see dng_pipeline_config.h).
        // Hoisted once per entry; reused below for both the M-9 create/join
        // measurement and the per-slice launch_delay_us instrumentation.
        const bool timing = stage1TimingEnabled();
        {
            if (timing) {
                std::fprintf(stderr,
                    "[CDngHostEntry] PerformAreaTask threads=%u maxThreads=%u"
                    " area=(t=%d,l=%d,b=%d,r=%d) area.H=%d area.W=%d numTilesV=%d\n",
                    threads, maxThreads,
                    area.t, area.l, area.b, area.r,
                    area.H(), area.W(),
                    (int)((area.H() + 15) / 16));
                std::fflush(stderr);
            }
        }
        if (threads > maxThreads) {
            threads = maxThreads;
        }
        if (threads < 1) {
            threads = 1;
        }

        if (threads == 1) {
            // Fallback to single thread
            dng_host::PerformAreaTask(task, area);
            return;
        }

        dng_point tileSize(task.FindTileSize(area));

        // Determine how to partition the area.
        // Primary: slice vertically (by rows of tiles).
        // Fallback: when numTilesV < threads (e.g. Stage1 synthetic area is 16×16N),
        //   switch to horizontal slicing so N lambdas are actually launched.
        // Stage2/3/4 areas are ~4024×6048, numTilesV≈252 >> 20; they never hit this branch.
        int32 totalHeight = area.H();
        int32 tileH = tileSize.v;
        int32 numTilesV = (totalHeight + tileH - 1) / tileH;

        int32 totalWidth = area.W();
        int32 tileW = tileSize.h;
        int32 numTilesH = (totalWidth + tileW - 1) / tileW;

        bool sliceHorizontal = (numTilesV < (int32)threads) && (numTilesH >= numTilesV);
        int32 numTilesAlongSlice = sliceHorizontal ? numTilesH : numTilesV;

        if (numTilesAlongSlice < (int32)threads) {
            threads = numTilesAlongSlice > 0 ? (uint32)numTilesAlongSlice : 1;
        }

        // M-10: Start() must receive the same thread count Finish() will use
        // (dng_area_task.h:157 documents Finish's threads param as "Same as
        // value passed to Start"). Moved here — after both clamps (maxThreads
        // above, numTilesAlongSlice here) — so Start/Finish never disagree.
        task.Start(threads, tileSize, &Allocator(), Sniffer());

        std::vector<std::future<void>> futures;
        std::mutex exceptionMutex;
        std::exception_ptr caughtException = nullptr;

        int32 tilesPerThread = numTilesAlongSlice / threads;
        int32 extraTiles = numTilesAlongSlice % threads;

        int32 currentV = area.t;
        int32 currentH = area.l;

        // M-9 measurement (DNG_STAGE1_TIMING gate, zero cost when disabled):
        // totalCreateUs sums the wall time each std::async(...) call takes to
        // return (thread-creation overhead borne by the dispatching thread);
        // lastWorkDoneUs tracks the latest ProcessOnThread completion
        // timestamp across all spawned slices so we can measure the "join
        // tail" — time spent in the f.wait() loop below after the last slice
        // has actually finished its work (thread teardown / future
        // synchronization overhead). spawnCount is the number of std::async
        // calls actually issued (tilesForThisThread==0 slices are skipped).
        long long totalCreateUs = 0;
        uint32 spawnCount = 0;
        std::atomic<long long> lastWorkDoneUs{0};

        for (uint32 i = 0; i < threads; ++i) {
            int32 tilesForThisThread = tilesPerThread + (i < (uint32)extraTiles ? 1 : 0);
            if (tilesForThisThread == 0) continue;

            dng_rect subArea = area;
            if (sliceHorizontal) {
                int32 widthForThisThread = tilesForThisThread * tileW;
                subArea.l = currentH;
                subArea.r = std::min(currentH + widthForThisThread, area.r);
                currentH += widthForThisThread;
            } else {
                int32 heightForThisThread = tilesForThisThread * tileH;
                subArea.t = currentV;
                subArea.b = std::min(currentV + heightForThisThread, area.b);
                currentV += heightForThisThread;
            }

            auto t_dispatch = std::chrono::steady_clock::now();
            futures.push_back(std::async(std::launch::async, [this, &task, i, subArea, tileSize, &exceptionMutex, &caughtException, t_dispatch, timing, &lastWorkDoneUs]() {
                if (timing) {
                    auto t_enter = std::chrono::steady_clock::now();
                    auto launch_delay_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        t_enter - t_dispatch).count();
                    std::ostringstream oss;
                    oss << std::this_thread::get_id();
                    std::fprintf(stderr,
                        "[CDngHostLambda] i=%u tid=%s launch_delay_us=%lld\n",
                        i, oss.str().c_str(), (long long) launch_delay_us);
                    std::fflush(stderr);
                }
                try {
                    task.ProcessOnThread(i, subArea, tileSize, Sniffer());
                    if (timing) {
                        // M-9: record this slice's completion time; the max
                        // across all slices marks when "real work" finished,
                        // vs. when the main thread actually observes all
                        // futures as joined (see join_tail_us below).
                        auto t_done = std::chrono::steady_clock::now();
                        long long done_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            t_done.time_since_epoch()).count();
                        long long prev = lastWorkDoneUs.load(std::memory_order_relaxed);
                        while (done_us > prev &&
                               !lastWorkDoneUs.compare_exchange_weak(prev, done_us,
                                   std::memory_order_relaxed)) {}
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(exceptionMutex);
                    if (!caughtException) {
                        caughtException = std::current_exception();
                    }
                }
            }));

            if (timing) {
                auto t_created = std::chrono::steady_clock::now();
                totalCreateUs += std::chrono::duration_cast<std::chrono::microseconds>(
                    t_created - t_dispatch).count();
            }
            ++spawnCount;
        }

        // Wait for all threads to finish
        for (auto &f : futures) {
            f.wait();
        }

        // M-9: join tail = time between the last slice's actual work
        // completing (lastWorkDoneUs) and this point, where the main thread
        // has observed every future as joined. Only meaningful if at least
        // one slice recorded a completion timestamp.
        if (timing) {
            long long lastDone = lastWorkDoneUs.load(std::memory_order_relaxed);
            long long joinTailUs = 0;
            if (lastDone > 0) {
                auto t_joined = std::chrono::steady_clock::now();
                long long joined_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    t_joined.time_since_epoch()).count();
                joinTailUs = joined_us - lastDone;
            }
            std::fprintf(stderr,
                "[CDngHostTiming] spawn_count=%u create_us=%lld join_tail_us=%lld"
                " area=(t=%d,l=%d,b=%d,r=%d)\n",
                spawnCount, totalCreateUs, joinTailUs,
                area.t, area.l, area.b, area.r);
            std::fflush(stderr);
        }

        // Finish the task
        task.Finish(threads);

        // Re-throw any caught exception
        if (caughtException) {
            std::rethrow_exception(caughtException);
        }
    }

private:
    // DNG_STAGE1_TIMING — DiagnosticConfig (see dng_pipeline_config.h).
    // Call-site lazy cache (pattern mirrors dng_opcodelist2_halide.cpp's
    // ol2_prewarm_enabled()/map_poly_timing_enabled()): one getenv() per
    // process instead of one per PerformAreaTask entry / per slice.
    static bool stage1TimingEnabled() {
        static const bool enabled = []() {
            const char *v = std::getenv("DNG_STAGE1_TIMING");
            return v && v[0] && v[0] != '0';
        }();
        return enabled;
    }

    uint32_t requestedThreads_ = 0;
    PipelineConfig cachedConfig_{};
    DecodeContext* decodeContext_ = nullptr;
};
