// Task 8 / gate G-C -- Android REAL-HARDWARE re-gate of the PRODUCTION split
// kernel (dng_render_stage4_split) after T7 replaced the 8-way select chain
// with branch-free flag arithmetic (commit a396f9b).
//
// Derived from test_orient_vk_device.cpp (Task 6). Two substantive changes:
//   * the kernels are the PRODUCTION generator's, not the V3 experimental copy;
//   * invoke() follows the production argument order -- crop_l/crop_t come
//     BEFORE src_scale and orientation/unoriented_* come AFTER it, the reverse
//     of the V3 layout. Getting this wrong would silently shift every scalar.
//
// The Task 6 findings this harness carries forward: the HSV tables must be
// neutralised or the render is black and every byte-compare is vacuous
// (F-T6-2), and guard_tail is not a red-state control on this backend
// (F-T6-3) -- the red state comes from the permutation mutation control.
//
// ORIGINAL TASK 6 HEADER FOLLOWS.
// Task 6 / gate G-B -- Android REAL-HARDWARE sub-tile GuardWithIf byte-compare.
//
// Runs entirely on the device. Three AOT pipelines are linked in:
//   k_ref_cpu   (arm-64-android, asserts+bounds-query ON)  -- oracle producer
//   k_vk_gt_on  (arm-64-android-vulkan..., guard_tail=true)
//   k_vk_gt_off (arm-64-android-vulkan..., guard_tail=false)
//
// The oracle is EXTERNAL to the fused kernel: unoriented CPU render at
// orientation=1, then the production CPU pass ceyx_orient_rgba(o). The fused
// Vulkan output is byte-compared against that. This is the same comparison
// v3_cpu_ref/v3_vk_run made on the host, moved onto hardware where it is
// admissible (G-10).
//
// Evidence legs (G-9):
//   (a) sub-16 cases -- 8x4 and 12x7 have BOTH dimensions inside one 16x16 tile.
//   (b) three identical runs -- driven by run_device_gate.sh into ONE artifact.
//   (c) device-execution proof -- halide_vulkan_device_interface() pointer is
//       printed, and every dst buffer is asserted to carry a non-null device
//       allocation on THAT interface before copy_to_host().
//   (d) red-state control -- guard_tail=off is expected to mismatch/crash, and
//       a MUTATION_CONTROL deliberately corrupts one oracle byte to prove the
//       comparator can report a failure.

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "v3_common.h"
#include "k_vk_gt_on.h"   // production dng_render_stage4_split, guard_tail=true
#include "k_vk_gt_off.h"  // production dng_render_stage4_split, guard_tail=false
#include "k_ref_cpu.h"    // same generator, CPU target -- layer-isolation control
#include "ceyx_orient.h"
#include "HalideRuntime.h"

extern "C" const halide_device_interface_t *halide_vulkan_device_interface();

using namespace v3;

