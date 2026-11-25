#ifndef SAVAGEWIND_XR_FEATURE_GUARD_H
#define SAVAGEWIND_XR_FEATURE_GUARD_H
#include <stdbool.h>
#include <vulkan/vulkan.h>

// Bounded admission of the actual runtime-forwarded chain. Reject, never repair
// or silently strip required game features. A runtime retry is checked afresh.
static inline bool xr_vulkan_feature_chain_valid(const VkDeviceCreateInfo *info) {
    if (!info || info->enabledExtensionCount > 256 ||
        (info->enabledExtensionCount && !info->ppEnabledExtensionNames)) return false;
    VkStructureType seen[64];
    unsigned count = 0;
    bool v12 = false, timeline = false, features2 = false;
    for (const VkBaseInStructure *next = info->pNext; next; next = next->pNext) {
        if (count == 64) return false;
        for (unsigned i = 0; i < count; ++i) if (seen[i] == next->sType) return false;
        seen[count++] = next->sType;
        v12 |= next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        timeline |= next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        features2 |= next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    }
    return !(v12 && timeline) && !(features2 && info->pEnabledFeatures);
}
#endif
