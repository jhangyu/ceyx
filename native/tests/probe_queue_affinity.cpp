// probe_queue_affinity.cpp — R1-T3 queue-ownership key audit probe.
//
// PURPOSE (measure only, never fix): answer the single question that decides
// whether R1-T2 can hand out one MTLCommandQueue per in-flight decode —
//   for ONE decode, do all Halide Metal entry points arrive with the same key?
// Two candidate keys are logged for every entry: the Halide `user_context`
// pointer and the calling OS thread (pthread_self()).
//
// MECHANISM: Halide v21 declares halide_metal_acquire_context /
// halide_metal_release_context WEAK (metal_v21.cpp:338, :378). Defining strong
// versions in a TU that is linked INTO libdng_decoder_native.dylib wins over
// the archive's weak definitions (verified with `nm -m`: both symbols flip
// from "weak external" to "external"). An executable-side definition would NOT
// win, because macOS two-level namespace binds the dylib to its own copy.
//
// BEHAVIOURAL PARITY: this probe reproduces today's behaviour exactly — ONE
// shared MTLDevice and ONE shared MTLCommandQueue, with a process-wide mutex
// acquired inside acquire_context and released inside release_context, which
// is precisely what the stock runtime does (thread_lock, metal_v21.cpp:284).
// Nothing about scheduling or ordering changes, so a decode run with the probe
// present must be byte-identical to one without it (probe AC4).
//
// ENTRY-POINT KIND: acquire_context receives no marker saying which Halide
// entry point called it, so the kind is recovered from the call stack
// (backtrace(3)). Every halide_metal_* entry point is an exported symbol in
// the dylib (nm: "weak external _halide_metal_device_malloc" etc.), so
// backtrace_symbols() resolves the caller by name. The first frame whose name
// starts with "_halide_metal_" and is neither of the two override functions is
// reported as fn=<kind>; if no such frame is found the raw frame is emitted so
// an absence is visible rather than silently dropped.
//
// OUTPUT: one line per acquire/release to the file named by
// DNG_QUEUE_PROBE_LOG (append; stderr if unset):
//   entry|ev=<acquire|release>|fn=<kind>|create=<0|1>|user_context=<ptr>
//         |thread=<ptr>|seq=<n>
// Lines are emitted under the same mutex that serialises the context, so the
// log is totally ordered and interleaving is real, not an artefact.
//
// THIS TU IS NEVER SHIPPED. It is unreferenced by CMake at ticket close; the
// only way it enters a binary is a temporary, reverted build-system edit.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <execinfo.h>
#include <pthread.h>

#include <objc/message.h>
#include <objc/runtime.h>

struct halide_metal_device;
struct halide_metal_command_queue;

extern "C" void *MTLCreateSystemDefaultDevice(void);

