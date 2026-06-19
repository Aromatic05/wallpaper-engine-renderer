#include "VulkanPass.hpp"

namespace wallpaper::vulkan
{

bool VulkanPass::canReuseForResidency(const VulkanPass& next_pass) const {
    const auto key = residencyKey();
    return !key.empty() && key == next_pass.residencyKey();
}

void VulkanPass::refreshResources(Scene& scene,
                                  const Device& device,
                                  RenderingResources& resources) {
    if (prepared()) {
        destory(device, resources);
    }
    prepare(scene, device, resources);
}

void VulkanPass::absorbResidencyGraphState(const VulkanPass& next_pass) {
    m_release_texs = next_pass.m_release_texs;
}

} // namespace wallpaper::vulkan
