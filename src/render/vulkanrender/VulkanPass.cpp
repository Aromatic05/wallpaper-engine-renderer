#include "VulkanPass.hpp"

#include "vulkan/Device.hpp"
#include "vulkan/TextureCache.hpp"

using namespace wallpaper::vulkan;

bool VulkanPass::canReuseForResidency(const VulkanPass& next_pass) const {
    const auto key = residencyKey();
    return !key.empty() && key == next_pass.residencyKey();
}

void VulkanPass::releaseFinalReadTexs(const Device& device) const {
    for (const auto& tex : releaseTexs()) {
        // A pass-local release key means this pass is the render graph's final reader for that
        // logical render target. Centralizing the cache handoff here keeps each concrete pass
        // focused on recording its own GPU work while sharing the same lifetime boundary logic.
        device.tex_cache().MarkShareReady(tex);
    }
}