namespace {

// INTENTIONALLY LEAKED (never destroyed). Halide's Metal teardown runs from
// static destructors / atexit (halide_metal_device_release), i.e. AFTER
// function-local statics in this TU would have been destroyed — locking a
// destroyed std::mutex aborts with "mutex lock failed: Invalid argument",
// which is exactly what the first probe run hit. The stock runtime has the
// same property (its thread_lock is a POD spinlock with no destructor), so
// leaking here is parity, not sloppiness.
std::mutex &context_lock() {
  static std::mutex *m = new std::mutex();
  return *m;
}

// Serialises log writes independently of the context lock so release paths
// that run outside it still produce ordered output.
std::mutex &log_lock() {
  static std::mutex *m = new std::mutex();
  return *m;
}

FILE *log_file() {
  static FILE *f = []() -> FILE * {
    const char *path = std::getenv("DNG_QUEUE_PROBE_LOG");
    if (!path || !path[0]) return stderr;
    FILE *out = std::fopen(path, "a");
    return out ? out : stderr;
  }();
  return f;
}

uint64_t next_seq() {
  static std::atomic<uint64_t> n{0};
  return ++n;
}

// Recover the Halide entry-point kind from the call stack. Returns a caller
// owned static buffer valid until the next call on the same thread.
const char *caller_entry_point() {
  static thread_local char name[256];
  name[0] = '\0';

  void *frames[16];
  const int n = backtrace(frames, 16);
  if (n <= 0) {
    std::snprintf(name, sizeof(name), "UNKNOWN_no_backtrace");
    return name;
  }
  char **syms = backtrace_symbols(frames, n);
  if (!syms) {
    std::snprintf(name, sizeof(name), "UNKNOWN_no_symbols");
    return name;
  }

  const char *fallback = nullptr;
  for (int i = 1; i < n; ++i) {
    // backtrace_symbols format: "<idx> <image> <addr> <symbol> + <off>"
    const char *hit = std::strstr(syms[i], "_halide_metal_");
    if (!hit) {
      hit = std::strstr(syms[i], "halide_metal_");
    }
    if (!hit) continue;
    if (!fallback) fallback = syms[i];
    if (std::strstr(hit, "acquire_context") || std::strstr(hit, "release_context")) {
      continue;  // our own override frame
    }
    // Copy up to the next space (strips "+ <offset>").
    size_t j = 0;
    while (hit[j] && hit[j] != ' ' && j + 1 < sizeof(name)) {
      name[j] = hit[j];
      ++j;
    }
    name[j] = '\0';
    break;
  }
  if (!name[0]) {
    // Classification failed. Emit the raw stack instead of a bare "UNKNOWN",
    // so an unclassified entry is auditable evidence rather than a hole in the
    // instrument-validation table (probe AC2).
    size_t pos = 0;
    pos += std::snprintf(name, sizeof(name), "%s{",
                         fallback ? "ONLY_CONTEXT_FRAMES" : "UNCLASSIFIED");
    for (int i = 1; i < n && i < 7 && pos + 2 < sizeof(name); ++i) {
      // Keep only the symbol column and drop spaces so the log stays
      // single-token per field.
      const char *s = syms[i];
      const char *sym = std::strrchr(s, ' ');
      const char *start = s;
      // walk back to the symbol name: "<idx> <image> <addr> <sym> + <off>"
      int spaces = 0;
      for (const char *p = s; *p; ++p) {
        if (*p == ' ' && (p == s || p[-1] != ' ')) {
          ++spaces;
          if (spaces == 3) {
            start = p + 1;
            break;
          }
        }
      }
      (void)sym;
      for (size_t j = 0; start[j] && start[j] != ' ' && pos + 1 < sizeof(name); ++j) {
        name[pos++] = start[j];
      }
      if (pos + 1 < sizeof(name)) name[pos++] = ';';
    }
    if (pos + 1 < sizeof(name)) name[pos++] = '}';
    name[pos] = '\0';
  }
  free(syms);
  return name;
}

void emit(const char *ev, const char *fn, int create, const void *user_context,
          const void *thread) {
  const uint64_t seq = next_seq();
  std::lock_guard<std::mutex> g(log_lock());
  FILE *f = log_file();
  std::fprintf(f,
               "entry|ev=%s|fn=%s|create=%d|user_context=%p|thread=%p|seq=%llu\n",
               ev, fn, create, user_context, thread,
               static_cast<unsigned long long>(seq));
  std::fflush(f);
}

// Created once, retained for process lifetime — exactly what the stock
// runtime does with its singletons. Only ever touched under context_lock().
void *g_device = nullptr;
void *g_queue = nullptr;

void ensure_context() {
  if (g_device && g_queue) return;
  if (!g_device) g_device = MTLCreateSystemDefaultDevice();
  if (g_device && !g_queue) {
    using MsgSend = void *(*)(void *, SEL);
    g_queue = reinterpret_cast<MsgSend>(objc_msgSend)(
        g_device, sel_registerName("newCommandQueue"));
  }
}

}  // namespace

extern "C" int halide_metal_acquire_context(void *user_context,
                                            struct halide_metal_device **device_ret,
                                            struct halide_metal_command_queue **queue_ret,
                                            bool create) {
  const char *fn = caller_entry_point();

  // Same discipline as the stock runtime: the process-wide lock is taken here
  // and released in release_context. Parity is the point of this probe.
  context_lock().lock();

  emit("acquire", fn, create ? 1 : 0, user_context,
       reinterpret_cast<void *>(pthread_self()));

  if (create) {
    ensure_context();
    if (!g_device || !g_queue) {
      context_lock().unlock();
      return -1;  // halide_error_code_generic_error
    }
  }
  // create == false: stock semantics are "hand back whatever already exists,
  // possibly null, and still succeed" — never create on this path.

  *device_ret = reinterpret_cast<struct halide_metal_device *>(g_device);
  *queue_ret = reinterpret_cast<struct halide_metal_command_queue *>(g_queue);
  return 0;
}

extern "C" int halide_metal_release_context(void *user_context) {
  emit("release", "-", 0, user_context, reinterpret_cast<void *>(pthread_self()));
  context_lock().unlock();
  return 0;
}

// AC4/nm content marker so a built binary can be proven to contain this probe
// by symbol rather than by mtime.
extern "C" const char *ceyx_queue_affinity_probe_v1() {
  return "ceyx_queue_affinity_probe_v1";
}
