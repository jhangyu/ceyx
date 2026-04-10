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
#include <cstdlib>

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
        if (const char* env = std::getenv("DNG_AREA_THREADS")) {
            int v = std::atoi(env);
            if (v > 0) {
                return static_cast<uint32>(v);
            }
        }
        uint32 threads = std::thread::hardware_concurrency();
        return threads > 1 ? threads : 1;
    }

    virtual void PerformAreaTask(dng_area_task &task, const dng_rect &area) override {
        uint32 threads = PerformAreaTaskThreads();
        uint32 maxThreads = task.MaxThreads();
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
        // We will slice the area horizontally based on the number of threads.
        // It's crucial to align partitions to tileSize boundaries to avoid issues.
        int32 totalHeight = area.H();
        int32 tileH = tileSize.v;
        int32 numTilesV = (totalHeight + tileH - 1) / tileH;
        
        if (numTilesV < threads) {
            threads = numTilesV > 0 ? numTilesV : 1;
        }

        std::vector<std::future<void>> futures;
        std::mutex exceptionMutex;
        std::exception_ptr caughtException = nullptr;

        int32 tilesPerThread = numTilesV / threads;
        int32 extraTiles = numTilesV % threads;
        
        int32 currentV = area.t;

        for (uint32 i = 0; i < threads; ++i) {
            int32 tilesForThisThread = tilesPerThread + (i < extraTiles ? 1 : 0);
            if (tilesForThisThread == 0) continue;

            int32 heightForThisThread = tilesForThisThread * tileH;
            
            dng_rect subArea = area;
            subArea.t = currentV;
            subArea.b = std::min(currentV + heightForThisThread, area.b);
            
            currentV += heightForThisThread;

            futures.push_back(std::async(std::launch::async, [this, &task, i, subArea, tileSize, &exceptionMutex, &caughtException]() {
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