namespace {

// Halide's DEFAULT error handler calls abort(). That makes every "does this
// allocation fail?" probe unmeasurable: the first failure kills the process
// before it can be recorded (observed as SIGABRT with no stderr, attempt 1).
// A handler that RETURNS lets the runtime propagate an error code instead.
void gate_error_handler(void * /*user_context*/, const char *msg) {
    printf("HALIDE_ERROR: %s\n", msg ? msg : "(null)");
}

// T8b PRODUCTION prefix (affine form, commit bac3cbe):
//   src_rgb, src_width, src_height, src_row_stride_px,
//   crop_l, crop_t, src_scale,
//   orient_a_x, orient_b_x, orient_c_x, orient_a_y, orient_b_y, orient_c_y, ...
// The T8 (flag) prefix had THREE int32 there (orientation, unoriented_width,
// unoriented_height); the V3 experimental prefix had the same three but BEFORE
// crop_l/crop_t. Both differ from this one, and the arity differs from T8's, so
// this typedef turns any stale copy-paste into a compile error rather than a
// silent scalar shift (Task_t8_android_device_gate.md "Harness note").
using KernelFn = int (*)(halide_buffer_t *, int32_t, int32_t, int32_t, int32_t,
                         int32_t, float,
                         int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                         halide_buffer_t *, halide_buffer_t *, halide_buffer_t *,
                         halide_buffer_t *, halide_buffer_t *, halide_buffer_t *,
                         halide_buffer_t *, halide_buffer_t *, halide_buffer_t *,
                         int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                         halide_buffer_t *, halide_buffer_t *, halide_buffer_t *,
                         int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                         halide_buffer_t *);

// PRODUCTION argument order (DngRenderStage4Android, T8b affine form):
//   src_rgb, src_width, src_height, src_row_stride_px,
//   crop_l, crop_t, src_scale,
//   orient_a_x, orient_b_x, orient_c_x, orient_a_y, orient_b_y, orient_c_y, ...
// The kernel no longer receives an orientation value or the unoriented extents:
// the whole decision is the host-side ceyx_orient_affine_coeffs() call below,
// and the extents are folded into the c terms. Every call site in this harness
// still passes the EXIF orientation `o`; the conversion happens exactly here, so
// the harness continues to test "ask for o, get oracle(o)".
int invoke(KernelFn fn, Buffer<uint16_t> &src, int w, int h, int stride, int o,
           Params &p, Buffer<uint8_t> &dst) {
    int32_t k[6];
    ceyx_orient_affine_coeffs(o, /*uw=*/w, /*uh=*/h, k);
    return fn(src, w, h, stride, /*crop_l=*/0, /*crop_t=*/0,
              /*src_scale=*/1.0f / 65535.0f,
              /*orient_a_x=*/k[0], /*orient_b_x=*/k[1], /*orient_c_x=*/k[2],
              /*orient_a_y=*/k[3], /*orient_b_y=*/k[4], /*orient_c_y=*/k[5],
              p.exp_ramp, p.tone_curve,
              p.encode_gamma, p.camera_white, p.camera_to_rgb, p.rgb_to_final,
              p.huesat_table, p.huesat_encode, p.huesat_decode,
              /*huesat_entry_count=*/1, 1, 2, 2, 0, 0, p.look_table,
              p.look_encode, p.look_decode,
              /*look_entry_count=*/1, 1, 2, 2, 0, 0, dst);
}

struct HwCase {
    const char *name;
    int w, h;
};

// The ORACLE kernel is the CPU branch of the same generator, whose schedule is
// split(y, yo, yi, 32).parallel(yo).vectorize(x, 8) with NO tail strategy --
// finding F-V3-3. At w<8 or h<32 that reads before the buffer min and the
// pipeline returns -4 ("accessed at -28, which is before the min (0)"). This
// is a property of the PRODUCTION CPU schedule, not of the orientation fusion,
// and it is exactly why the sub-16 leg needs a padded oracle rather than a
// direct call.
//
// The pipeline at orientation == 1 is pointwise: dst(x,y) depends only on
// src(x,y), and make_src is a pure function of (x,y) independent of the row
// stride. So computing the baseline at (roundup(w,8), roundup(h,32)) and
// taking the top-left w*h sub-rectangle yields byte-identical pixels to an
// unpadded run, while keeping every index inside the buffer.
bool oracle_unoriented(int w, int h, Params &p, std::vector<uint8_t> &out) {
    auto roundup = [](int v, int m) { return ((v + m - 1) / m) * m; };
    const int W = roundup(w, 8), H = roundup(h, 32);
    Buffer<uint16_t> src = make_src(W, H, W);
    Buffer<uint8_t> pad = make_dst(W, H);
    memset(pad.data(), 0xAB, (size_t)W * H * 4);
    if (invoke(k_ref_cpu, src, W, H, W, /*orientation=*/1, p, pad) != 0)
        return false;
    out.resize((size_t)w * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c)
                out[((size_t)y * w + x) * 4 + c] = pad(x, y, c);
    return true;
}

// Sub-16 leg (G-9a): 8x4 and 12x7 are entirely inside one 16x16 tile, so the
// whole dispatch IS tail. 24x20 / 33x17 leave a partial tail beside a full
// tile. 64x32 is an exact tile multiple and is the negative control: it must
// pass with guard_tail OFF too, otherwise the harness is measuring something
// other than the tail.
// HW_CASES="WxH,WxH,..." overrides the list; every size tried is recorded in
// the artifact (no discarded attempts, prediction file rule F1).
std::vector<HwCase> hw_cases() {
    static std::vector<std::string> names;
    std::vector<HwCase> v;
    const char *env = getenv("HW_CASES");
    if (!env) {
        return {{"subtile_8x4", 8, 4},   {"subtile_12x7", 12, 7},
                {"tail_24x20", 24, 20},  {"tail_33x17", 33, 17},
                {"exact_64x32", 64, 32}};
    }
    names.clear();
    names.reserve(64);
    std::string s(env), tok;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t c = s.find(',', pos);
        if (c == std::string::npos) c = s.size();
        tok = s.substr(pos, c - pos);
        int w = 0, h = 0;
        if (sscanf(tok.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            names.push_back("case_" + tok);
            v.push_back({nullptr, w, h});
        }
        pos = c + 1;
    }
    for (size_t i = 0; i < v.size(); ++i) v[i].name = names[i].c_str();
    return v;
}

// ---------------------------------------------------------------------------
// Raw Vulkan probe: device name + the allocation-alignment limits that V1's
// "w*h % 64 == 0" rule was derived from on MoltenVK.
// ---------------------------------------------------------------------------
void probe_vulkan_limits() {
    void *lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("VK_DEVICE_NAME=<dlopen-failed>\n");
        printf("VK_ALLOC_GRANULARITY=-1\n");
        return;
    }
    auto getproc = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    auto vkCreateInstance =
        (PFN_vkCreateInstance)dlsym(lib, "vkCreateInstance");
    if (!getproc || !vkCreateInstance) {
        printf("VK_DEVICE_NAME=<no-proc-addr>\n");
        printf("VK_ALLOC_GRANULARITY=-1\n");
        return;
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        printf("VK_DEVICE_NAME=<instance-create-failed>\n");
        printf("VK_ALLOC_GRANULARITY=-1\n");
        return;
    }
    auto enumerate = (PFN_vkEnumeratePhysicalDevices)getproc(
        inst, "vkEnumeratePhysicalDevices");
    auto getprops = (PFN_vkGetPhysicalDeviceProperties)getproc(
        inst, "vkGetPhysicalDeviceProperties");
    uint32_t n = 0;
    enumerate(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) enumerate(inst, &n, devs.data());
    if (!n) {
        printf("VK_DEVICE_NAME=<no-physical-device>\n");
        printf("VK_ALLOC_GRANULARITY=-1\n");
        return;
    }
    VkPhysicalDeviceProperties pr{};
    getprops(devs[0], &pr);
    printf("VK_DEVICE_NAME=%s\n", pr.deviceName);
    printf("VK_DEVICE_API=%u.%u.%u\n", VK_VERSION_MAJOR(pr.apiVersion),
           VK_VERSION_MINOR(pr.apiVersion), VK_VERSION_PATCH(pr.apiVersion));
    printf("VK_LIMIT_nonCoherentAtomSize=%llu\n",
           (unsigned long long)pr.limits.nonCoherentAtomSize);
    printf("VK_LIMIT_bufferImageGranularity=%llu\n",
           (unsigned long long)pr.limits.bufferImageGranularity);
    printf("VK_LIMIT_minStorageBufferOffsetAlignment=%llu\n",
           (unsigned long long)pr.limits.minStorageBufferOffsetAlignment);
    printf("VK_LIMIT_minMemoryMapAlignment=%llu\n",
           (unsigned long long)pr.limits.minMemoryMapAlignment);
    printf("VK_ALLOC_GRANULARITY=%llu\n",
           (unsigned long long)pr.limits.nonCoherentAtomSize);
}

