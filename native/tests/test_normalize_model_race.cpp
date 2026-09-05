// test_normalize_model_race.cpp — R4 item 2: multi-threaded repro for the
// shared-static data race at src/metadata/normalize_model.cpp:406.
//
// The defect: `static const char *orig;` is a function-local static inside
// LibRaw::GetNormalizedModel(), i.e. one object shared by every thread. The
// alias tables are scanned with a carry-forward convention -- an entry starting
// with '@' is the canonical model name and is stashed in `orig`; a following
// plain entry that matches the input model triggers
// `strcpy(normalized_model, orig)`. Two threads normalising DIFFERENT camera
// models interleave that write and that read, so one thread publishes the
// other thread's canonical name into its own imgdata.idata.normalized_model.
//
// Per user ruling r-2, hitting GetNormalizedModel() directly with synthesised
// idata is sufficient acceptance evidence; no real camera RAW samples are used
// and no file is opened. GetNormalizedModel() is protected
// (internal/libraw_internal_funcs.h:127, included inside libraw.h's protected
// region at libraw.h:515), so the test reaches it through a derived class.
//
// Usage:
//   test_normalize_model_race [--threads N] [--iters N] [--selfcheck]
//
// Normal mode  : exit 0 iff zero mismatches were observed.
// --selfcheck  : positive control. One case is given a deliberately wrong
//                expectation, so the comparison MUST report mismatches. Exit 0
//                iff it did (instrument proven capable of failing); exit 3 if
//                it did not (instrument blind -- every other result from this
//                binary is then worthless).

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// GetNormalizedModel() is declared inside libraw.h's own
// `#ifdef LIBRAW_LIBRARY_BUILD / #include "internal/libraw_internal_funcs.h"`
// block (libraw.h:514-516) -- external consumers never see it. LibRaw's own
// internal .cpp files reach it via internal/dcraw_defs.h, which #defines this
// macro AND #includes internal/var_defines.h (the short-name macros --
// `model`/`make`/`normalized_model` -- this test deliberately avoids, see the
// header comment above). Defining the macro directly, without pulling in
// dcraw_defs.h/var_defines.h, exposes the declaration without the clobber.
#define LIBRAW_LIBRARY_BUILD
#include "libraw/libraw.h"

namespace {

struct Case {
  unsigned makerIndex;
  const char *make;
  const char *inputModel;
  const char *expected;
};

// Each case is an alias -> canonical mapping read straight out of
// normalize_model.cpp's tables. The canonical strings are deliberately all
// different, so cross-thread contamination is directly observable as "thread A
// produced thread B's canonical name".
//
//   Nikon      nikonalias  (write 905 / read 910)  "@COOLPIX 5700","E5700"
//   Nikon      nikonalias                          "@COOLPIX 2500","E2500"
//   Fujifilm   fujialias   (write 808 / read 813)  "@DBP for GX680","DX-2000"
//   Olympus    olyalias    (write 918 / read 921)  "@C-3030Z","C3030Z"
//   Panasonic  panalias    (write 943 / read 949)  "@DMC-FZ150","V-LUX 3"
const Case kCases[] = {
    {LIBRAW_CAMERAMAKER_Nikon, "Nikon", "E5700", "COOLPIX 5700"},
    {LIBRAW_CAMERAMAKER_Nikon, "Nikon", "E2500", "COOLPIX 2500"},
    {LIBRAW_CAMERAMAKER_Fujifilm, "Fujifilm", "DX-2000", "DBP for GX680"},
    {LIBRAW_CAMERAMAKER_Olympus, "Olympus", "C3030Z", "C-3030Z"},
    {LIBRAW_CAMERAMAKER_Panasonic, "Panasonic", "V-LUX 3", "DMC-FZ150"},
};
const size_t kCaseCount = sizeof(kCases) / sizeof(kCases[0]);

// GetNormalizedModel() is protected; a derived class is the sanctioned way to
// reach it without editing LibRaw's headers.
class ModelProbe : public LibRaw {
 public:
  void setUp(const Case &c) {
    std::memset(imgdata.idata.make, 0, sizeof(imgdata.idata.make));
    std::memset(imgdata.idata.model, 0, sizeof(imgdata.idata.model));
    std::memset(imgdata.idata.normalized_make, 0,
                sizeof(imgdata.idata.normalized_make));
    std::memset(imgdata.idata.normalized_model, 0,
                sizeof(imgdata.idata.normalized_model));
    std::snprintf(imgdata.idata.make, sizeof(imgdata.idata.make), "%s", c.make);
    std::snprintf(imgdata.idata.model, sizeof(imgdata.idata.model), "%s",
                  c.inputModel);
    imgdata.idata.maker_index = c.makerIndex;
  }

  void normalize() { GetNormalizedModel(); }

  const char *normalized() const { return imgdata.idata.normalized_model; }
};

struct Observation {
  std::atomic<unsigned long long> iterations{0};
  std::atomic<unsigned long long> mismatches{0};
  // First witnessed corruption, guarded by firstTaken so exactly one writer
  // fills it; readers only run after every thread has joined.
  std::atomic<bool> firstTaken{false};
  char firstInput[64];
  char firstExpected[64];
  char firstActual[64];

