// dng_copy_lock.cpp — "Lock B": de-globalize Halide v21's device_copy_mutex.
// R2-T2, parallel-decode campaign, 2026-09-05. See dng_copy_lock.h for the why.
//
// =============================================================================
// MECHANISM (demonstrated by R2-T1, not hypothesised)
// =============================================================================
// Halide declares halide_copy_to_host / halide_copy_to_device /
// halide_buffer_copy WEAK. A strong definition compiled INTO
// libdng_decoder_native.dylib displaces the archive's weak one: R2-T1's probe
// flipped all three from "weak external" to "external" in `nm -m` and decoded
// bit-identically (tmp/verify/r2t1-instrument-validation.txt §1). So NO
// halide_runtime_fork/ copy and no llvm-objcopy --weaken are needed here.
//
// The three upstream bodies are preserved verbatim by calling the exported
// internals they delegate to:
//   Halide::Runtime::Internal::copy_to_host_already_locked   (C++ mangled)
//   copy_to_device_already_locked                            (extern "C")
//   halide_buffer_copy_already_locked                        (extern "C")
// The ONLY delta is which lock is taken before entering them.
//
// =============================================================================
// WHAT UPSTREAM'S LOCK ACTUALLY PROTECTS
// =============================================================================
// Upstream says so itself (device_interface.cpp:22-27): "At present only
// halide_copy_to_host, halide_copy_to_device, and halide_buffer_copy are atomic
// with respect to each other." The invariant is the DIRTY-BIT STATE MACHINE of
// the buffers involved — host_dirty()/device_dirty()/buf->device/
// buf->device_interface — which is PER-BUFFER state. It is not a device lock and
// it is not an allocator lock:
//   * halide_device_malloc (:251) and halide_device_free (:274) take NO mutex,
//     so the Metal allocator is ALREADY entered concurrently, unlocked, today.
//     copy_to_device_already_locked:176 reaches device_malloc, so this change
//     creates no new concurrency condition there — it only removes an incidental
//     serialization of a path that upstream already runs unlocked.
//   * halide_device_sync (:216) and halide_metal_run take no mutex either.
// This TU therefore keeps the ATOMICITY UNIT (a buffer) and drops the SCOPE
// (the whole process).
//
// =============================================================================
// CONCURRENT-ENTRY SAFETY ANALYSIS — is each body safe on DISTINCT buffers?
// (Halide v21.0.0 sources; device_interface.cpp / metal.cpp line numbers as
//  audited in Halcyon/scripts/tmp/halide-audit/{device_interface,metal}_v21.cpp)
// =============================================================================
// 1. copy_to_host_already_locked (:30-54)
//      Reads/writes ONLY buf's own fields (:31 device_dirty, :37 host_dirty,
//      :50 set_device_dirty) and calls interface->impl->copy_to_host.
//      -> halide_metal_copy_to_host (metal:813): MetalContextHolder (R1-T2's
//         override, its own pool mutex, released before any Metal work),
//         halide_metal_device_sync_internal(metal_context.queue, buffer) — a
//         command buffer on THIS THREAD's queue, committed and waited on
//         (metal:701-714) — then copy_memory() between this buffer's own host
//         pointer and its own MTLBuffer contents. Every mutable object is either
//         thread-local (the queue, post R1-T2) or buffer-local.
// 2. copy_to_device_already_locked (:154-205)
//      Same shape; the one cross-buffer reach is halide_device_malloc (:176),
//      which upstream already runs unlocked (see above). Metal's device_malloc
//      (metal:573) calls malloc() (thread-safe), newBufferWithLength on the
//      shared MTLDevice (Apple documents MTLDevice as thread-safe), and
//      impl->use_module().
// 3. halide_buffer_copy_already_locked (:483-609)
//      Touches TWO buffers, src and dst, and their dirty bits (:574, :596-601).
//      That is why this TU locks BOTH stripes (in a fixed order) rather than one.
//      Its recursive/internal calls (:562, :564, :575, :580) all go to the
//      *_already_locked variants, never back through these entry points, so
//      there is no re-entrancy into our locks and no self-deadlock.
// 4. UseModule (:62-77, taken by halide_buffer_copy:621-622)
//      impl->use_module()/release_module() resolve to halide_use_jit_module /
//      halide_release_jit_module, which are EMPTY in an AOT build — verified on
//      the real artifact, not assumed:
//        otool -tvV -p _halide_use_jit_module build-r1t2/libdng_decoder_native.dylib
//        -> stp/mov/ldp/ret, no body.
//      They are NOT called by this TU: `impl` is an incomplete type in the
//      public header, so that call does not compile from here (see the note
//      above halide_buffer_copy). The omission is the ONE declared deviation
//      from upstream, and the empty bodies above are why it is inert.
// 5. debug_log_and_validate_buf (:87-126)
//      An unexported internal, so it cannot be called from here. R2-T1's probe
//      SKIPPED it; this TU instead REPLICATES it (validate_buf below) using the
//      four exported error entry points it calls. The only thing not replicated
//      is the debug() stream, which is compiled out unless the runtime is built
//      with DEBUG_RUNTIME. Validation is pure reads of the buffer's own fields.
//
// WHAT STILL NEEDS MUTUAL EXCLUSION AND STILL GETS IT:
//   * Two operations on the SAME buffer (or an overlapping src/dst pair) — the
//     stripe keyed on the buffer address serializes them exactly as before.
//   * Nothing else was found. Allocation bookkeeping is malloc's and Metal's,
//     and both are thread-safe; the dirty bits are per-buffer; the command queue
//     is per-thread after R1-T2.
//
// KNOWN RESIDUAL, NOT FIXED HERE (declared, not hidden):
//   a) halide_device_crop (:642), halide_device_slice (:672) and
//      halide_device_release_crop (:712) also take device_copy_mutex and CANNOT
//      be overridden from a ceyx TU (their bodies need the opaque
//      halide_device_interface_impl_t). Once this TU stops taking that mutex,
//      they are no longer mutually exclusive with copies. Whether that matters
//      is a REACHABILITY question, settled by measurement in
//      tmp/verify/r2t2-crop-reachability.txt — this TU is only correct on the
//      decode paths that measurement covers, and the artifact records it.
//   b) metal_api_supports_set_bytes / metal_api_checked_device (metal:941-943)
//      are written unlocked inside halide_metal_run, which is not a mutex holder
//      today either. Pre-existing, untouched, out of scope.
//
// =============================================================================
// LOCK GEOMETRY
// =============================================================================
// A fixed array of stripes, indexed by a hash of the buffer address. No
// allocation, no per-buffer lifetime bookkeeping, no map lookups on the hot
// path. Distinct buffers that collide on a stripe merely serialize (correct,
// slightly slower); the same buffer always maps to the same stripe (correct).
// The array is INTENTIONALLY LEAKED for the same reason as the queue pool:
// Halide's teardown runs from atexit/static destructors, and locking a destroyed
// std::mutex aborts.