// Empirical counterpart to the reported limit: allocate uint8 device buffers
// of every span 1..320 bytes and report which fail. V1's MoltenVK-derived rule
// was "w*h % 64 == 0"; this measures whether that rule holds here.
void probe_alloc_rule(const halide_device_interface_t *vk) {
    int first_fail = -1, fails = 0;
    int fail_not_mult64 = 0, fail_mult64 = 0, total_not_mult64 = 0;
    for (int n = 1; n <= 320; ++n) {
        Buffer<uint8_t> b(n);
        memset(b.data(), 0, (size_t)n);
        b.set_host_dirty();
        // Use the C runtime entry, NOT Buffer::device_malloc(): the latter
        // asserts (SIGABRT, message "..._device...") on a non-zero return, so
        // it cannot be used to *measure* which spans fail. Measured first
        // attempt aborted here; the C entry returns the code instead.
        int rc = halide_device_malloc(nullptr, b.raw_buffer(), vk);
        if (rc == 0) halide_device_free(nullptr, b.raw_buffer());
        bool is64 = (n % 64) == 0;
        if (!is64) ++total_not_mult64;
        if (rc != 0) {
            ++fails;
            if (first_fail < 0) first_fail = n;
            if (is64) ++fail_mult64; else ++fail_not_mult64;
        }
    }
    printf("ALLOC_SWEEP spans=1..320 failures=%d first_failing_span=%d "
           "failures_when_span_mod64_nonzero=%d/%d failures_when_span_mod64_zero=%d\n",
           fails, first_fail, fail_not_mult64, total_not_mult64, fail_mult64);
    if (fails == 0) {
        printf("ALLOC_RULE_VERDICT=looser  "
               "(MoltenVK-derived w*h%%64==0 rule does NOT hold here: every span "
               "1..320 allocated successfully, so the device imposes no such "
               "constraint at the Halide allocation layer)\n");
    } else if (fail_not_mult64 == total_not_mult64 && fail_mult64 == 0) {
        printf("ALLOC_RULE_VERDICT=holds  "
               "(exactly the non-multiple-of-64 spans failed)\n");
    } else {
        printf("ALLOC_RULE_VERDICT=stricter_or_other  "
               "(failure pattern does not match w*h%%64==0)\n");
    }
}

