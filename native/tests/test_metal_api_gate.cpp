// test_metal_api_gate.cpp — R4 item 3, EVIDENCE TRACK B.
//
// WHAT THIS OBSERVES, and why it is not a proxy.
// The defect is two unsynchronised stores inside Halide's _halide_metal_run,
// executed only while the memo cache is unpopulated.  The preflight established
// that ThreadSanitizer cannot see them — halide_runtime.a is emitted by the
// Halide AOT generator, never compiled by our toolchain, so no build flag can
// instrument the only object containing both racing accesses (evidence:
// docs/logs/2026-09-05/item3-P0-result.md).  This harness therefore measures the
// PRECONDITION of the race directly: how many threads are simultaneously inside
// the bracket that strictly contains the racy block.  Two or more threads inside
// it at once IS the race condition, observed rather than inferred.
//
// GEOMETRY, frozen before any number exists (docs/logs/2026-09-05/item3-prereg.md):
//   * one fresh PROCESS per run — the memo is populated once per process, so
//     in-process repetition measures nothing after the first launch;
//   * kThreads at the slider maximum of 8, to open the window as wide as the
//     product ever allows;
//   * 40 runs per arm; one binary for both arms, differing only by the
//     environment variable, so their identity is provable by UUID;
//   * RED  (CEYX_METAL_API_GATE=0): PASS iff max occupancy >= 2 in at least one
//          run of the 40.  This is the positive control: it proves the harness
//          can see the condition at all.
//   * GREEN (gate enabled): PASS iff max occupancy == 1 in every run.
// A silent RED arm means the harness is blind and is reported as such, never as
// a pass.
//
// WHY A BARE AOT KERNEL AND NO RAW SAMPLE FILE.
// The memo branch needs a Metal kernel launch with a non-empty argument block
// (it is guarded by the total argument size being non-zero — see `cbz x13` at
// 0xe168 in item3/arx/memo_region.txt).  dng_demosaic_bilinear takes two scalar
// arguments, so a tiny buffer through that kernel reaches the branch without any
// decode machinery or sample data.  Ruling r-2 already established that real RAW
// samples are not required for a race test.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "HalideBuffer.h"
#include "dng_demosaic_bilinear.h"
#include "dng_metal_api_gate.h"

// LINK STUB, declared deliberately rather than discovered at signoff.
// dng_metal_context.cpp calls dng_decode_slot_count_relaxed() (item 1, landed at
// 543bf07) to size its queue pool.  The real definition is in dng_pipeline.cpp,
// which would drag the entire decode pipeline, the DNG SDK and every AOT kernel
// into a target that exists to launch ONE trivial kernel on N threads.  This
// harness does not exercise slot accounting at all, so it supplies the value
// directly.  It returns 8, matching the thread count frozen in the
// pre-registration (the slider maximum), so the queue pool never becomes the
// thing that limits concurrency in the window under observation.  Note the pool
// does not block when its cap is reached — it round-robins existing queues
// (dng_metal_context.cpp assign_queue_locked) — so this value cannot serialise
// the arms and cannot flatter the GREEN arm.
size_t dng_decode_slot_count_relaxed() { return 8; }

namespace {

constexpr int kThreads = 8;   // slider maximum (r-6); widest window the product allows
constexpr int kW = 64;
constexpr int kH = 64;

// One Metal kernel launch on this thread, from cold.  Every thread does the
// identical work, so nothing but scheduling distinguishes them.
int launch_once() {
    Halide::Runtime::Buffer<uint16_t> src(kW, kH);
    Halide::Runtime::Buffer<uint16_t> dst(kW - 4, kH - 4, 3);
    src.fill(uint16_t{1024});
    return dng_demosaic_bilinear(src, 0, 0, dst);
}

}  // namespace

int main() {
#if !defined(__APPLE__) || defined(DNG_FORCE_VULKAN)
    std::printf("SKIP: Metal-only test\n");
    return 0;
#else
    // Marker check first: prove this binary contains the code under test.
    std::printf("marker=%s\n", ceyx_metal_api_gate_v1());
    const char *env = std::getenv("CEYX_METAL_API_GATE");
    std::printf("arm=%s\n", (env != nullptr && env[0] == '0') ? "RED" : "GREEN");
    std::printf("threads=%d\n", kThreads);

    std::atomic<int> failures{0};
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&failures]() {
            if (launch_once() != 0) {
                failures.fetch_add(1);
            }
        });
    }
    for (auto &t : ts) t.join();

    const int max_occ = ceyx_metal_api_gate_max_occupancy();
    std::printf("max_occupancy=%d\n", max_occ);
    std::printf("kernel_failures=%d\n", failures.load());
    // The verdict is NOT decided here: this binary reports the observation and
    // the arm it ran in, and the judgment rule frozen in the pre-registration is
    // applied to the 40 artifacts afterwards.  A harness that decides its own
    // pass condition per run is a harness that can be argued with after the
    // fact.
    return (failures.load() == 0) ? 0 : 1;
#endif
}
