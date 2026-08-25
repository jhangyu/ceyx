#ifndef DNG_VK_PIPELINE_CACHE_PERSIST_H
#define DNG_VK_PIPELINE_CACHE_PERSIST_H

// =============================================================================
// R3-3 (2026-07-05): VkPipelineCache cross-launch persistence.
//
// NEW FILE — NOT part of upstream Halide v21.0.0. This header contains ALL of
// the pipeline-cache persistence logic added by the runtime fork, so that the
// patched copies of upstream files (vulkan.cpp / vulkan_resources.h in this
// directory) carry a minimal diff vs third-party sources in ./upstream/.
//
// Include contract: must be included from the forked vulkan_resources.h AFTER
// "vulkan_memory.h" (needs VulkanMemoryAllocator, the vk* function-pointer
// table from vulkan_interface.h, and runtime_internal.h primitives).
//
// Red lines honored here:
//   * Cache I/O failure on ANY path must never fail a decode — every failure
//     degrades to the upstream VK_NULL_HANDLE behavior, silently (verbose log
//     only when DNG_PIPELINE_VERBOSE=1).
//   * Runtime switch: feature is inert unless a cache path is provided via
//     dng_vk_pipeline_cache_set_path() (FFI) or DNG_VK_PIPELINE_CACHE_PATH
//     env var; DNG_VK_PIPELINE_CACHE=0 force-disables regardless of path.
// =============================================================================