struct Tally {
    int checks = 0, mismatched_cases = 0, kernel_failures = 0, device_proofs = 0;
    // Cross-implementation (GPU vs CPU-oracle) leg, reported separately so a
    // float-precision difference cannot be mistaken for a permutation defect.
    int cpu_oracle_mismatched = 0, cpu_oracle_max_abs = 0;
};

// Runs every case/orientation for one kernel variant and prints one
// DEVICE_GATE line per case+orientation.
void run_variant(const char *label, KernelFn fn, Params &p,
                 const halide_device_interface_t *vk, Tally &t) {
    for (const HwCase &cs : hw_cases()) {
        const int w = cs.w, h = cs.h, stride = cs.w;
        Buffer<uint16_t> src = make_src(w, h, stride);

        // Oracle: unoriented CPU render once per case (padded, see above).
        std::vector<uint8_t> un;
        if (!oracle_unoriented(w, h, p, un)) {
            printf("DEVICE_GATE guard_tail=%s subtile=%dx%d ORACLE_FAILED\n",
                   label, w, h);
            ++t.kernel_failures;
            continue;
        }
        int rc = 0;

        for (int o = 1; o <= 8; ++o) {
            const int ow = transposes(o) ? h : w;
            const int oh = transposes(o) ? w : h;

            std::vector<uint8_t> oracle((size_t)ow * oh * 4);
            int32_t rw = 0, rh = 0;
            int32_t orc = ceyx_orient_rgba(un.data(), oracle.data(),
                                           oracle.size(), w, h, o, &rw, &rh);
            if (orc != 0 || rw != ow || rh != oh) {
                printf("DEVICE_GATE guard_tail=%s subtile=%dx%d o=%d "
                       "ORACLE_ORIENT_RC=%d dims=%dx%d\n",
                       label, w, h, o, orc, rw, rh);
                ++t.kernel_failures;
                continue;
            }

            Buffer<uint8_t> dst = make_dst(ow, oh);
            // G-6 poison: a no-op kernel must not pass by leaving a value that
            // happens to match.
            memset(dst.data(), 0xAB, (size_t)ow * oh * 4);
            dst.set_host_dirty();
            src.set_host_dirty();
            p.set_host_dirty();

            rc = invoke(fn, src, w, h, stride, o, p, dst);
            if (rc != 0) {
                printf("DEVICE_GATE guard_tail=%s subtile=%dx%d o=%d "
                       "KERNEL_RC=%d mismatched_bytes=-1 CRASH_OR_ERROR\n",
                       label, w, h, o, rc);
                ++t.kernel_failures;
                continue;
            }

            // (c) device-execution proof.
            const bool on_device = dst.raw_buffer()->device != 0 &&
                                   dst.raw_buffer()->device_interface == vk;
            if (on_device) ++t.device_proofs;

            // G-7: check copy_to_host()'s return code.
            int crc = dst.copy_to_host();

            size_t n = oracle.size(), bad = 0;
            int max_abs = 0;
            for (size_t i = 0; i < n; ++i) {
                int d = (int)dst.data()[i] - (int)oracle[i];
                if (d) {
                    ++bad;
                    if (d < 0) d = -d;
                    if (d > max_abs) max_abs = d;
                }
            }
            // Same-device composed reference: render UNORIENTED on the GPU,
            // then apply the production CPU permutation. Precision differences
            // between the Adreno and the ARM CPU cancel here, so this leg
            // isolates the PERMUTATION claim; the CPU-oracle leg above is a
            // stricter cross-implementation check that a GPU/CPU float
            // discrepancy alone can trip.
            size_t gpu_bad = 0;
            int gpu_max_abs = 0;
            {
                Buffer<uint8_t> gun = make_dst(w, h);
                memset(gun.data(), 0xAB, (size_t)w * h * 4);
                gun.set_host_dirty();
                src.set_host_dirty();
                p.set_host_dirty();
                if (invoke(fn, src, w, h, stride, 1, p, gun) == 0) {
                    gun.copy_to_host();
                    std::vector<uint8_t> gref(n);
                    int32_t grw = 0, grh = 0;
                    if (ceyx_orient_rgba(gun.data(), gref.data(), gref.size(), w,
                                         h, o, &grw, &grh) == 0) {
                        for (size_t i = 0; i < n; ++i) {
                            int d = (int)dst.data()[i] - (int)gref[i];
                            if (d) {
                                ++gpu_bad;
                                if (d < 0) d = -d;
                                if (d > gpu_max_abs) gpu_max_abs = d;
                            }
                        }
                    }
                }
            }
            ++t.checks;
            if (gpu_bad) ++t.mismatched_cases;
            if (bad) ++t.cpu_oracle_mismatched;
            if (max_abs > t.cpu_oracle_max_abs) t.cpu_oracle_max_abs = max_abs;
            printf("DEVICE_GATE guard_tail=%s subtile=%dx%d o=%d dims=%dx%d "
                   "bytes=%zu mismatched_bytes=%zu max_abs_diff=%d "
                   "vs_cpu_oracle_mismatched=%zu vs_cpu_oracle_max_abs=%d "
                   "device=%d copy_rc=%d %s\n",
                   label, w, h, o, ow, oh, n, gpu_bad, gpu_max_abs, bad, max_abs,
                   on_device ? 1 : 0, crc, gpu_bad ? "MISMATCH" : "MATCH");
        }
    }
}

