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

struct VulkanSurfaceInfo {
    std::function<VkResult(VkInstance, VkSurfaceKHR*)> createSurfaceOp;
    std::vector<std::string>                           instanceExts;
};

struct RenderInitInfo {
    bool enable_valid_layer { false };
    bool offscreen { false };

    std::span<const std::uint8_t> uuid;
    TexTiling                     offscreen_tiling { TexTiling::OPTIMAL };
    VulkanSurfaceInfo             surface_info;

    std::uint16_t width { 1920 };
    std::uint16_t height { 1080 };
    ReDrawCB      redraw_callback;
};
} // namespace wallpaper
