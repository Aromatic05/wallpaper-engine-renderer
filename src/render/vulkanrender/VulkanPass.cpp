#include "VulkanPass.hpp"

namespace wallpaper::vulkan
{

bool VulkanPass::canReuseForResidency(const VulkanPass& next_pass) const {
    const auto key = residencyKey();
    return !key.empty() && key == next_pass.residencyKey();
}

} // namespace wallpaper::vulkan