// (d) mutation control: prove the byte comparator above can report a failure.
// Same oracle, same kernel, but one oracle byte is flipped before comparing.
void mutation_control(Params &p, const halide_device_interface_t *vk) {
    const int w = 12, h = 7, stride = 12, o = 6;
    const int ow = h, oh = w;
    Buffer<uint16_t> src = make_src(w, h, stride);
    std::vector<uint8_t> un;
    if (!oracle_unoriented(w, h, p, un)) {
        printf("MUTATION_CONTROL result=ERROR reason=oracle_kernel\n");
        return;
    }
    std::vector<uint8_t> oracle((size_t)ow * oh * 4);
    int32_t rw = 0, rh = 0;
    if (ceyx_orient_rgba(un.data(), oracle.data(), oracle.size(), w, h, o, &rw,
                         &rh) != 0) {
        printf("MUTATION_CONTROL result=ERROR reason=oracle_orient\n");
        return;
    }
    Buffer<uint8_t> dst = make_dst(ow, oh);
    memset(dst.data(), 0xAB, (size_t)ow * oh * 4);
    dst.set_host_dirty();
    src.set_host_dirty();
    p.set_host_dirty();
    if (invoke(k_vk_gt_on, src, w, h, stride, o, p, dst) != 0) {
        printf("MUTATION_CONTROL result=ERROR reason=vk_kernel\n");
        return;
    }
    (void)vk;
    dst.copy_to_host();

    size_t clean = 0;
    for (size_t i = 0; i < oracle.size(); ++i)
        if (dst.data()[i] != oracle[i]) ++clean;

    // Flip exactly one byte of the oracle.
    const size_t idx = oracle.size() / 2;
    oracle[idx] = (uint8_t)(oracle[idx] ^ 0xFF);
    size_t mutated = 0;
    for (size_t i = 0; i < oracle.size(); ++i)
        if (dst.data()[i] != oracle[i]) ++mutated;

    printf("MUTATION_CONTROL clean_mismatched_bytes=%zu "
           "after_1byte_flip_mismatched_bytes=%zu %s\n",
           clean, mutated,
           (clean == 0 && mutated == 1) ? "COMPARATOR_CAN_FAIL"
                                        : "COMPARATOR_CONTROL_INCONCLUSIVE");
}

