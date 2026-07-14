#include "render/vulkanrender/VulkanRender.hpp"
#include "wallpaper/VulkanOutputInit.hpp"
#include "wallpaper/swapchain/ExSwapchain.hpp"
#include "render/vulkan/include/vulkan/VulkanExSwapchain.hpp"

#include <drm/drm_fourcc.h>

#include <cassert>

int main() {
    wallpaper::vulkan::VulkanRender renderer;
    wallpaper::RenderInitInfo       info {};
    info.offscreen        = true;
    info.export_mode      = wallpaper::ExternalFrameExportMode::SHM;
    info.offscreen_tiling = wallpaper::TexTiling::OPTIMAL;
    info.width            = 640;
    info.height           = 360;

    assert(renderer.init(info));
    auto* initial = renderer.exSwapchain();
    assert(initial != nullptr);
    assert(initial->width() == 640);
    assert(initial->height() == 360);

    assert(renderer.resizeOutput(800, 450));
    auto* resized = renderer.exSwapchain();
    assert(resized != nullptr);
    assert(resized->width() == 800);
    assert(resized->height() == 450);

    assert(renderer.resizeOutput(800, 450));
    assert(renderer.exSwapchain() == resized);
    assert(! renderer.resizeOutput(0, 450));

    wallpaper::RenderInitInfo dynamicRejectedInfo     = info;
    dynamicRejectedInfo.width                         = 800;
    dynamicRejectedInfo.height                        = 450;
    dynamicRejectedInfo.export_mode                   = wallpaper::ExternalFrameExportMode::DMA_BUF;
    dynamicRejectedInfo.offscreen_tiling              = wallpaper::TexTiling::LINEAR;
    dynamicRejectedInfo.allow_shm_fallback            = false;
    dynamicRejectedInfo.consumer_dmabuf_formats_known = true;
    dynamicRejectedInfo.consumer_dmabuf_formats       = {
        { DRM_FORMAT_NV12, DRM_FORMAT_MOD_LINEAR },
    };
    auto* outputBeforeRejectedReconfigure = renderer.exSwapchain();
    assert(! renderer.reconfigureOutput(dynamicRejectedInfo));
    assert(renderer.exSwapchain() == outputBeforeRejectedReconfigure);

    wallpaper::RenderInitInfo dynamicFallbackInfo = dynamicRejectedInfo;
    dynamicFallbackInfo.allow_shm_fallback        = true;
    assert(renderer.reconfigureOutput(dynamicFallbackInfo));
    assert(renderer.exSwapchain() != nullptr);

    renderer.destroy();

    wallpaper::RenderInitInfo rejectedInfo {};
    rejectedInfo.offscreen                     = true;
    rejectedInfo.export_mode                   = wallpaper::ExternalFrameExportMode::DMA_BUF;
    rejectedInfo.offscreen_tiling              = wallpaper::TexTiling::LINEAR;
    rejectedInfo.allow_shm_fallback            = false;
    rejectedInfo.consumer_dmabuf_formats_known = true;
    rejectedInfo.consumer_dmabuf_formats       = {
        { DRM_FORMAT_NV12, DRM_FORMAT_MOD_LINEAR },
    };
    rejectedInfo.width  = 64;
    rejectedInfo.height = 64;

    wallpaper::vulkan::VulkanRender rejectedRenderer;
    assert(! rejectedRenderer.init(rejectedInfo));
    rejectedRenderer.destroy();

    wallpaper::RenderInitInfo fallbackInfo = rejectedInfo;
    fallbackInfo.allow_shm_fallback        = true;
    wallpaper::vulkan::VulkanRender fallbackRenderer;
    assert(fallbackRenderer.init(fallbackInfo));
    assert(fallbackRenderer.exSwapchain() != nullptr);
    fallbackRenderer.destroy();

    // AMD DCC modifiers expose metadata as additional DRM memory planes. When this exact modifier
    // is supported by the current GPU, the public DMA-BUF contract must describe every plane rather
    // than flattening the image into a single COLOR subresource.
    constexpr std::uint64_t kAmdDccModifier = 0x020000000056bb03ULL;
    wallpaper::RenderInitInfo dccInfo {};
    dccInfo.offscreen                     = true;
    dccInfo.export_mode                   = wallpaper::ExternalFrameExportMode::DMA_BUF;
    dccInfo.offscreen_tiling              = wallpaper::TexTiling::LINEAR;
    dccInfo.allow_shm_fallback            = false;
    dccInfo.consumer_dmabuf_formats_known = true;
    dccInfo.consumer_dmabuf_formats       = {
        { DRM_FORMAT_ABGR8888, kAmdDccModifier },
    };
    dccInfo.width  = 2560;
    dccInfo.height = 1600;

    wallpaper::vulkan::VulkanRender dccRenderer;
    if (dccRenderer.init(dccInfo)) {
        auto* dccSwapchain = dynamic_cast<wallpaper::vulkan::VulkanExSwapchain*>(
            dccRenderer.exSwapchain());
        assert(dccSwapchain != nullptr);
        for (const auto& handle : dccSwapchain->handles()) {
            assert(handle.image.drm_modifier == kAmdDccModifier);
            assert(handle.image.n_planes > 1);
            for (std::uint32_t plane = 0; plane < handle.image.n_planes; ++plane) {
                assert(handle.image.planes[plane].fd >= 0);
                assert(handle.image.planes[plane].stride > 0);
            }
        }
        dccRenderer.destroy();
    }
    return 0;
}