// CEYX_COPY_LOCK_DISABLED exists ONLY so an A/B control binary can be built from
// this exact source tree with the change absent (the campaign's post-change bench
// number is otherwise not comparable to R2-T1's, which was measured on a tree
// that still had LIBRAW_NOTHREADS). It is never defined by any CMake target;
// the control build passes it in CMAKE_CXX_FLAGS on a private build dir. With it
// defined this TU compiles to nothing and Halide's weak definitions win, i.e.
// stock upstream behaviour.
#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN) && !defined(CEYX_COPY_LOCK_DISABLED)

#include "dng_copy_lock.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "HalideRuntime.h"

// ---- upstream internals we bind to (all verified exported by `nm -m` on
// ---- build-r1t2/libdng_decoder_native.dylib) ----
namespace Halide {
namespace Runtime {
namespace Internal {
int copy_to_host_already_locked(void *user_context, halide_buffer_t *buf);
}  // namespace Internal
}  // namespace Runtime
}  // namespace Halide

extern "C" int copy_to_device_already_locked(
    void *user_context, halide_buffer_t *buf,
    const halide_device_interface_t *device_interface);

extern "C" int halide_buffer_copy_already_locked(
    void *user_context, halide_buffer_t *src,
    const halide_device_interface_t *dst_device_interface, halide_buffer_t *dst);

namespace {

// 64 stripes against a 5-lane decode pool: collision probability per pair is
// 1/64, so the striping is not the serializer it replaces.
constexpr size_t kStripes = 64;

std::mutex *stripes() {
    static std::mutex *m = new std::mutex[kStripes];  // leaked on purpose
    return m;
}

// Buffers come from malloc/stack and are at least 8-byte aligned, so the low
// bits carry no entropy; shift them out, then mix with a 64-bit odd constant.
inline size_t stripe_of(const void *p) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p) >> 4;
    v *= 0x9E3779B97F4A7C15ull;
    return static_cast<size_t>((v >> 32) & (kStripes - 1));
}