  Observation() {
    firstInput[0] = '\0';
    firstExpected[0] = '\0';
    firstActual[0] = '\0';
  }
};

void worker(const Case *c, const char *expected, unsigned long long iters,
            Observation *obs) {
  // One probe per thread, reused: LibRaw's constructor is expensive and
  // GetNormalizedModel() only reads/writes idata, which setUp() resets every
  // iteration. Reusing the instance keeps the loop tight, which widens the
  // write->read window overlap between threads.
  //
  // Heap-allocated, not a stack local: LibRaw (imgdata + friends) is a large
  // object, and std::thread's default stack (~512KB on macOS, vs. ~8MB for
  // the main thread) is not big enough for it on the stack -- discovered as
  // a real SIGBUS/stack-overflow crash (___chkstk_darwin) in every worker
  // thread on first invocation, see docs/logs/2026-09-05/race2/T3-red.txt.
  std::unique_ptr<ModelProbe> probe(new ModelProbe());
  for (unsigned long long i = 0; i < iters; ++i) {
    probe->setUp(*c);
    probe->normalize();
    if (std::strcmp(probe->normalized(), expected) != 0) {
      obs->mismatches.fetch_add(1, std::memory_order_relaxed);
      bool taken = false;
      if (obs->firstTaken.compare_exchange_strong(taken, true,
                                                  std::memory_order_acq_rel)) {
        std::snprintf(obs->firstInput, sizeof(obs->firstInput), "%s",
                      c->inputModel);
        std::snprintf(obs->firstExpected, sizeof(obs->firstExpected), "%s",
                      expected);
        std::snprintf(obs->firstActual, sizeof(obs->firstActual), "%s",
                      probe->normalized());
      }
    }
    obs->iterations.fetch_add(1, std::memory_order_relaxed);
  }
}

// Serial precondition: every case must normalise to its expected value when
// nothing else is running. If this fails, the expectation constants are wrong
// and any later "mismatch" would be a bug in the test, not a race.
int serialPrecondition() {
  int bad = 0;
  for (size_t i = 0; i < kCaseCount; ++i) {
    ModelProbe probe;
    probe.setUp(kCases[i]);
    probe.normalize();
    const bool ok = std::strcmp(probe.normalized(), kCases[i].expected) == 0;
    std::printf("serial|case=%s|expected=%s|actual=%s|%s\n",
                kCases[i].inputModel, kCases[i].expected, probe.normalized(),
                ok ? "OK" : "WRONG");
    if (!ok) ++bad;
  }
  std::printf("serial|bad=%d\n", bad);
  return bad;
}

}  // namespace

int main(int argc, char **argv) {
  unsigned long long iters = 200000ULL;
  size_t threads = 8;
  bool selfcheck = false;

  for (int a = 1; a < argc; ++a) {
    if (std::strcmp(argv[a], "--selfcheck") == 0) {
      selfcheck = true;
    } else if (std::strcmp(argv[a], "--iters") == 0 && a + 1 < argc) {
      iters = std::strtoull(argv[++a], NULL, 10);
    } else if (std::strcmp(argv[a], "--threads") == 0 && a + 1 < argc) {
      threads = static_cast<size_t>(std::strtoul(argv[++a], NULL, 10));
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[a]);
      return 2;
    }
  }
  if (threads < 2) {
    std::fprintf(stderr, "threads must be >= 2 to expose a data race\n");
    return 2;
  }

  std::printf("config|threads=%zu|iters=%llu|selfcheck=%d|cases=%zu\n", threads,
              iters, selfcheck ? 1 : 0, kCaseCount);

  const int serialBad = serialPrecondition();
  if (serialBad != 0 && !selfcheck) {
    std::fprintf(stderr,
                 "serial precondition failed (%d cases) -- expectation "
                 "constants are wrong, aborting before the threaded phase\n",
                 serialBad);
    return 2;
  }

  Observation obs;
  std::vector<std::thread> pool;
  pool.reserve(threads);
  // Round-robin the cases across threads so adjacent threads always hold
  // DIFFERENT canonical names; that difference is what makes the corruption
  // observable rather than benign.
  for (size_t t = 0; t < threads; ++t) {
    const Case *c = &kCases[t % kCaseCount];
    // Positive control: thread 0 is told to expect a string the code never
    // produces, so its comparison must report mismatches on every iteration.
    const char *expected = (selfcheck && t == 0)
                               ? "__CONTROL_NEVER_PRODUCED__"
                               : c->expected;
    pool.push_back(std::thread(worker, c, expected, iters, &obs));
  }
  for (size_t t = 0; t < pool.size(); ++t) pool[t].join();

  const unsigned long long mismatches =
      obs.mismatches.load(std::memory_order_relaxed);
  const unsigned long long done = obs.iterations.load(std::memory_order_relaxed);
  std::printf("result|iterations=%llu|mismatches=%llu\n", done, mismatches);
  if (obs.firstTaken.load(std::memory_order_acquire)) {
    std::printf("first|input=%s|expected=%s|actual=%s\n", obs.firstInput,
                obs.firstExpected, obs.firstActual);
  } else {
    std::printf("first|none\n");
  }

  if (selfcheck) {
    if (mismatches == 0) {
      std::printf("verdict|SELFCHECK_BLIND\n");
      return 3;  // instrument cannot fail -- every other result is worthless
    }
    std::printf("verdict|SELFCHECK_OK\n");
    return 0;
  }

  if (mismatches != 0) {
    std::printf("verdict|RACE_OBSERVED\n");
    return 1;
  }
  std::printf("verdict|CLEAN\n");
  return 0;
}
