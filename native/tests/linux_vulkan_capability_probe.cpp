// linux_vulkan_capability_probe.cpp — standalone Linux Vulkan capability gate.
//
// Purpose (spec A8, plan T4): answer ONE question before any decode gate runs —
// does this runner's Vulkan ICD satisfy the strict AOT contract
//   host-vulkan-vk_int8-vk_int16-vk_int64-no_asserts-no_bounds_query
// that halide_aot.cmake selects on Linux? Because this binary links neither
// dng_decoder_native nor any AOT kernel (plan T4 criterion 4), a non-zero exit
// here is attributable to the *device*, never to the pipeline.
//
// Structure mirrors native/tests/android_vulkan_capability_probe.cpp. The
// output contract is deliberately stricter than the Android probe's: every
// capability is emitted as a machine-parseable
//     FEATURE <name> = <0|1>
// line, and every unmet requirement as
//     MISSING <name>
// so the CI artifact is self-describing (spec A8 "print its own result line").
//
// Halide's vk_int8 / vk_int16 need more than the shader feature bit: the
// corresponding storage extension must also be present, or the generated SPIR-V
// cannot address 8/16-bit values in buffers. Both halves are therefore
// required, and reported separately so a partial ICD is diagnosable.
//
// Forced-failure injection (plan T4 criterion 3): setting
//     DNG_PROBE_FORCE_MISSING=<FEATURE name>[,<FEATURE name>...]
// clears those feature bits after they are read, which exercises the failure
// path (MISSING line + non-zero exit) on a runner whose ICD is fully capable.
// The injection is announced on stdout so an injected run can never be mistaken
// for a genuine one.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char* kRequiredAotTarget =
    "host-vulkan-vk_int8-vk_int16-vk_int64-no_asserts-no_bounds_query";

// A single reported capability. `required` marks the ones the AOT contract
// depends on; non-required entries are printed for diagnosis only.
struct Capability {
    const char* name;
    bool present;
    bool required;
};

bool forcedMissing(const char* name) {
    const char* raw = std::getenv("DNG_PROBE_FORCE_MISSING");
    if (raw == nullptr) {
        return false;
    }
    const std::string list(raw);
    const std::string needle(name);
    std::string::size_type pos = 0;
    while (pos <= list.size()) {
        const std::string::size_type comma = list.find(',', pos);
        const std::string::size_type end =
            (comma == std::string::npos) ? list.size() : comma;
        if (list.compare(pos, end - pos, needle) == 0) {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return false;
}

bool hasComputeQueue(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    if (count > 0) {
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, queues.data());
    }
    for (const VkQueueFamilyProperties& queue : queues) {
        if ((queue.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            return true;
        }
    }
    return false;
}

bool hasDeviceExtension(VkPhysicalDevice device, const char* extension) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> props(count);
    if (count > 0 &&
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data()) != VK_SUCCESS) {
        return false;
    }
    for (const VkExtensionProperties& prop : props) {
        if (std::strcmp(prop.extensionName, extension) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    std::cout << "PROBE linux_vulkan_capability\n";
    std::cout << "REQUIRED_AOT_TARGET " << kRequiredAotTarget << "\n";
    if (const char* forced = std::getenv("DNG_PROBE_FORCE_MISSING")) {
        std::cout << "INJECTED_FAILURE DNG_PROBE_FORCE_MISSING=" << forced << "\n";
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "dng_linux_vulkan_capability_probe";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName = "ceyx";
    appInfo.engineVersion = 1;
    // 1.1 so vkGetPhysicalDeviceFeatures2 is core; the KHR fallback below still
    // covers a 1.0 loader that exposes the extension entry point.
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        std::cout << "MISSING vulkan_instance\n";
        std::cerr << "[LINUX VULKAN PROBE] FAIL vkCreateInstance=" << result
                  << " (no usable ICD; install libvulkan1 + a driver)\n";
        return 2;
    }

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS || deviceCount == 0) {
        std::cout << "MISSING vulkan_physical_device\n";
        std::cerr << "[LINUX VULKAN PROBE] FAIL no physical Vulkan device\n";
        vkDestroyInstance(instance, nullptr);
        return 2;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
    if (!getFeatures2) {
        getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }

    int compatibleIndex = -1;
    // Capabilities of the last device inspected, kept so the MISSING report
    // below names what the runner actually lacks rather than a generic failure.
    std::vector<Capability> lastCaps;

    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDevice device = devices[i];
        VkPhysicalDeviceProperties props{};
        VkPhysicalDeviceFeatures baseFeatures{};
        vkGetPhysicalDeviceProperties(device, &props);
        vkGetPhysicalDeviceFeatures(device, &baseFeatures);

        VkPhysicalDeviceShaderFloat16Int8Features float16Int8{};
        float16Int8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;

        VkPhysicalDevice8BitStorageFeatures storage8{};
        storage8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES;
        storage8.pNext = &float16Int8;

        VkPhysicalDevice16BitStorageFeatures storage16{};
        storage16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
        storage16.pNext = &storage8;

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &storage16;
        features2.features = baseFeatures;

        if (getFeatures2) {
            getFeatures2(device, &features2);
        }

        std::cout << "DEVICE " << i << " name=" << props.deviceName
                  << " apiVersion=" << VK_VERSION_MAJOR(props.apiVersion) << "."
                  << VK_VERSION_MINOR(props.apiVersion) << "."
                  << VK_VERSION_PATCH(props.apiVersion)
                  << " driverVersion=" << props.driverVersion
                  << " vendorID=0x" << std::hex << props.vendorID << std::dec << "\n";

        std::vector<Capability> caps = {
            {"computeQueue", hasComputeQueue(device), true},
            {"shaderInt8", float16Int8.shaderInt8 != VK_FALSE, true},
            {"storageBuffer8BitAccess", storage8.storageBuffer8BitAccess != VK_FALSE, true},
            {"VK_KHR_8bit_storage",
             hasDeviceExtension(device, VK_KHR_8BIT_STORAGE_EXTENSION_NAME), true},
            {"shaderInt16", features2.features.shaderInt16 != VK_FALSE, true},
            {"storageBuffer16BitAccess", storage16.storageBuffer16BitAccess != VK_FALSE, true},
            {"VK_KHR_16bit_storage",
             hasDeviceExtension(device, VK_KHR_16BIT_STORAGE_EXTENSION_NAME), true},
            {"shaderInt64", features2.features.shaderInt64 != VK_FALSE, true},
        };

        bool compatible = true;
        for (Capability& cap : caps) {
            if (forcedMissing(cap.name)) {
                cap.present = false;
            }
            std::cout << "FEATURE " << cap.name << " = " << (cap.present ? 1 : 0) << "\n";
            if (cap.required && !cap.present) {
                compatible = false;
            }
        }
        std::cout << "DEVICE_COMPATIBLE " << i << " = " << (compatible ? 1 : 0) << "\n";

        lastCaps = caps;
        if (compatible && compatibleIndex < 0) {
            compatibleIndex = static_cast<int>(i);
        }
    }

    vkDestroyInstance(instance, nullptr);

    if (compatibleIndex < 0) {
        for (const Capability& cap : lastCaps) {
            if (cap.required && !cap.present) {
                std::cout << "MISSING " << cap.name << "\n";
            }
        }
        std::cout << "RESULT FAIL\n";
        std::cerr << "[LINUX VULKAN PROBE] FAIL required Halide Vulkan target features missing\n";
        return 2;
    }

    std::cout << "SELECTED_DEVICE " << compatibleIndex << "\n";
    std::cout << "RESULT PASS\n";
    return 0;
}