namespace Halide {
namespace Runtime {
namespace Internal {
namespace Vulkan {

// --- libc declarations not provided by runtime_internal.h -------------------
// (runtime_internal.h already declares fclose/fwrite/remove/getenv/malloc/free;
// fopen is intentionally bottlenecked there behind halide_fopen, which is a
// WEAK_INLINE bitcode symbol we cannot rely on from a plain C++ TU, so we
// declare bionic's fopen/fread/rename directly. Android-only code path.)
extern "C" void *fopen(const char *path, const char *mode);
extern "C" size_t fread(void *ptr, size_t size, size_t nmemb, void *stream);
extern "C" int rename(const char *oldpath, const char *newpath);

// --- persisted file envelope -------------------------------------------------
// Guards against: foreign/corrupted files (magic+version), driver or GPU
// change (vendor/device/driverVersion/pipelineCacheUUID), truncation or bit
// rot (payload_size + FNV-1a 64 hash). Any mismatch => start with empty cache.
struct DngVkpcFileHeader {
    uint8_t magic[8];                 // "DNGVKPC1"
    uint32_t header_version;          // 1
    uint32_t vendor_id;               // VkPhysicalDeviceProperties::vendorID
    uint32_t device_id;               // ::deviceID
    uint32_t driver_version;          // ::driverVersion
    uint8_t pipeline_cache_uuid[16];  // ::pipelineCacheUUID (VK_UUID_SIZE)
    uint64_t payload_size;            // bytes following this header
    uint64_t payload_fnv1a;           // FNV-1a 64 of the payload
};

// --- module state (single winning copy across fork TU + runtime archive) ----
WEAK char dng_vkpc_path[768] = "";
WEAK VkPipelineCache dng_vkpc_handle = VK_NULL_HANDLE;
WEAK VkDevice dng_vkpc_device = VK_NULL_HANDLE;
WEAK DngVkpcFileHeader dng_vkpc_header = {};
WEAK bool dng_vkpc_header_valid = false;   // header fields captured from props
WEAK bool dng_vkpc_dirty = false;          // pipelines created since last save
WEAK bool dng_vkpc_file_was_loaded = false;  // this session started from file
WEAK uint64_t dng_vkpc_loaded_bytes = 0;   // payload bytes accepted at load
WEAK ScopedSpinLock::AtomicFlag dng_vkpc_lock = 0;

WEAK bool dng_vkpc_verbose() {
    const char *v = getenv("DNG_PIPELINE_VERBOSE");
    return (v != nullptr) && (v[0] != '\0') && (v[0] != '0');
}

// Evidence logger. NOTE: StackPrinter does NOT print on destruction (only
// HeapPrinter does, via halide_print) — verified the hard way: the first
// on-device run produced zero [VkPCache] lines. This RAII wrapper streams
// into a stack buffer and writes it to STDERR on scope exit, matching where
// the pipeline's [Stage4-Perf]/[Warmup] evidence lines already go (harness
// logs capture stderr directly; halide_print would go to logcat on Android).
class DngVkpcLogger : public StackPrinter<StringStreamPrinterType, 256> {
public:
    explicit DngVkpcLogger(void *uc)
        : StackPrinter<StringStreamPrinterType, 256>(uc) {
        *this << "[VkPCache] ";
    }
    ~DngVkpcLogger() {
        write(STDERR_FILENO, str(), size());
    }
};
#define DNG_VKPC_LOG(uc) DngVkpcLogger(uc)

WEAK uint64_t dng_vkpc_fnv1a64(const uint8_t *data, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint64_t i = 0; i < n; i++) {
        h ^= (uint64_t)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Resolve the effective cache path. Returns nullptr when disabled.
// Must be called under dng_vkpc_lock.
WEAK const char *dng_vkpc_effective_path() {
    const char *kill = getenv("DNG_VK_PIPELINE_CACHE");
    if (kill != nullptr && kill[0] == '0' && kill[1] == '\0') {
        return nullptr;  // force-disabled
    }
    if (dng_vkpc_path[0] == '\0') {
        const char *env_path = getenv("DNG_VK_PIPELINE_CACHE_PATH");
        if (env_path != nullptr && env_path[0] != '\0') {
            size_t n = strlen(env_path);
            if (n >= sizeof(dng_vkpc_path)) {
                n = sizeof(dng_vkpc_path) - 1;
            }
            memcpy(dng_vkpc_path, env_path, n);
            dng_vkpc_path[n] = '\0';
        }
    }
    return (dng_vkpc_path[0] != '\0') ? dng_vkpc_path : nullptr;
}

// Read + validate the cache file. On success returns a malloc'd payload the
// caller must free() and sets *payload_size. On any failure returns nullptr
// (empty-cache fallback). Must be called under dng_vkpc_lock.
WEAK uint8_t *dng_vkpc_read_file(void *user_context, const char *path,
                                 const DngVkpcFileHeader &expect,
                                 uint64_t *payload_size) {
    *payload_size = 0;
    void *f = fopen(path, "rb");
    if (f == nullptr) {
        if (dng_vkpc_verbose()) {
            DNG_VKPC_LOG(user_context) << "load miss: no cache file at " << path << "\n";
        }
        return nullptr;
    }
    DngVkpcFileHeader hdr;
    uint8_t *payload = nullptr;
    const char *reject = nullptr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        reject = "short header";
    } else if (memcmp(hdr.magic, expect.magic, sizeof(hdr.magic)) != 0) {
        reject = "bad magic";
    } else if (hdr.header_version != expect.header_version) {
        reject = "header version mismatch";
    } else if (hdr.vendor_id != expect.vendor_id ||
               hdr.device_id != expect.device_id ||
               hdr.driver_version != expect.driver_version ||
               memcmp(hdr.pipeline_cache_uuid, expect.pipeline_cache_uuid, 16) != 0) {
        reject = "device/driver mismatch";
    } else if (hdr.payload_size == 0 || hdr.payload_size > (256ULL << 20)) {
        reject = "implausible payload size";
    } else {
        payload = (uint8_t *)malloc((size_t)hdr.payload_size);
        if (payload == nullptr) {
            reject = "payload alloc failed";
        } else if (fread(payload, 1, (size_t)hdr.payload_size, f) != (size_t)hdr.payload_size) {
            reject = "truncated payload";
        } else if (dng_vkpc_fnv1a64(payload, hdr.payload_size) != hdr.payload_fnv1a) {
            reject = "payload hash mismatch";
        }
    }
    fclose(f);
    if (reject != nullptr) {
        if (payload != nullptr) {
            free(payload);
        }
        if (dng_vkpc_verbose()) {
            DNG_VKPC_LOG(user_context) << "load miss: " << reject << " (" << path << ") — starting with empty cache\n";
        }
        return nullptr;
    }
    *payload_size = hdr.payload_size;
    return payload;
}

// Acquire (lazily creating) the persistent VkPipelineCache for the current
// device. Returns VK_NULL_HANDLE whenever the feature is disabled or any
// step failed — callers then behave exactly like upstream.
WEAK VkPipelineCache dng_vkpc_acquire(void *user_context, VulkanMemoryAllocator *allocator) {
    if (allocator == nullptr || vkCreatePipelineCache == nullptr ||
        vkGetPhysicalDeviceProperties == nullptr) {
        return VK_NULL_HANDLE;
    }
    ScopedSpinLock lock(&dng_vkpc_lock);
    const char *path = dng_vkpc_effective_path();
    if (path == nullptr) {
        return VK_NULL_HANDLE;
    }
    VkDevice device = allocator->current_device();
    if (device == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    if (dng_vkpc_handle != VK_NULL_HANDLE) {
        if (dng_vkpc_device == device) {
            return dng_vkpc_handle;
        }
        // Context was recreated behind our back; the old handle died with the
        // old device. Reset and recreate against the new device.
        dng_vkpc_handle = VK_NULL_HANDLE;
        dng_vkpc_device = VK_NULL_HANDLE;
        dng_vkpc_dirty = false;
        dng_vkpc_file_was_loaded = false;
        dng_vkpc_loaded_bytes = 0;
    }

    // Capture the invalidation key from the physical device.
    VkPhysicalDeviceProperties props;
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(allocator->current_physical_device(), &props);
    memcpy(dng_vkpc_header.magic, "DNGVKPC1", 8);
    dng_vkpc_header.header_version = 1;
    dng_vkpc_header.vendor_id = props.vendorID;
    dng_vkpc_header.device_id = props.deviceID;
    dng_vkpc_header.driver_version = props.driverVersion;
    memcpy(dng_vkpc_header.pipeline_cache_uuid, props.pipelineCacheUUID, 16);
    dng_vkpc_header.payload_size = 0;
    dng_vkpc_header.payload_fnv1a = 0;
    dng_vkpc_header_valid = true;

    uint64_t payload_size = 0;
    uint8_t *payload = dng_vkpc_read_file(user_context, path, dng_vkpc_header, &payload_size);

    VkPipelineCacheCreateInfo create_info;
    memset(&create_info, 0, sizeof(create_info));
    create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    create_info.initialDataSize = (size_t)payload_size;
    create_info.pInitialData = payload;

    VkPipelineCache handle = VK_NULL_HANDLE;
    VkResult result = vkCreatePipelineCache(device, &create_info, allocator->callbacks(), &handle);
    if (result != VK_SUCCESS && payload != nullptr) {
        // Driver rejected the initial data outright — retry with empty cache.
        create_info.initialDataSize = 0;
        create_info.pInitialData = nullptr;
        result = vkCreatePipelineCache(device, &create_info, allocator->callbacks(), &handle);
        payload_size = 0;
    }
    if (payload != nullptr) {
        free(payload);
    }
    if (result != VK_SUCCESS) {
        if (dng_vkpc_verbose()) {
            DNG_VKPC_LOG(user_context) << "vkCreatePipelineCache failed (" << (int)result << ") — feature dormant this session\n";
        }
        return VK_NULL_HANDLE;
    }
    dng_vkpc_handle = handle;
    dng_vkpc_device = device;
    dng_vkpc_file_was_loaded = (payload_size > 0);
    dng_vkpc_loaded_bytes = payload_size;
    if (dng_vkpc_verbose()) {
        if (dng_vkpc_file_was_loaded) {
            DNG_VKPC_LOG(user_context) << "load HIT: " << path << " payload=" << (uint64_t)payload_size << " bytes\n";
        } else {
            DNG_VKPC_LOG(user_context) << "created empty pipeline cache (path=" << path << ")\n";
        }
    }
    return dng_vkpc_handle;
}

// Mark that at least one pipeline was created against the persistent cache.
WEAK void dng_vkpc_mark_dirty() {
    ScopedSpinLock lock(&dng_vkpc_lock);
    dng_vkpc_dirty = true;
}

// Serialize the cache to disk (atomic: <path>.tmp + rename). No-op unless
// dirty. Never propagates errors. Must NOT be called under dng_vkpc_lock.
WEAK int dng_vkpc_save(void *user_context) {
    ScopedSpinLock lock(&dng_vkpc_lock);
    if (!dng_vkpc_dirty || dng_vkpc_handle == VK_NULL_HANDLE ||
        dng_vkpc_device == VK_NULL_HANDLE || !dng_vkpc_header_valid ||
        vkGetPipelineCacheData == nullptr) {
        return 0;  // nothing to do
    }
    const char *path = dng_vkpc_effective_path();
    if (path == nullptr) {
        return 0;
    }
    size_t payload_size = 0;
    VkResult result = vkGetPipelineCacheData(dng_vkpc_device, dng_vkpc_handle, &payload_size, nullptr);
    if (result != VK_SUCCESS || payload_size == 0) {
        if (dng_vkpc_verbose()) {
            DNG_VKPC_LOG(user_context) << "save skip: vkGetPipelineCacheData size query rc=" << (int)result << " size=" << (uint64_t)payload_size << "\n";
        }
        return -1;
    }
    uint8_t *payload = (uint8_t *)malloc(payload_size);
    if (payload == nullptr) {
        return -1;
    }
    result = vkGetPipelineCacheData(dng_vkpc_device, dng_vkpc_handle, &payload_size, payload);
    if (result != VK_SUCCESS) {
        free(payload);
        if (dng_vkpc_verbose()) {
            DNG_VKPC_LOG(user_context) << "save skip: vkGetPipelineCacheData rc=" << (int)result << "\n";
        }
        return -1;
    }

    DngVkpcFileHeader hdr = dng_vkpc_header;
    hdr.payload_size = (uint64_t)payload_size;
    hdr.payload_fnv1a = dng_vkpc_fnv1a64(payload, (uint64_t)payload_size);

    // Build "<path>.tmp" (bounded).
    char tmp_path[768 + 8];
    size_t path_len = strlen(path);
    if (path_len + 5 >= sizeof(tmp_path)) {
        free(payload);
        return -1;
    }
    memcpy(tmp_path, path, path_len);
    memcpy(tmp_path + path_len, ".tmp", 5);  // includes NUL

    bool ok = false;
    void *f = fopen(tmp_path, "wb");
    if (f != nullptr) {
        ok = (fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr)) &&
             (fwrite(payload, 1, payload_size, f) == payload_size);
        ok = (fclose(f) == 0) && ok;
        if (ok) {
            ok = (rename(tmp_path, path) == 0);
        }
        if (!ok) {
            remove(tmp_path);
        }
    }
    free(payload);
    if (!ok) {
        if (dng_vkpc_verbose()) {
            DNG_VKPC_LOG(user_context) << "save FAILED writing " << tmp_path << " (decode unaffected)\n";
        }
        return -1;
    }
    dng_vkpc_dirty = false;
    if (dng_vkpc_verbose()) {
        DNG_VKPC_LOG(user_context) << "save OK: " << path << " payload=" << (uint64_t)payload_size << " bytes\n";
    }
    return 0;
}

// Called from the forked halide_vulkan_device_release just before the VkDevice
// is destroyed: flush to disk, destroy the cache object, reset state.
WEAK void dng_vkpc_on_device_release(void *user_context, VulkanMemoryAllocator *allocator) {
    dng_vkpc_save(user_context);
    ScopedSpinLock lock(&dng_vkpc_lock);
    if (dng_vkpc_handle != VK_NULL_HANDLE && dng_vkpc_device != VK_NULL_HANDLE &&
        vkDestroyPipelineCache != nullptr) {
        const VkAllocationCallbacks *callbacks = allocator ? allocator->callbacks() : nullptr;
        vkDestroyPipelineCache(dng_vkpc_device, dng_vkpc_handle, callbacks);
    }
    dng_vkpc_handle = VK_NULL_HANDLE;
    dng_vkpc_device = VK_NULL_HANDLE;
    dng_vkpc_dirty = false;
    dng_vkpc_file_was_loaded = false;
    dng_vkpc_loaded_bytes = 0;
}

#undef DNG_VKPC_LOG

}  // namespace Vulkan
}  // namespace Internal
}  // namespace Runtime
}  // namespace Halide

#endif  // DNG_VK_PIPELINE_CACHE_PERSIST_H