// RAII for one buffer.
class StripeLock {
  public:
    explicit StripeLock(const void *p) : m_(&stripes()[stripe_of(p)]) { m_->lock(); }
    ~StripeLock() { m_->unlock(); }
    StripeLock(const StripeLock &) = delete;
    StripeLock &operator=(const StripeLock &) = delete;

  private:
    std::mutex *m_;
};

// RAII for a pair. Locks in ascending stripe index so two threads copying
// between the same two buffers in opposite directions cannot deadlock; takes the
// lock only once when both land on the same stripe (self-deadlock guard).
class StripeLock2 {
  public:
    StripeLock2(const void *a, const void *b) {
        size_t ia = (a != nullptr) ? stripe_of(a) : 0;
        size_t ib = (b != nullptr) ? stripe_of(b) : 0;
        if (a == nullptr) ia = ib;
        if (b == nullptr) ib = ia;
        if (ia > ib) {
            const size_t t = ia;
            ia = ib;
            ib = t;
        }
        first_ = &stripes()[ia];
        second_ = (ia == ib) ? nullptr : &stripes()[ib];
        first_->lock();
        if (second_ != nullptr) second_->lock();
    }
    ~StripeLock2() {
        if (second_ != nullptr) second_->unlock();
        first_->unlock();
    }
    StripeLock2(const StripeLock2 &) = delete;
    StripeLock2 &operator=(const StripeLock2 &) = delete;

  private:
    std::mutex *first_;
    std::mutex *second_;
};

// Faithful replica of upstream's debug_log_and_validate_buf
// (device_interface.cpp:87-126) minus the debug() stream, which is compiled out
// in a non-DEBUG_RUNTIME build. Pure reads of the buffer's own fields.
inline int validate_buf(void *user_context, const halide_buffer_t *buf_arg,
                        const char *routine) {
    if (buf_arg == nullptr) {
        return halide_error_buffer_is_null(user_context, routine);
    }
    const halide_buffer_t &buf(*buf_arg);
    const bool device_interface_set = (buf.device_interface != nullptr);
    const bool device_set = (buf.device != 0);
    if (device_set && !device_interface_set) {
        return halide_error_no_device_interface(user_context);
    }
    if (device_interface_set && !device_set) {
        return halide_error_device_interface_no_device(user_context);
    }
    if (buf.host_dirty() && buf.device_dirty()) {
        return halide_error_host_and_device_dirty(user_context);
    }
    return halide_error_code_success;
}

// NOTE: upstream's UseModule (device_interface.cpp:62-77) is NOT replicated
// here, and cannot be: it calls device_interface->impl->use_module(), and
// halide_device_interface_impl_t is forward-declared only in the public header
// (HalideRuntime.h:781), so a ceyx TU cannot dereference impl. I tried and the
// compiler rejected it ("member access into incomplete type"), which is the same
// wall that keeps device_crop/_slice/_release_crop unoverridable.
// This is the ONE declared deviation from upstream in this TU, and it is inert
// in this build for a checkable reason, not a hopeful one: use_module /
// release_module resolve to halide_use_jit_module / halide_release_jit_module,
// which are EMPTY in an AOT build —
//   otool -tvV -p _halide_use_jit_module build-r1t2/libdng_decoder_native.dylib
//   -> stp x29,x30 / mov x29,sp / ldp x29,x30 / ret
// If ceyx ever links the JIT runtime instead, that pair becomes a real refcount
// and this deviation stops being inert; that is why it is written down here.

}  // namespace

namespace ceyx {

int dng_copy_lock_stripes() { return static_cast<int>(kStripes); }

const char *dng_copy_lock_marker() { return "ceyx_copy_lock_v1"; }

}  // namespace ceyx

extern "C" const char *ceyx_copy_lock_v1(void) { return "ceyx_copy_lock_v1"; }