// ---------------------------------------------------------------------------
// BLIND-INSTRUMENT FIX + GATE.
//
// v3_common.h's Params fills huesat_table and look_table with 0.0f. Those are
// HSV maps of (hue_shift, sat_scale, val_scale) triples, so an all-zero table
// forces val_scale = 0 and the render collapses to RGB(0,0,0), A=255. Measured:
// native/experiments/vk_orient/v3/build/refs/ref_large_64x32_o1.bin has TWO
// distinct byte values and is byte-IDENTICAL to ..._o2.bin -- i.e. the V3
// byte-compare was comparing black images to black images and could not have
// detected a wrong permutation. Neutralise the tables here.
void neutralize_hsv_tables(Params &p) {
    p.huesat_table(0) = 0.0f;  // hue shift
    p.huesat_table(1) = 1.0f;  // saturation scale
    p.huesat_table(2) = 1.0f;  // value scale
    p.look_table(0) = 0.0f;
    p.look_table(1) = 1.0f;
    p.look_table(2) = 1.0f;
}

// Refuses to let a green byte-compare mean anything unless the oracle image is
// actually orientation-sensitive. Two mechanical checks on the CPU oracle:
//   1. the unoriented image has more than a handful of distinct byte values;
//   2. oracle(o) differs from oracle(1) for every o in 2..8.
// If either fails, every MATCH in this run is vacuous.
bool content_sensitivity_gate(Params &p) {
    const int w = 12, h = 7;
    std::vector<uint8_t> un;
    if (!oracle_unoriented(w, h, p, un)) {
        printf("CONTENT_SENSITIVITY result=ERROR reason=oracle\n");
        return false;
    }
    bool seen[256] = {false};
    int distinct = 0;
    for (uint8_t b : un)
        if (!seen[b]) { seen[b] = true; ++distinct; }

    int differing = 0;
    std::vector<uint8_t> base((size_t)w * h * 4);
    int32_t rw = 0, rh = 0;
    ceyx_orient_rgba(un.data(), base.data(), base.size(), w, h, 1, &rw, &rh);
    for (int o = 2; o <= 8; ++o) {
        const int ow = transposes(o) ? h : w, oh = transposes(o) ? w : h;
        std::vector<uint8_t> ref((size_t)ow * oh * 4);
        if (ceyx_orient_rgba(un.data(), ref.data(), ref.size(), w, h, o, &rw,
                             &rh) != 0)
            continue;
        bool diff = ref.size() != base.size() ||
                    memcmp(ref.data(), base.data(), ref.size()) != 0;
        if (diff) ++differing;
    }
    const bool ok = distinct > 8 && differing == 7;
    printf("CONTENT_SENSITIVITY distinct_byte_values=%d "
           "orientations_differing_from_o1=%d/7 %s\n",
           distinct, differing,
           ok ? "IMAGE_IS_ORIENTATION_SENSITIVE"
              : "BLIND_INSTRUMENT_EVERY_MATCH_IS_VACUOUS");
    return ok;
}

// (d, kernel leg) Permutation mutation control. The MUTATION_CONTROL above
// only proves the byte comparator can fire. This proves the whole chain
// (device kernel -> readback -> oracle) is sensitive to the thing under test:
// the fused kernel is asked for the WRONG orientation and the comparison
// against the oracle for the requested orientation must go red. Orientation
// pairs are chosen to preserve dst extents, so a mismatch is caused by the
// permutation and not by a size error.
void permutation_mutation_control(Params &p) {
    struct Pair { int requested, wrong; };
    // 2<->4 and 3<->1 keep (w,h); 5<->7 and 6<->8 keep (h,w).
    const Pair pairs[] = {{2, 4}, {3, 1}, {5, 7}, {6, 8}};
    const int w = 12, h = 7, stride = 12;
    std::vector<uint8_t> un;
    if (!oracle_unoriented(w, h, p, un)) {
        printf("PERMUTATION_MUTATION result=ERROR reason=oracle\n");
        return;
    }
    Buffer<uint16_t> src = make_src(w, h, stride);
    int red = 0, total = 0;
    for (const Pair &pr : pairs) {
        const int ow = transposes(pr.requested) ? h : w;
        const int oh = transposes(pr.requested) ? w : h;
        std::vector<uint8_t> oracle((size_t)ow * oh * 4);
        int32_t rw = 0, rh = 0;
        if (ceyx_orient_rgba(un.data(), oracle.data(), oracle.size(), w, h,
                             pr.requested, &rw, &rh) != 0)
            continue;
        Buffer<uint8_t> dst = make_dst(ow, oh);
        memset(dst.data(), 0xAB, (size_t)ow * oh * 4);
        dst.set_host_dirty();
        src.set_host_dirty();
        p.set_host_dirty();
        if (invoke(k_vk_gt_on, src, w, h, stride, pr.wrong, p, dst) != 0)
            continue;
        dst.copy_to_host();
        size_t bad = 0;
        for (size_t i = 0; i < oracle.size(); ++i)
            if (dst.data()[i] != oracle[i]) ++bad;
        ++total;
        if (bad) ++red;
        printf("PERMUTATION_MUTATION requested=%d kernel_given=%d "
               "mismatched_bytes=%zu %s\n",
               pr.requested, pr.wrong, bad, bad ? "RED_AS_EXPECTED" : "GREEN_UNEXPECTED");
    }
    printf("PERMUTATION_MUTATION_SUMMARY red=%d/%d %s\n", red, total,
           (total > 0 && red == total) ? "KERNEL_PATH_CAN_FAIL"
                                       : "KERNEL_CONTROL_INCONCLUSIVE");
}

