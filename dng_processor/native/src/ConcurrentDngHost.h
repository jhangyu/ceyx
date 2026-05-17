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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "dng_pipeline_config.h"

class ConcurrentDngHost : public dng_host {
public:
    explicit ConcurrentDngHost(uint32_t requestedThreads = 0)
        : dng_host(), requestedThreads_(requestedThreads) {}
    virtual ~ConcurrentDngHost() {}

    virtual uint32 PerformAreaTaskThreads() override {
        // Priority: explicit constructor override > env var > hardware concurrency.
        if (requestedThreads_ > 0) {
            return requestedThreads_;
        }
        const uint32_t configuredThreads = PipelineConfig::loadFromEnv().threads.area_threads;
        if (configuredThreads > 0) {
            return configuredThreads;
        }
        uint32 threads = std::thread::hardware_concurrency();
        return threads > 1 ? threads : 1;
    }

    virtual void PerformAreaTask(dng_area_task &task, const dng_rect &area) override {
        uint32 threads = PerformAreaTaskThreads();
        uint32 maxThreads = task.MaxThreads();
        {
            const char *timing_env = std::getenv("DNG_STAGE1_TIMING");
            if (timing_env && timing_env[0] && timing_env[0] != '0') {
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
        
        // Let the task know how many threads we will use
        task.Start(threads, tileSize, &Allocator(), Sniffer());

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

        std::vector<std::future<void>> futures;
        std::mutex exceptionMutex;
        std::exception_ptr caughtException = nullptr;

        int32 tilesPerThread = numTilesAlongSlice / threads;
        int32 extraTiles = numTilesAlongSlice % threads;

        int32 currentV = area.t;
        int32 currentH = area.l;

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

            const char *timing_env = std::getenv("DNG_STAGE1_TIMING");
            const bool timing = timing_env && timing_env[0] && timing_env[0] != '0';
            auto t_dispatch = std::chrono::steady_clock::now();
            futures.push_back(std::async(std::launch::async, [this, &task, i, subArea, tileSize, &exceptionMutex, &caughtException, t_dispatch, timing]() {
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
                } catch (...) {
                    std::lock_guard<std::mutex> lock(exceptionMutex);
                    if (!caughtException) {
                        caughtException = std::current_exception();
                    }
                }
            }));
        }

        // Wait for all threads to finish
        for (auto &f : futures) {
            f.wait();
        }

        // Finish the task
        task.Finish(threads);

        // Re-throw any caught exception
        if (caughtException) {
            std::rethrow_exception(caughtException);
        }
    }

private:
    uint32_t requestedThreads_ = 0;
};
