// VulkanPrime.cpp -- see VulkanPrime.h.

#include "app/gui/VulkanPrime.h"

#if defined(_WIN32) && (defined(SS_BACKEND_VULKAN) || defined(SS_BUILD_SAM))
#include "core/Env.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <vector>
#endif

namespace gui {

#if defined(_WIN32) && (defined(SS_BACKEND_VULKAN) || defined(SS_BUILD_SAM))

// ~170 ms on an RTX 3070 laptop, 130 of it vkCreateInstance -- so counting the
// GPUs first to skip single-GPU machines would save little.
void prime_vulkan_driver() {
    const char* mode = spirula::env("GUI_VK_PRIME");
    if (mode && std::strcmp(mode, "0") == 0) return;
    const bool keep = mode && std::strcmp(mode, "keep") == 0;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    // Never destroyed: that would unload the driver DLL before GL loads it,
    // and with it whatever the first device set up.
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return;

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance, &n, gpus.data());
    gpus.resize(n);

    const float priority = 1.0f;
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu, &props);
        if (props.vendorID != 0x10DE) continue;  // NVIDIA's PCI vendor ID
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = 0;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        VkDevice device = VK_NULL_HANDLE;
        if (vkCreateDevice(gpu, &dci, nullptr, &device) == VK_SUCCESS && !keep)
            vkDestroyDevice(device, nullptr);
    }
}

#else

void prime_vulkan_driver() {}

#endif

}  // namespace gui