// Layer-isolation control. Runs the SAME fused permutation through the CPU
// target of the SAME generator (k_ref_cpu, orientation = o) and compares
// against the same ceyx_orient_rgba oracle. If a given orientation fails here
// too, the defect is in the generator's permutation text and is neither
// Vulkan-specific, device-specific, nor a tail/precision artefact. Size is a
// multiple of 8 and 32 so the CPU schedule's untailed split/vectorize
// (F-V3-3) is not in play.
void cpu_fused_control(Params &p) {
    // 32x32: square AND a multiple of both 8 (vectorize) and 32 (split), so
    // every orientation -- including the transposing ones -- has a dst extent
    // the untailed CPU schedule can handle. At 24x32 the transposed dst is
    // 32x24 and h'=24 tripped F-V3-3, leaving o=5..8 untested.
    const int w = 32, h = 32, stride = 32;
    std::vector<uint8_t> un;
    if (!oracle_unoriented(w, h, p, un)) {
        printf("CPU_FUSED_CONTROL result=ERROR\n");
        return;
    }
    Buffer<uint16_t> src = make_src(w, h, stride);
    for (int o = 1; o <= 8; ++o) {
        const int ow = transposes(o) ? h : w, oh = transposes(o) ? w : h;
        std::vector<uint8_t> oracle((size_t)ow * oh * 4);
        int32_t rw = 0, rh = 0;
        if (ceyx_orient_rgba(un.data(), oracle.data(), oracle.size(), w, h, o,
                             &rw, &rh) != 0)
            continue;
        Buffer<uint8_t> dst = make_dst(ow, oh);
        memset(dst.data(), 0xAB, oracle.size());
        if (invoke(k_ref_cpu, src, w, h, stride, o, p, dst) != 0) {
            printf("CPU_FUSED_CONTROL o=%d KERNEL_RC!=0\n", o);
            continue;
        }
        size_t bad = 0;
        int max_abs = 0;
        for (size_t i = 0; i < oracle.size(); ++i) {
            int d = (int)dst.data()[i] - (int)oracle[i];
            if (d) { ++bad; if (d < 0) d = -d; if (d > max_abs) max_abs = d; }
        }
        printf("CPU_FUSED_CONTROL o=%d dims=%dx%d mismatched_bytes=%zu "
               "max_abs_diff=%d %s\n",
               o, ow, oh, bad, max_abs, bad ? "MISMATCH" : "MATCH");
    }
}

// For every orientation, ask the Vulkan kernel for o and then identify WHICH
// oracle orientation (if any) its output actually equals. A failing case that
// equals oracle(k) names the defect exactly ("kernel produced k when asked for
// o"); one that equals nothing is a corruption rather than a mislabelling.
// 32x32 keeps every dst extent square, so extent bookkeeping cannot explain a
// difference and every candidate is byte-comparable.
void identify_actual_permutation(Params &p) {
    const int w = 32, h = 32, stride = 32;
    std::vector<uint8_t> un;
    if (!oracle_unoriented(w, h, p, un)) {
        printf("IDENTIFY result=ERROR\n");
        return;
    }
    std::vector<std::vector<uint8_t>> oracles(9);
    for (int k = 1; k <= 8; ++k) {
        oracles[k].resize((size_t)w * h * 4);
        int32_t rw = 0, rh = 0;
        ceyx_orient_rgba(un.data(), oracles[k].data(), oracles[k].size(), w, h,
                         k, &rw, &rh);
    }
    Buffer<uint16_t> src = make_src(w, h, stride);
    for (int o = 1; o <= 8; ++o) {
        Buffer<uint8_t> dst = make_dst(w, h);
        memset(dst.data(), 0xAB, (size_t)w * h * 4);
        dst.set_host_dirty();
        src.set_host_dirty();
        p.set_host_dirty();
        if (invoke(k_vk_gt_on, src, w, h, stride, o, p, dst) != 0) continue;
        dst.copy_to_host();
        int equals = 0;
        int best_k = -1;
        size_t best_bad = (size_t)-1;
        for (int k = 1; k <= 8; ++k) {
            size_t bad = 0;
            for (size_t i = 0; i < oracles[k].size(); ++i)
                if (dst.data()[i] != oracles[k][i]) ++bad;
            if (bad == 0) equals = k;
            if (bad < best_bad) { best_bad = bad; best_k = k; }
        }
        printf("IDENTIFY asked_for=%d equals_oracle_orientation=%d "
               "closest_oracle=%d closest_mismatched_bytes=%zu\n",
               o, equals, best_k, best_bad);
    }
}

}  // namespace

