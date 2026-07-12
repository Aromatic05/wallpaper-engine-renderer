#pragma once

#include "swapchain/ExSwapchain.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace wallpaper
{
using ReDrawCB = std::function<void()>;

enum class VulkanDevicePreference {
    Default,
    PreferIntegrated,
    PreferDiscrete,
};

enum class GpuPipelinePreference {
    Nvidia,
    NvidiaStateless,
    Va,
};

struct VulkanSurfaceInfo {
    std::function<VkResult(VkInstance, VkSurfaceKHR*)> createSurfaceOp;
    std::vector<std::string>                           instanceExts;
};

struct RenderInitInfo {
    bool enable_valid_layer { false };
    bool offscreen { false };
    ExternalFrameExportMode export_mode { ExternalFrameExportMode::OPAQUE_FD };
    bool allow_shm_fallback { false };

    std::span<const std::uint8_t> uuid;
    VulkanDevicePreference        device_preference { VulkanDevicePreference::Default };
    GpuPipelinePreference         gpu_pipeline_preference { GpuPipelinePreference::Nvidia };
    TexTiling                     offscreen_tiling { TexTiling::OPTIMAL };
    std::uint32_t                 export_drm_fourcc { 0 };
    std::vector<std::uint64_t>    export_drm_modifiers;
    VulkanSurfaceInfo             surface_info;

    std::uint16_t width { 1920 };
    std::uint16_t height { 1080 };
    double        render_scale { 1.0 };
    std::uint32_t msaa_samples { 1 };
    ReDrawCB      redraw_callback;
};
} // namespace wallpaper
