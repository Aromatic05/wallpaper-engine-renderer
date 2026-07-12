#pragma once

#include <array>
#include <cstdint>
#include <utility>

#include <vulkan/vulkan.h>

namespace wallpaper::vulkan
{

struct FinalOutputAttachmentPlan {
    VkSampleCountFlagBits sample_count { VK_SAMPLE_COUNT_1_BIT };
    std::uint32_t         attachment_count { 1 };
    std::uint32_t         color_attachment { 0 };
    std::uint32_t         resolve_attachment { VK_ATTACHMENT_UNUSED };
    bool                  uses_resolve { false };
};

inline VkSampleCountFlagBits ResolveFinalOutputSampleCount(
    std::uint32_t requested_samples,
    VkSampleCountFlags supported_samples,
    bool sample_rate_shading_enabled = true) noexcept {
    if (! sample_rate_shading_enabled || requested_samples <= 1) {
        return VK_SAMPLE_COUNT_1_BIT;
    }

    constexpr std::array candidates {
        std::pair { 64u, VK_SAMPLE_COUNT_64_BIT },
        std::pair { 32u, VK_SAMPLE_COUNT_32_BIT },
        std::pair { 16u, VK_SAMPLE_COUNT_16_BIT },
        std::pair { 8u, VK_SAMPLE_COUNT_8_BIT },
        std::pair { 4u, VK_SAMPLE_COUNT_4_BIT },
        std::pair { 2u, VK_SAMPLE_COUNT_2_BIT },
        std::pair { 1u, VK_SAMPLE_COUNT_1_BIT },
    };
    for (const auto& [count, flag] : candidates) {
        if (count <= requested_samples &&
            (supported_samples & static_cast<VkSampleCountFlags>(flag)) != 0u) {
            return flag;
        }
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

inline VkPipelineMultisampleStateCreateInfo BuildFinalOutputMultisampleState(
    VkSampleCountFlagBits sample_count) noexcept {
    const bool enabled = sample_count != VK_SAMPLE_COUNT_1_BIT;
    return VkPipelineMultisampleStateCreateInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = enabled ? sample_count : VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = enabled ? VK_TRUE : VK_FALSE,
        .minSampleShading = enabled ? 1.0f : 0.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };
}

inline FinalOutputAttachmentPlan BuildFinalOutputAttachmentPlan(
    VkSampleCountFlagBits sample_count) noexcept {
    const bool uses_resolve = sample_count != VK_SAMPLE_COUNT_1_BIT;
    return FinalOutputAttachmentPlan {
        .sample_count = uses_resolve ? sample_count : VK_SAMPLE_COUNT_1_BIT,
        .attachment_count = uses_resolve ? 2u : 1u,
        .color_attachment = 0,
        .resolve_attachment = uses_resolve ? 1u : VK_ATTACHMENT_UNUSED,
        .uses_resolve = uses_resolve,
    };
}

} // namespace wallpaper::vulkan
