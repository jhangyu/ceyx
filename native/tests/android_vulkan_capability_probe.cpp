#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

const char* yesNo(VkBool32 value) {
    return value ? "yes" : "no";
}

uint32_t versionMajor(uint32_t version) {
    return VK_VERSION_MAJOR(version);
}

uint32_t versionMinor(uint32_t version) {
    return VK_VERSION_MINOR(version);
}

uint32_t versionPatch(uint32_t version) {
    return VK_VERSION_PATCH(version);
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

}  // namespace

int main() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "dng_android_vulkan_capability_probe";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName = "ceyx";
    appInfo.engineVersion = 1;
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        std::cerr << "[ANDROID VULKAN PROBE] FAIL vkCreateInstance=" << result << "\n";
        return 2;
    }

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS || deviceCount == 0) {
        std::cerr << "[ANDROID VULKAN PROBE] FAIL no physical Vulkan device\n";
        vkDestroyInstance(instance, nullptr);
        return 2;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    auto getFeatures2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
    if (!getFeatures2) {
        getFeatures2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }

    bool anyCompatible = false;
    std::cout << "[ANDROID VULKAN PROBE] required target: "
              << "arm-64-android-vulkan-vk_int8-vk_int16-vk_int64-no_asserts-no_bounds_query\n";

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

        const bool computeQueue = hasComputeQueue(device);
        const bool supportsVkInt8 =
            float16Int8.shaderInt8 && storage8.storageBuffer8BitAccess;
        const bool supportsVkInt16 =
            features2.features.shaderInt16 && storage16.storageBuffer16BitAccess;
        const bool supportsVkInt64 = features2.features.shaderInt64;
        const bool compatible =
            computeQueue && supportsVkInt8 && supportsVkInt16 && supportsVkInt64;
        anyCompatible = anyCompatible || compatible;

        std::cout << "device[" << i << "]: " << props.deviceName << "\n";
        std::cout << "  apiVersion: "
                  << versionMajor(props.apiVersion) << "."
                  << versionMinor(props.apiVersion) << "."
                  << versionPatch(props.apiVersion) << "\n";
        std::cout << "  computeQueue: " << (computeQueue ? "yes" : "no") << "\n";
        std::cout << "  shaderInt8: " << yesNo(float16Int8.shaderInt8) << "\n";
        std::cout << "  storageBuffer8BitAccess: "
                  << yesNo(storage8.storageBuffer8BitAccess) << "\n";
        std::cout << "  shaderInt16: " << yesNo(features2.features.shaderInt16) << "\n";
        std::cout << "  storageBuffer16BitAccess: "
                  << yesNo(storage16.storageBuffer16BitAccess) << "\n";
        std::cout << "  shaderInt64: " << yesNo(features2.features.shaderInt64) << "\n";
        std::cout << "  targetCompatible: " << (compatible ? "yes" : "no") << "\n";
    }

    vkDestroyInstance(instance, nullptr);

    if (!anyCompatible) {
        std::cerr << "[ANDROID VULKAN PROBE] FAIL required Halide Vulkan target features missing\n";
        return 2;
    }

    std::cout << "[ANDROID VULKAN PROBE] PASS\n";
    return 0;
}
