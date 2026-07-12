#include "render/vulkanrender/VulkanRender.hpp"
#include "wallpaper/VulkanOutputInit.hpp"
#include "wallpaper/swapchain/ExSwapchain.hpp"

#include <cassert>

int main() {
    wallpaper::vulkan::VulkanRender renderer;
    wallpaper::RenderInitInfo info {};
    info.offscreen = true;
    info.export_mode = wallpaper::ExternalFrameExportMode::SHM;
    info.offscreen_tiling = wallpaper::TexTiling::OPTIMAL;
    info.width = 640;
    info.height = 360;

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

    renderer.destroy();
    return 0;
}
