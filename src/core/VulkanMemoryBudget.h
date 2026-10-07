#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace spirula::vkmemory {

struct Budget {
    uint64_t budget_bytes = 0, usage_bytes = 0, available_bytes = 0;
};

inline Budget queryBudget(VkPhysicalDevice device) {
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(device, &properties);
    Budget out;
    for (uint32_t i = 0; i < properties.memoryProperties.memoryHeapCount; ++i) {
        if (!(properties.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        out.budget_bytes += budget.heapBudget[i];
        out.usage_bytes += budget.heapUsage[i];
        if (budget.heapBudget[i] > budget.heapUsage[i]) out.available_bytes += budget.heapBudget[i] - budget.heapUsage[i];
    }
    return out;
}

}  // namespace spirula::vkmemory