// Self-check for the one deadlock-shaped corner in this TU: two threads taking
// the SAME pair of buffers in OPPOSITE order, once for a cross-stripe pair and
// once for a pair that collides on a single stripe (where taking the lock twice
// would self-deadlock on a non-recursive std::mutex). Not wired into any build
// target; it is called by tmp/r2t2/selftest_stripe.cpp during the R2-T2 window.
// Returns 0 on success, non-zero on a checked failure; a real deadlock is caught
// by the watchdog, which exits(3) rather than hanging the harness.
extern "C" int ceyx_copy_lock_selftest(void) {
    // 1. stripe_of must be a pure function of the address.
    int x = 0, y = 0;
    if (stripe_of(&x) != stripe_of(&x)) return 1;

    // 2. Find a colliding pair by search, so the same-stripe path is genuinely
    //    exercised rather than assumed. Addresses inside one array differ.
    static char pad[1 << 16];
    const void *same_a = nullptr;
    const void *same_b = nullptr;
    for (size_t i = 0; i < sizeof(pad) && same_b == nullptr; i += 16) {
        for (size_t j = i + 16; j < sizeof(pad); j += 16) {
            if (stripe_of(&pad[i]) == stripe_of(&pad[j])) {
                same_a = &pad[i];
                same_b = &pad[j];
                break;
            }
        }
    }
    if (same_a == nullptr) return 2;  // no colliding pair found: search is broken

    const void *diff_a = &x;
    const void *diff_b = &y;
    for (size_t i = 0; i < sizeof(pad); i += 16) {
        if (stripe_of(&pad[i]) != stripe_of(diff_a)) {
            diff_b = &pad[i];
            break;
        }
    }
    if (stripe_of(diff_a) == stripe_of(diff_b)) return 3;

    std::atomic<bool> done{false};
    std::thread watchdog([&done]() {
        for (int i = 0; i < 100 && !done.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!done.load()) {
            std::fputs("ceyx_copy_lock_selftest: DEADLOCK (watchdog fired)\n", stderr);
            std::fflush(stderr);
            std::_Exit(3);
        }
    });

    std::atomic<int> counter{0};
    auto hammer = [&counter](const void *p, const void *q) {
        for (int i = 0; i < 20000; ++i) {
            StripeLock2 lock(p, q);
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    };

    {
        std::thread t1(hammer, diff_a, diff_b);
        std::thread t2(hammer, diff_b, diff_a);  // opposite order: ordering test
        t1.join();
        t2.join();
    }
    {
        std::thread t1(hammer, same_a, same_b);
        std::thread t2(hammer, same_b, same_a);  // same stripe: single-take test
        t1.join();
        t2.join();
    }
    // 3. Single-buffer path, and the null-src shape halide_buffer_copy can pass.
    {
        std::thread t1([&]() { for (int i = 0; i < 20000; ++i) { StripeLock l(diff_a); counter.fetch_add(1); } });
        std::thread t2([&]() { for (int i = 0; i < 20000; ++i) { StripeLock2 l(nullptr, diff_a); counter.fetch_add(1); } });
        t1.join();
        t2.join();
    }

    done.store(true);
    watchdog.join();
    return (counter.load() == 120000) ? 0 : 4;
}

extern "C" {

// Upstream: device_interface.cpp:141-150.
int halide_copy_to_host(void *user_context, struct halide_buffer_t *buf) {
    StripeLock lock(buf);
    const int result = validate_buf(user_context, buf, "halide_copy_to_host");
    if (result) {
        return result;
    }
    return Halide::Runtime::Internal::copy_to_host_already_locked(user_context, buf);
}

// Upstream: device_interface.cpp:207-212.
int halide_copy_to_device(void *user_context, struct halide_buffer_t *buf,
                          const struct halide_device_interface_t *device_interface) {
    StripeLock lock(buf);
    return copy_to_device_already_locked(user_context, buf, device_interface);
}

// Upstream: device_interface.cpp:611-625. Two buffers, so two stripes.
int halide_buffer_copy(void *user_context, struct halide_buffer_t *src,
                       const struct halide_device_interface_t *dst_device_interface,
                       struct halide_buffer_t *dst) {
    // Upstream brackets this with UseModule(src->device_interface) and
    // UseModule(dst_device_interface); see the note above for why that is
    // omitted and why it is inert in an AOT build.
    StripeLock2 lock(src, dst);
    return halide_buffer_copy_already_locked(user_context, src, dst_device_interface,
                                             dst);
}

}  // extern "C"

#endif  // __APPLE__ && !DNG_FORCE_VULKAN && !CEYX_COPY_LOCK_DISABLED