// Host-side dump of the six affine coefficients the kernel will receive, for
// every orientation at the 32x32 IDENTIFY size. This is the ENTIRE orientation
// decision under the T7b design, so recording it makes any future red result
// separable into "host table wrong" vs "kernel mis-lowered the multiply-add"
// without re-running anything.
static void dump_affine_coeffs(int uw, int uh) {
    for (int o = 1; o <= 8; ++o) {
        int32_t k[6];
        ceyx_orient_affine_coeffs(o, uw, uh, k);
        printf("AFFINE_COEFFS uw=%d uh=%d o=%d a_x=%d b_x=%d c_x=%d "
               "a_y=%d b_y=%d c_y=%d\n",
               uw, uh, o, k[0], k[1], k[2], k[3], k[4], k[5]);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    halide_set_error_handler(gate_error_handler);
    printf("GATE=G-C TASK=8b FORMULATION=affine_host_coeffs "
           "KERNEL=dng_render_stage4_split(production)\n");
    dump_affine_coeffs(32, 32);
    dump_affine_coeffs(12, 7);

    const halide_device_interface_t *vk = halide_vulkan_device_interface();
    printf("VULKAN_DEVICE_INTERFACE=%p\n", (const void *)vk);
    if (!vk) {
        printf("VERDICT=FAIL reason=no_vulkan_device_interface\n");
        return 3;
    }

    probe_vulkan_limits();
    probe_alloc_rule(vk);

    Params p;
    neutralize_hsv_tables(p);
    const bool sensitive = content_sensitivity_gate(p);

    Tally on, off;
    run_variant("on", k_vk_gt_on, p, vk, on);
    run_variant("off", k_vk_gt_off, p, vk, off);
    mutation_control(p, vk);
    permutation_mutation_control(p);
    cpu_fused_control(p);
    identify_actual_permutation(p);

    printf("SUMMARY_ON checks=%d mismatched_cases=%d kernel_failures=%d "
           "device_proofs=%d\n",
           on.checks, on.mismatched_cases, on.kernel_failures, on.device_proofs);
    printf("SUMMARY_ON_VS_CPU_ORACLE mismatched_cases=%d max_abs_diff=%d\n",
           on.cpu_oracle_mismatched, on.cpu_oracle_max_abs);
    printf("SUMMARY_OFF_VS_CPU_ORACLE mismatched_cases=%d max_abs_diff=%d\n",
           off.cpu_oracle_mismatched, off.cpu_oracle_max_abs);
    printf("SUMMARY_OFF checks=%d mismatched_cases=%d kernel_failures=%d "
           "device_proofs=%d\n",
           off.checks, off.mismatched_cases, off.kernel_failures,
           off.device_proofs);

    // Gate: guard_tail=on must be perfect; guard_tail=off must go RED
    // somewhere, otherwise the chosen sizes are not exercising the tail.
    const bool on_green =
        on.checks > 0 && on.mismatched_cases == 0 && on.kernel_failures == 0 &&
        on.device_proofs == on.checks;
    const bool off_red = off.mismatched_cases > 0 || off.kernel_failures > 0;
    printf("ON_GREEN=%d OFF_RED=%d CONTENT_SENSITIVE=%d\n", on_green ? 1 : 0,
           off_red ? 1 : 0, sensitive ? 1 : 0);
    printf("VERDICT=%s\n",
           (on_green && off_red && sensitive) ? "PASS" : "FAIL");
    return (on_green && off_red && sensitive) ? 0 : 1;
}
